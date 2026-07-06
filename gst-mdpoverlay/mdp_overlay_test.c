/* mdp_overlay_test — standalone validation of the MDP4 hardware overlay path on the TouchPad (APQ8060).
 *
 * Opens /dev/fb0, allocates a pmem_adsp buffer, fills it with an NV12 R/G/B color-bar test frame,
 * and displays it via a hardware overlay plane (MSMFB_OVERLAY_SET/PLAY) at a fixed on-screen rect.
 * If a red|green|blue bar rectangle appears on the panel, the overlay hardware + NV12 path work — the
 * foundation for routing decoded video straight to the display (zero readback). Also verifies chroma
 * order (MDP_Y_CBCR_H2V2 = NV12). This is the overlay analogue of gst-mdpdetile/mdp_detile.c.
 *
 * Usage: ./mdp_overlay_test [dstx dsty dstw dsth] [srcw srch] [seconds]
 *   default: dst 200,150 400x300 ; src 320x240 ; 8s
 *
 * Cross-build (staging-glibc-252 / gcc125): see build.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

/* --- kernel ABI (include/linux/msm_mdp.h + android_pmem.h) --- */
struct mdp_rect { uint32_t x, y, w, h; };
struct msmfb_img { uint32_t width, height, format; };
struct mdp_overlay {
    struct msmfb_img src;
    struct mdp_rect  src_rect;
    struct mdp_rect  dst_rect;
    uint32_t z_order;      /* stage number (0..) */
    uint32_t is_fg;        /* 1 = foreground pipe (controls alpha/transp blend) */
    uint32_t alpha;
    uint32_t transp_mask;
    uint32_t flags;
    uint32_t id;           /* MSMFB_NEW_REQUEST(-1) in, assigned id out */
    uint32_t user_data[8];
};
struct msmfb_data { uint32_t offset; int memory_id; int id; uint32_t flags; uint32_t priv; };
struct msmfb_overlay_data { uint32_t id; struct msmfb_data data; };

#define MSMFB_OVERLAY_SET   _IOWR('m', 135, struct mdp_overlay)
#define MSMFB_OVERLAY_UNSET _IOW ('m', 136, unsigned int)
#define MSMFB_OVERLAY_PLAY  _IOW ('m', 137, struct msmfb_overlay_data)
#define MSMFB_NEW_REQUEST   ((uint32_t)-1)

#define MDP_Y_CBCR_H2V2     2      /* NV12 (Cb in MSB) — matches gst-mdpdetile output */
#define MDP_RGBA_8888       9
#define MDP_OV_PIPE_SHARE   0x00800000
#define PMEM_ALLOCATE       _IOW('p', 5, unsigned int)

static int pmem_alloc(size_t sz, void **map, int *outfd) {
    int fd = open("/dev/pmem_adsp", O_RDWR);
    if (fd < 0) { perror("open /dev/pmem_adsp"); return -1; }
    if (ioctl(fd, PMEM_ALLOCATE, sz) < 0) { perror("PMEM_ALLOCATE"); close(fd); return -1; }
    void *p = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap pmem"); close(fd); return -1; }
    *map = p; *outfd = fd; return 0;
}

/* BT.601 YCbCr for pure R/G/B */
static void rgb_bars_nv12(uint8_t *buf, int w, int h) {
    struct { uint8_t Y, Cb, Cr; } bar[3] = {
        { 76,  85, 255 },   /* red   */
        {150,  44,  21 },   /* green */
        { 29, 255, 107 },   /* blue  */
    };
    uint8_t *Y = buf, *C = buf + (size_t)w * h;   /* NV12: Y plane then interleaved CbCr */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            Y[(size_t)y * w + x] = bar[(x * 3) / w].Y;
    for (int cy = 0; cy < h / 2; cy++)
        for (int cx = 0; cx < w / 2; cx++) {
            int b = ((cx * 2) * 3) / w;
            C[(size_t)cy * w + cx * 2 + 0] = bar[b].Cb;
            C[(size_t)cy * w + cx * 2 + 1] = bar[b].Cr;
        }
}

