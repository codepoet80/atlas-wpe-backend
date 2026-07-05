/* gstmdpdetile — GStreamer element: HW de-tile of Qualcomm tiled NV12 (NV12_64Z32) -> linear NV12
 * via the MDP4 rotator (/dev/msm_rotator). Drop-in replacement for the CPU `videoconvert` de-tile
 * in the Atlas/WPE video path on the HP TouchPad (APQ8060). Recipe validated standalone (mdp_detile.c):
 * ~2.6ms/frame @640p HW, vs CPU videoconvert that caused QoS frame-drops.
 *
 * v1: copies the incoming tiled buffer into a pmem src buffer, ROTATEs into a pmem dst buffer, then
 * copies the de-tiled result (cropped to display height) into the output buffer. Zero-copy input
 * (feed the decoder's OMX pmem fd from pPlatformPrivate) is a later optimization.
 */
#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

GST_DEBUG_CATEGORY_STATIC (mdpdetile_debug);
#define GST_CAT_DEFAULT mdpdetile_debug

/* --- kernel ABI (include/linux/msm_rotator.h + msm_mdp.h + android_pmem.h) --- */
struct msmfb_img  { uint32_t width, height, format; };
struct mdp_rect   { uint32_t x, y, w, h; };
struct msmfb_data { uint32_t offset; int memory_id; int id; uint32_t flags; uint32_t priv; };
struct msm_rotator_img_info {
    uint32_t session_id; struct msmfb_img src, dst; struct mdp_rect src_rect;
    uint32_t dst_x, dst_y; unsigned char rotations; int enable;
};
struct msm_rotator_data_info {
    int session_id; struct msmfb_data src, dst; uint32_t version_key;
    struct msmfb_data src_chroma, dst_chroma;
};
#define MSM_ROTATOR_IOCTL_START  _IOWR('R', 1, struct msm_rotator_img_info)
#define MSM_ROTATOR_IOCTL_ROTATE _IOW ('R', 2, struct msm_rotator_data_info)
#define MSM_ROTATOR_IOCTL_FINISH _IOW ('R', 3, int)
#define ROTATOR_VERSION_01 0xA5B4C301
#define PMEM_ALLOCATE      _IOW('p', 5, unsigned int)
#define MDP_Y_CRCB_H2V2       5
#define MDP_Y_CRCB_H2V2_TILE  12

#define GST_TYPE_MDPDETILE (gst_mdpdetile_get_type())
G_DECLARE_FINAL_TYPE (GstMdpDetile, gst_mdpdetile, GST, MDPDETILE, GstBaseTransform)

struct _GstMdpDetile {
    GstBaseTransform parent;
    gint dw, dh;            /* display width/height (output) */
    gint cw, ch;            /* coded/tiled width/height (rotator + src pmem) */
    gsize src_sz, dst_sz;   /* pmem buffer sizes (coded) */
    gint rot_fd, src_fd, dst_fd;
    void *src_map, *dst_map;
    uint32_t session_id;
    gboolean ready;
};
G_DEFINE_TYPE (GstMdpDetile, gst_mdpdetile, GST_TYPE_BASE_TRANSFORM)

static GstStaticPadTemplate sink_tmpl = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, format=(string)NV12_64Z32, "
        "width=(int)[1,4096], height=(int)[1,4096], framerate=(fraction)[0/1,MAX]"));
static GstStaticPadTemplate src_tmpl = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC, GST_PAD_ALWAYS,
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

static void detile_teardown (GstMdpDetile *self) {
    if (self->rot_fd >= 0 && self->ready)
        ioctl (self->rot_fd, MSM_ROTATOR_IOCTL_FINISH, &self->session_id);
    if (self->src_map) munmap (self->src_map, self->src_sz), self->src_map = NULL;
    if (self->dst_map) munmap (self->dst_map, self->dst_sz), self->dst_map = NULL;
    if (self->src_fd >= 0) close (self->src_fd), self->src_fd = -1;
    if (self->dst_fd >= 0) close (self->dst_fd), self->dst_fd = -1;
    if (self->rot_fd >= 0) close (self->rot_fd), self->rot_fd = -1;
    self->ready = FALSE;
}

