/* gstmdpoverlay — GStreamer video sink: display linear NV12 straight to the panel via the MSM **V4L2
 * video-out overlay** (/dev/video0). This is the interface the LEGACY webOS video sink used
 * (libPmMediaGstVideoSinkLib stage b) — it owns a RESERVED MDP VG (video) pipe (kernel msm_fb.c layout
 * <VG1><VG2><RGB1>, fb0=RGB1), so it gets a video pipe WITHOUT contending for the fb0 MSMFB_OVERLAY pool
 * that LunaSysMgr's compositor exhausts. Zero GPU readback, zero WPE compositing (hole-punch).
 *
 * Pipeline: omxh264dec ! mdpdetile ! mdpoverlaysink   (mdpdetile emits linear NV12 = MDP_Y_CBCR_H2V2).
 * Dest on-screen rect via props dst-x/dst-y/dst-w/dst-h; env ATLAS_OV_DST="x,y,w,h" overrides. NOTE the
 * V4L2 fourcc naming is swapped vs MDP: V4L2_PIX_FMT_NV21 -> MDP_Y_CBCR_H2V2 (Cb-first, mdpdetile's
 * output), so we pass NV21 (override via ATLAS_OV_FMT fourcc int).
 *
 * v1: one memcpy/frame into a pmem_adsp buffer, then QBUF it (userptr = pmem vaddr, reserved = pmem fd,
 * exactly as the driver's msmv4l2_fb_update expects). Zero-copy (QBUF mdpdetile's dst pmem) is later.
 */
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideosink.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <linux/videodev2.h>

GST_DEBUG_CATEGORY_STATIC (mdpoverlay_debug);
#define GST_CAT_DEFAULT mdpoverlay_debug

#define PMEM_ALLOCATE _IOW('p', 5, unsigned int)
#ifndef V4L2_PIX_FMT_NV21
#define V4L2_PIX_FMT_NV21 v4l2_fourcc('N','V','2','1')
#endif

#define GST_TYPE_MDPOVERLAY (gst_mdpoverlay_get_type())
G_DECLARE_FINAL_TYPE (GstMdpOverlay, gst_mdpoverlay, GST, MDPOVERLAY, GstVideoSink)

struct _GstMdpOverlay {
    GstVideoSink parent;
    gint dstx, dsty, dstw, dsth;   /* on-screen destination rect (properties) */
    gint vw, vh;                   /* incoming video (source) width/height */
    guint fourcc;                  /* V4L2 pixelformat (default NV21 -> MDP_Y_CBCR_H2V2) */
    gsize fsz;                     /* pmem frame size = vw*vh*3/2 */
    gint vid_fd, pmem_fd;
    void *pmem_map;
    gboolean streaming;
};
G_DEFINE_TYPE (GstMdpOverlay, gst_mdpoverlay, GST_TYPE_VIDEO_SINK)

enum { PROP_0, PROP_DST_X, PROP_DST_Y, PROP_DST_W, PROP_DST_H };

static GstStaticPadTemplate sink_tmpl = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, format=(string)NV12, "
        "width=(int)[1,4096], height=(int)[1,4096], framerate=(fraction)[0/1,MAX]"));

static int pmem_alloc (gsize sz, void **map) {
    int fd = open ("/dev/pmem_adsp", O_RDWR);
    if (fd < 0) return -1;
    if (ioctl (fd, PMEM_ALLOCATE, sz) < 0) { close (fd); return -1; }
    void *p = mmap (0, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close (fd); return -1; }
    *map = p; return fd;
}

static void overlay_teardown (GstMdpOverlay *self) {
    if (self->vid_fd >= 0 && self->streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ioctl (self->vid_fd, VIDIOC_STREAMOFF, &type);
    }
    self->streaming = FALSE;
    if (self->pmem_map) munmap (self->pmem_map, self->fsz), self->pmem_map = NULL;
    if (self->pmem_fd >= 0) close (self->pmem_fd), self->pmem_fd = -1;
    if (self->vid_fd >= 0) close (self->vid_fd), self->vid_fd = -1;
}

