/* gstmdpoverlay — GStreamer video sink: display linear NV12 straight to the panel via an MDP4 hardware
 * overlay plane (/dev/fb0 MSMFB_OVERLAY_SET/PLAY). Zero GPU readback, zero WPE compositing — the MDP
 * composites the video buffer over the UI base layer (hole-punch). For the Atlas/WPE FULLSCREEN video
 * fast path on the HP TouchPad (APQ8060). Recipe validated standalone (mdp_overlay_test.c).
 *
 * Pipeline: omxh264dec ! mdpdetile ! mdpoverlaysink   (mdpdetile emits linear NV12 = MDP_Y_CBCR_H2V2).
 * The destination on-screen rect is set via properties dst-x/dst-y/dst-w/dst-h (WPE sets the fullscreen
 * rect); env ATLAS_OV_DST="x,y,w,h" overrides for standalone testing. Default = full 1024x768.
 *
 * v1: one memcpy/frame into a pmem_adsp buffer, then OVERLAY_PLAY that fd (cheap vs the readback it
 * replaces). Zero-copy (PLAY mdpdetile's dst pmem directly) is a later optimization.
 */
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideosink.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

GST_DEBUG_CATEGORY_STATIC (mdpoverlay_debug);
#define GST_CAT_DEFAULT mdpoverlay_debug

/* --- kernel ABI (include/linux/msm_mdp.h + android_pmem.h) --- */
struct mdp_rect  { uint32_t x, y, w, h; };
struct msmfb_img { uint32_t width, height, format; };
struct mdp_overlay {
    struct msmfb_img src;
    struct mdp_rect  src_rect;
    struct mdp_rect  dst_rect;
    uint32_t z_order, is_fg, alpha, transp_mask, flags, id;
    uint32_t user_data[8];
};
struct msmfb_data { uint32_t offset; int memory_id; int id; uint32_t flags; uint32_t priv; };
struct msmfb_overlay_data { uint32_t id; struct msmfb_data data; };

#define MSMFB_OVERLAY_SET   _IOWR('m', 135, struct mdp_overlay)
#define MSMFB_OVERLAY_UNSET _IOW ('m', 136, unsigned int)
#define MSMFB_OVERLAY_PLAY  _IOW ('m', 137, struct msmfb_overlay_data)
#define MSMFB_NEW_REQUEST   ((uint32_t)-1)
#define MDP_Y_CBCR_H2V2     2      /* NV12 (Cb-first) — mdpdetile output */
#define PMEM_ALLOCATE       _IOW('p', 5, unsigned int)

#define GST_TYPE_MDPOVERLAY (gst_mdpoverlay_get_type())
G_DECLARE_FINAL_TYPE (GstMdpOverlay, gst_mdpoverlay, GST, MDPOVERLAY, GstVideoSink)

struct _GstMdpOverlay {
    GstVideoSink parent;
    gint dstx, dsty, dstw, dsth;   /* on-screen destination rect (properties) */
    gint vw, vh;                   /* incoming video (source) width/height */
    gsize fsz;                     /* pmem frame size = vw*vh*3/2 */
    gint fb_fd, pmem_fd;
    void *pmem_map;
    uint32_t ov_id;
    gboolean ov_set;
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
    if (self->fb_fd >= 0 && self->ov_set) {
        uint32_t id = self->ov_id;
        ioctl (self->fb_fd, MSMFB_OVERLAY_UNSET, &id);
    }
    self->ov_set = FALSE;
    if (self->pmem_map) munmap (self->pmem_map, self->fsz), self->pmem_map = NULL;
    if (self->pmem_fd >= 0) close (self->pmem_fd), self->pmem_fd = -1;
    if (self->fb_fd >= 0) close (self->fb_fd), self->fb_fd = -1;
}