/* NV12_64Z32 sink <-> NV12 src, keeping width/height/framerate */
static GstCaps *
gst_mdpdetile_transform_caps (GstBaseTransform *trans, GstPadDirection dir,
    GstCaps *caps, GstCaps *filter) {
    const char *outfmt = (dir == GST_PAD_SINK) ? "NV12" : "NV12_64Z32";
    GstCaps *res = gst_caps_copy (caps);
    for (guint i = 0; i < gst_caps_get_size (res); i++)
        gst_structure_set (gst_caps_get_structure (res, i),
            "format", G_TYPE_STRING, outfmt, NULL);
    if (filter) {
        GstCaps *tmp = gst_caps_intersect_full (filter, res, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref (res); res = tmp;
    }
    return res;
}

static gboolean
gst_mdpdetile_set_caps (GstBaseTransform *trans, GstCaps *incaps, GstCaps *outcaps) {
    GstMdpDetile *self = GST_MDPDETILE (trans);
    GstVideoInfo iinfo, oinfo;
    if (!gst_video_info_from_caps (&iinfo, incaps) ||
        !gst_video_info_from_caps (&oinfo, outcaps))
        return FALSE;

    detile_teardown (self);
    self->dw = GST_VIDEO_INFO_WIDTH (&oinfo);
    self->dh = GST_VIDEO_INFO_HEIGHT (&oinfo);
    self->cw = GST_ROUND_UP_64 (GST_VIDEO_INFO_WIDTH (&iinfo));   /* tile 64 wide */
    self->ch = GST_ROUND_UP_32 (GST_VIDEO_INFO_HEIGHT (&iinfo));  /* tile 32 tall */
    self->src_sz = (gsize) self->cw * self->ch * 3 / 2;
    self->dst_sz = self->src_sz;
    GST_INFO_OBJECT (self, "detile %dx%d (coded %dx%d, insize=%zu) -> NV12 %dx%d",
        self->dw, self->dh, self->cw, self->ch, self->src_sz, self->dw, self->dh);

    self->src_fd = pmem_alloc (self->src_sz, &self->src_map);
    self->dst_fd = pmem_alloc (self->dst_sz, &self->dst_map);
    self->rot_fd = open ("/dev/msm_rotator", O_RDWR);
    if (self->src_fd < 0 || self->dst_fd < 0 || self->rot_fd < 0) {
        GST_ERROR_OBJECT (self, "pmem/rotator open failed (src=%d dst=%d rot=%d)",
            self->src_fd, self->dst_fd, self->rot_fd);
        detile_teardown (self); return FALSE;
    }

    struct msm_rotator_img_info img; memset (&img, 0, sizeof img);
    img.src.width = self->cw; img.src.height = self->ch; img.src.format = MDP_Y_CRCB_H2V2_TILE;
    img.dst.width = self->cw; img.dst.height = self->ch; img.dst.format = MDP_Y_CRCB_H2V2;
    img.src_rect.x = 0; img.src_rect.y = 0; img.src_rect.w = self->cw; img.src_rect.h = self->ch;
    img.rotations = 0; img.enable = 1;
    if (ioctl (self->rot_fd, MSM_ROTATOR_IOCTL_START, &img) < 0) {
        GST_ERROR_OBJECT (self, "MSM_ROTATOR_IOCTL_START failed: %s", g_strerror (errno));
        detile_teardown (self); return FALSE;
    }
    self->session_id = img.session_id;
    self->ready = TRUE;
    GST_INFO_OBJECT (self, "rotator session %u ready", self->session_id);
    return TRUE;
}

static GstFlowReturn
gst_mdpdetile_transform (GstBaseTransform *trans, GstBuffer *inbuf, GstBuffer *outbuf) {
    GstMdpDetile *self = GST_MDPDETILE (trans);
    if (!self->ready) return GST_FLOW_NOT_NEGOTIATED;

    GstMapInfo im, om;
    if (!gst_buffer_map (inbuf, &im, GST_MAP_READ)) return GST_FLOW_ERROR;
    /* copy tiled frame into src pmem (v1; zero-copy via decoder pmem fd is a later step) */
    gsize n = MIN (im.size, self->src_sz);
    memcpy (self->src_map, im.data, n);
    gst_buffer_unmap (inbuf, &im);

    struct msm_rotator_data_info d; memset (&d, 0, sizeof d);
    gsize cysize = (gsize) self->cw * self->ch;   /* coded Y plane size */
    d.session_id = self->session_id;
    d.src.offset = 0;          d.src.memory_id = self->src_fd;
    d.dst.offset = 0;          d.dst.memory_id = self->dst_fd;
    d.version_key = ROTATOR_VERSION_01;
    d.src_chroma.offset = cysize; d.src_chroma.memory_id = self->src_fd;
    d.dst_chroma.offset = cysize; d.dst_chroma.memory_id = self->dst_fd;
    if (ioctl (self->rot_fd, MSM_ROTATOR_IOCTL_ROTATE, &d) < 0) {
        GST_ERROR_OBJECT (self, "ROTATE failed: %s", g_strerror (errno));
        return GST_FLOW_ERROR;
    }

    /* copy de-tiled result into the output buffer, cropping coded height -> display height */
    if (!gst_buffer_map (outbuf, &om, GST_MAP_WRITE)) return GST_FLOW_ERROR;
    guint8 *dp = self->dst_map;
    gsize dy = (gsize) self->dw * self->dh;             /* out Y size */
    gsize dc = dy / 2;                                  /* out CbCr size */
    /* Y: dw wide, dh rows out of a cw-strided coded plane */
    for (gint y = 0; y < self->dh; y++)
        memcpy (om.data + (gsize) y * self->dw, dp + (gsize) y * self->cw, self->dw);
    /* Chroma: the rotator can only emit MDP_Y_CRCB (Cr-first, = NV21 byte order) for a CrCb-tiled
     * src (src=12->dst=2 is rejected InvalidArgument). Swap each (Cr,Cb) pair -> (Cb,Cr) so we emit
     * true NV12. dw wide, dh/2 rows; coded chroma at cysize with cw stride. */
    guint8 *sc = dp + cysize;
    guint8 *oc = om.data + dy;
    for (gint y = 0; y < self->dh / 2; y++) {
        const guint8 *srow = sc + (gsize) y * self->cw;
        guint8 *orow = oc + (gsize) y * self->dw;
        for (gint x = 0; x + 1 < self->dw; x += 2) {
            orow[x]     = srow[x + 1];   /* Cb */
            orow[x + 1] = srow[x];       /* Cr */
        }
    }
    (void) dc;
    gst_buffer_unmap (outbuf, &om);
    return GST_FLOW_OK;
}

/* Tell GstBaseTransform each format's buffer size (else it defaults the output buffer to the input
 * size = the coded 640x384, not the 640x360 display size). */
static gboolean
gst_mdpdetile_get_unit_size (GstBaseTransform *trans, GstCaps *caps, gsize *size) {
    GstVideoInfo info;
    if (!gst_video_info_from_caps (&info, caps)) return FALSE;
    *size = GST_VIDEO_INFO_SIZE (&info);
    return TRUE;
}

static gboolean gst_mdpdetile_stop (GstBaseTransform *trans) {
    detile_teardown (GST_MDPDETILE (trans));
    return TRUE;
}

static void gst_mdpdetile_init (GstMdpDetile *self) {
    self->rot_fd = self->src_fd = self->dst_fd = -1;
}

static void gst_mdpdetile_class_init (GstMdpDetileClass *klass) {
    GstElementClass *ec = GST_ELEMENT_CLASS (klass);
    GstBaseTransformClass *tc = GST_BASE_TRANSFORM_CLASS (klass);
    gst_element_class_add_static_pad_template (ec, &sink_tmpl);
    gst_element_class_add_static_pad_template (ec, &src_tmpl);
    gst_element_class_set_static_metadata (ec, "MDP4 HW de-tiler",
        "Filter/Converter/Video", "De-tile Qualcomm tiled NV12 via the MDP rotator",
        "Atlas TouchPad");
    tc->transform_caps = gst_mdpdetile_transform_caps;
    tc->set_caps = gst_mdpdetile_set_caps;
    tc->transform = gst_mdpdetile_transform;
    tc->get_unit_size = gst_mdpdetile_get_unit_size;
    tc->stop = gst_mdpdetile_stop;
    tc->passthrough_on_same_caps = FALSE;
}

static gboolean plugin_init (GstPlugin *plugin) {
    GST_DEBUG_CATEGORY_INIT (mdpdetile_debug, "mdpdetile", 0, "MDP4 HW de-tiler");
    return gst_element_register (plugin, "mdpdetile", GST_RANK_NONE, GST_TYPE_MDPDETILE);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, mdpdetile,
    "MDP4 rotator HW de-tiler for Qualcomm tiled NV12",
    plugin_init, "1.0", "LGPL", "atlas", "atlas")