static gboolean gst_mdpoverlay_set_caps (GstBaseSink *bsink, GstCaps *caps) {
    GstMdpOverlay *self = GST_MDPOVERLAY (bsink);
    GstVideoInfo info;
    if (!gst_video_info_from_caps (&info, caps)) return FALSE;
    overlay_teardown (self);

    self->vw = GST_VIDEO_INFO_WIDTH (&info);
    self->vh = GST_VIDEO_INFO_HEIGHT (&info);
    self->fsz = (gsize) self->vw * self->vh * 3 / 2;

    const char *e = getenv ("ATLAS_OV_DST");
    if (e) { int x,y,w,h; if (sscanf (e, "%d,%d,%d,%d", &x,&y,&w,&h) == 4) {
        self->dstx=x; self->dsty=y; self->dstw=w; self->dsth=h; } }
    { const char *ef = getenv ("ATLAS_OV_FMT"); if (ef) self->fourcc = (guint) strtoul (ef, NULL, 0); }

    self->pmem_fd = pmem_alloc (self->fsz, &self->pmem_map);
    self->vid_fd = open ("/dev/video0", O_RDWR);
    if (self->pmem_fd < 0 || self->vid_fd < 0) {
        GST_ERROR_OBJECT (self, "pmem/video0 open failed (pmem=%d vid=%d): %s",
            self->pmem_fd, self->vid_fd, g_strerror (errno));
        overlay_teardown (self); return FALSE;
    }

    /* source pixel format (V4L2_BUF_TYPE_VIDEO_OUTPUT) */
    struct v4l2_format fmt; memset (&fmt, 0, sizeof fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = self->vw; fmt.fmt.pix.height = self->vh;
    fmt.fmt.pix.pixelformat = self->fourcc;
    if (ioctl (self->vid_fd, VIDIOC_S_FMT, &fmt) < 0) {
        GST_ERROR_OBJECT (self, "S_FMT(OUTPUT) failed: %s", g_strerror (errno)); overlay_teardown (self); return FALSE; }

    /* source crop = whole frame */
    struct v4l2_crop crop; memset (&crop, 0, sizeof crop);
    crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    crop.c.left = 0; crop.c.top = 0; crop.c.width = self->vw; crop.c.height = self->vh;
    if (ioctl (self->vid_fd, VIDIOC_S_CROP, &crop) < 0)
        GST_WARNING_OBJECT (self, "S_CROP failed: %s", g_strerror (errno));   /* non-fatal */

    /* destination on-screen window (V4L2_BUF_TYPE_VIDEO_OVERLAY) */
    struct v4l2_format win; memset (&win, 0, sizeof win);
    win.type = V4L2_BUF_TYPE_VIDEO_OVERLAY;
    win.fmt.win.w.left = self->dstx; win.fmt.win.w.top = self->dsty;
    win.fmt.win.w.width = self->dstw; win.fmt.win.w.height = self->dsth;
    if (ioctl (self->vid_fd, VIDIOC_S_FMT, &win) < 0) {
        GST_ERROR_OBJECT (self, "S_FMT(OVERLAY) failed: %s", g_strerror (errno)); overlay_teardown (self); return FALSE; }

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl (self->vid_fd, VIDIOC_STREAMON, &type) < 0) {
        GST_ERROR_OBJECT (self, "STREAMON failed: %s", g_strerror (errno)); overlay_teardown (self); return FALSE; }
    self->streaming = TRUE;
    GST_INFO_OBJECT (self, "V4L2 overlay: src %dx%d fourcc=0x%x -> win %d,%d %dx%d (/dev/video0)",
        self->vw, self->vh, self->fourcc, self->dstx, self->dsty, self->dstw, self->dsth);
    return TRUE;
}

static GstFlowReturn gst_mdpoverlay_show_frame (GstVideoSink *vsink, GstBuffer *buf) {
    GstMdpOverlay *self = GST_MDPOVERLAY (vsink);
    if (!self->streaming) return GST_FLOW_OK;
    GstMapInfo m;
    if (!gst_buffer_map (buf, &m, GST_MAP_READ)) return GST_FLOW_ERROR;
    memcpy (self->pmem_map, m.data, MIN (m.size, self->fsz));   /* v1: copy into pmem */
    gst_buffer_unmap (buf, &m);

    struct v4l2_buffer vb; memset (&vb, 0, sizeof vb);
    vb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    vb.memory = V4L2_MEMORY_USERPTR;
    vb.m.userptr = (unsigned long) self->pmem_map;   /* pmem vaddr -> driver resolves offset via VMA */
    vb.length = self->fsz;
    vb.reserved = (uint32_t) self->pmem_fd;           /* msm driver: reserved = the pmem fd */
    if (ioctl (self->vid_fd, VIDIOC_QBUF, &vb) < 0)
        GST_WARNING_OBJECT (self, "QBUF failed: %s", g_strerror (errno));   /* non-fatal — keep streaming */
    return GST_FLOW_OK;
}