static gboolean gst_mdpoverlay_set_caps (GstBaseSink *bsink, GstCaps *caps) {
    GstMdpOverlay *self = GST_MDPOVERLAY (bsink);
    GstVideoInfo info;
    if (!gst_video_info_from_caps (&info, caps)) return FALSE;
    overlay_teardown (self);

    self->vw = GST_VIDEO_INFO_WIDTH (&info);
    self->vh = GST_VIDEO_INFO_HEIGHT (&info);
    self->fsz = (gsize) self->vw * self->vh * 3 / 2;

    /* env override for standalone testing: ATLAS_OV_DST="x,y,w,h" */
    const char *e = getenv ("ATLAS_OV_DST");
    if (e) { int x,y,w,h; if (sscanf (e, "%d,%d,%d,%d", &x,&y,&w,&h) == 4) {
        self->dstx=x; self->dsty=y; self->dstw=w; self->dsth=h; } }

    self->pmem_fd = pmem_alloc (self->fsz, &self->pmem_map);
    self->fb_fd = open ("/dev/fb0", O_RDWR);
    if (self->pmem_fd < 0 || self->fb_fd < 0) {
        GST_ERROR_OBJECT (self, "pmem/fb0 open failed (pmem=%d fb=%d)", self->pmem_fd, self->fb_fd);
        overlay_teardown (self); return FALSE;
    }

    struct mdp_overlay ov; memset (&ov, 0, sizeof ov);
    ov.src.width = self->vw; ov.src.height = self->vh; ov.src.format = MDP_Y_CBCR_H2V2;
    ov.src_rect.x = 0; ov.src_rect.y = 0; ov.src_rect.w = self->vw; ov.src_rect.h = self->vh;
    ov.dst_rect.x = self->dstx; ov.dst_rect.y = self->dsty;
    ov.dst_rect.w = self->dstw; ov.dst_rect.h = self->dsth;
    ov.z_order = 1;            /* above the UI base layer */
    ov.is_fg = 1; ov.alpha = 0xff; ov.transp_mask = 0xffffffff;
    ov.id = MSMFB_NEW_REQUEST;
    if (ioctl (self->fb_fd, MSMFB_OVERLAY_SET, &ov) < 0) {
        GST_ERROR_OBJECT (self, "MSMFB_OVERLAY_SET failed: %s", g_strerror (errno));
        overlay_teardown (self); return FALSE;
    }
    self->ov_id = ov.id; self->ov_set = TRUE;
    GST_INFO_OBJECT (self, "overlay id=%u  src %dx%d NV12 -> dst %d,%d %dx%d",
        self->ov_id, self->vw, self->vh, self->dstx, self->dsty, self->dstw, self->dsth);
    return TRUE;
}

static GstFlowReturn gst_mdpoverlay_show_frame (GstVideoSink *vsink, GstBuffer *buf) {
    GstMdpOverlay *self = GST_MDPOVERLAY (vsink);
    if (!self->ov_set) return GST_FLOW_OK;
    GstMapInfo m;
    if (!gst_buffer_map (buf, &m, GST_MAP_READ)) return GST_FLOW_ERROR;
    memcpy (self->pmem_map, m.data, MIN (m.size, self->fsz));   /* v1: copy into pmem */
    gst_buffer_unmap (buf, &m);

    struct msmfb_overlay_data play; memset (&play, 0, sizeof play);
    play.id = self->ov_id;
    play.data.offset = 0;
    play.data.memory_id = self->pmem_fd;
    if (ioctl (self->fb_fd, MSMFB_OVERLAY_PLAY, &play) < 0) {
        GST_WARNING_OBJECT (self, "OVERLAY_PLAY failed: %s", g_strerror (errno));
        return GST_FLOW_OK;   /* non-fatal — keep streaming */
    }
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
    self->fb_fd = self->pmem_fd = -1;
    self->dstx = 0; self->dsty = 0; self->dstw = 1024; self->dsth = 768;   /* default fullscreen */
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
    gst_element_class_set_static_metadata (ec, "MDP4 HW overlay sink",
        "Sink/Video", "Display linear NV12 via an MDP4 hardware overlay (hole-punch)", "Atlas TouchPad");
    bc->set_caps = gst_mdpoverlay_set_caps;
    bc->stop = gst_mdpoverlay_stop;
    vc->show_frame = gst_mdpoverlay_show_frame;
}

static gboolean plugin_init (GstPlugin *plugin) {
    GST_DEBUG_CATEGORY_INIT (mdpoverlay_debug, "mdpoverlay", 0, "MDP4 HW overlay sink");
    return gst_element_register (plugin, "mdpoverlaysink", GST_RANK_NONE, GST_TYPE_MDPOVERLAY);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, mdpoverlay,
    "MDP4 HW overlay video sink for Atlas TouchPad",
    plugin_init, "1.0", "LGPL", "atlas", "atlas")