int main(int argc, char **argv) {
    int dx = 200, dy = 150, dw = 400, dh = 300, sw = 320, sh = 240, secs = 8;
    uint32_t flags = 0, zorder = 1, fmt = MDP_Y_CBCR_H2V2;
    if (argc >= 5) { dx = atoi(argv[1]); dy = atoi(argv[2]); dw = atoi(argv[3]); dh = atoi(argv[4]); }
    if (argc >= 7) { sw = atoi(argv[5]); sh = atoi(argv[6]); }
    if (argc >= 8) { secs = atoi(argv[7]); }
    if (argc >= 9) { flags = (uint32_t)strtoul(argv[8], NULL, 0); }   /* e.g. 0x800000 = MDP_OV_PIPE_SHARE */
    if (argc >= 10){ zorder = (uint32_t)atoi(argv[9]); }
    if (argc >= 11){ fmt = (uint32_t)atoi(argv[10]); }               /* 2=NV12(VG pipe), 9=RGBA(RGB pipe) */
    sw &= ~1; sh &= ~1;
    printf("params: dst %d,%d %dx%d src %dx%d fmt=%u flags=0x%x z=%u\n", dx,dy,dw,dh,sw,sh,fmt,flags,zorder);

    int fb = open("/dev/fb0", O_RDWR);
    if (fb < 0) { perror("open /dev/fb0"); return 1; }

    int isRGB = (fmt == MDP_RGBA_8888);
    size_t fsz = isRGB ? (size_t)sw * sh * 4 : (size_t)sw * sh * 3 / 2;
    void *map; int pfd;
    if (pmem_alloc(fsz, &map, &pfd) < 0) { close(fb); return 1; }
    if (isRGB) {   /* R|G|B bars, RGBA8888 */
        uint32_t *px = (uint32_t *)map;
        uint32_t bar[3] = { 0xff0000ff, 0xff00ff00, 0xffff0000 };   /* ABGR-ish; exact order not critical for the pipe test */
        for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) px[(size_t)y*sw + x] = bar[(x*3)/sw];
    } else {
        rgb_bars_nv12((uint8_t *)map, sw, sh);
    }

    struct mdp_overlay ov; memset(&ov, 0, sizeof ov);
    ov.src.width = sw; ov.src.height = sh; ov.src.format = fmt;
    ov.src_rect.x = 0; ov.src_rect.y = 0; ov.src_rect.w = sw; ov.src_rect.h = sh;
    ov.dst_rect.x = dx; ov.dst_rect.y = dy; ov.dst_rect.w = dw; ov.dst_rect.h = dh;
    ov.z_order = zorder;     /* above the UI base layer */
    ov.is_fg   = 1;
    ov.alpha   = 0xff;
    ov.transp_mask = 0xffffffff;
    ov.flags   = flags;
    ov.id      = MSMFB_NEW_REQUEST;

    if (ioctl(fb, MSMFB_OVERLAY_SET, &ov) < 0) { perror("MSMFB_OVERLAY_SET"); close(fb); return 1; }
    printf("OVERLAY_SET ok: id=%u  src %dx%d NV12  dst %d,%d %dx%d z=%u\n",
           ov.id, sw, sh, dx, dy, dw, dh, ov.z_order);

    struct msmfb_overlay_data play; memset(&play, 0, sizeof play);
    play.id = ov.id;
    play.data.offset = 0;
    play.data.memory_id = pfd;     /* pmem fd holding the NV12 frame */
    if (ioctl(fb, MSMFB_OVERLAY_PLAY, &play) < 0) { perror("MSMFB_OVERLAY_PLAY"); }
    else printf("OVERLAY_PLAY ok — R|G|B bars should be on screen at %d,%d for %ds\n", dx, dy, secs);

    /* re-play each second: some MDP builds retire the overlay after one refresh if not kept fed */
    for (int i = 0; i < secs; i++) { sleep(1); ioctl(fb, MSMFB_OVERLAY_PLAY, &play); }

    uint32_t id = ov.id;
    if (ioctl(fb, MSMFB_OVERLAY_UNSET, &id) < 0) perror("MSMFB_OVERLAY_UNSET");
    else printf("OVERLAY_UNSET ok — cleaned up\n");

    munmap(map, fsz); close(pfd); close(fb);
    return 0;
}