static gboolean gst_mdpoverlay_stop (GstBaseSink *bsink) {
    overlay_teardown (GST_MDPOVERLAY (bsink));
    return TRUE;
}

static void gst_mdpoverlay_set_property (GObject *o, guint id, const GValue *v, GParamSpec *ps) {
    GstMdpOverlay *self = GST_MDPOVERLAY (o);
    switch (id) {
        case PROP_DST_X: self->dstx = g_value_get_int (v); break;
        case PROP_DST_Y: self->dsty = g_value_get_int (v); break;
        case PROP_DST_W: self->dstw = g_value_get_int (v); break;
        case PROP_DST_H: self->dsth = g_value_get_int (v); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, ps);
    }
}
static void gst_mdpoverlay_get_property (GObject *o, guint id, GValue *v, GParamSpec *ps) {
    GstMdpOverlay *self = GST_MDPOVERLAY (o);
    switch (id) {
        case PROP_DST_X: g_value_set_int (v, self->dstx); break;
        case PROP_DST_Y: g_value_set_int (v, self->dsty); break;
        case PROP_DST_W: g_value_set_int (v, self->dstw); break;
        case PROP_DST_H: g_value_set_int (v, self->dsth); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, ps);
    }
}

static void gst_mdpoverlay_init (GstMdpOverlay *self) {
    self->vid_fd = self->pmem_fd = -1;
    self->dstx = 0; self->dsty = 0; self->dstw = 1024; self->dsth = 768;   /* default fullscreen */
    self->fourcc = V4L2_PIX_FMT_NV21;   /* -> MDP_Y_CBCR_H2V2 (Cb-first, mdpdetile output) */
}

static void gst_mdpoverlay_class_init (GstMdpOverlayClass *klass) {
    GObjectClass *gc = G_OBJECT_CLASS (klass);
    GstElementClass *ec = GST_ELEMENT_CLASS (klass);
    GstBaseSinkClass *bc = GST_BASE_SINK_CLASS (klass);
    GstVideoSinkClass *vc = GST_VIDEO_SINK_CLASS (klass);
    gc->set_property = gst_mdpoverlay_set_property;
    gc->get_property = gst_mdpoverlay_get_property;
    g_object_class_install_property (gc, PROP_DST_X,
        g_param_spec_int ("dst-x", "dst-x", "overlay x", 0, 4096, 0, G_PARAM_READWRITE));
    g_object_class_install_property (gc, PROP_DST_Y,
        g_param_spec_int ("dst-y", "dst-y", "overlay y", 0, 4096, 0, G_PARAM_READWRITE));
    g_object_class_install_property (gc, PROP_DST_W,
        g_param_spec_int ("dst-w", "dst-w", "overlay width", 1, 4096, 1024, G_PARAM_READWRITE));
    g_object_class_install_property (gc, PROP_DST_H,
        g_param_spec_int ("dst-h", "dst-h", "overlay height", 1, 4096, 768, G_PARAM_READWRITE));
    gst_element_class_add_static_pad_template (ec, &sink_tmpl);
    gst_element_class_set_static_metadata (ec, "MDP4 V4L2 HW overlay sink",
        "Sink/Video", "Display linear NV12 via the MSM V4L2 video-out overlay (/dev/video0)", "Atlas TouchPad");
    bc->set_caps = gst_mdpoverlay_set_caps;
    bc->stop = gst_mdpoverlay_stop;
    vc->show_frame = gst_mdpoverlay_show_frame;
}

static gboolean plugin_init (GstPlugin *plugin) {
    GST_DEBUG_CATEGORY_INIT (mdpoverlay_debug, "mdpoverlay", 0, "MDP4 V4L2 HW overlay sink");
    return gst_element_register (plugin, "mdpoverlaysink", GST_RANK_NONE, GST_TYPE_MDPOVERLAY);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, mdpoverlay,
    "MDP4 V4L2 HW overlay video sink for Atlas TouchPad",
    plugin_init, "1.0", "LGPL", "atlas", "atlas")
