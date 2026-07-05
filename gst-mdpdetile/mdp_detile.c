/* mdp_detile — standalone MDP4 rotator de-tile primitive (validation).
 * Reads a captured TILED NV12 frame (MDP_Y_CRCB_H2V2_TILE, decoder output), de-tiles it in HW
 * via /dev/msm_rotator into a LINEAR NV12 buffer, writes the result out. Proves the rotator recipe
 * before wrapping it as a GStreamer element to replace CPU videoconvert. Recipe reverse-engineered
 * from the legacy libPmMediaGstVideoSinkLib.so (_vhm_request_rotator_session / _vhm_rotate).
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>

/* --- kernel ABI (from include/linux/msm_rotator.h + msm_mdp.h + android_pmem.h) --- */
struct msmfb_img  { uint32_t width, height, format; };
struct mdp_rect   { uint32_t x, y, w, h; };
struct msmfb_data { uint32_t offset; int memory_id; int id; uint32_t flags; uint32_t priv; };

struct msm_rotator_img_info {
    uint32_t session_id;
    struct msmfb_img src, dst;
    struct mdp_rect  src_rect;
    uint32_t dst_x, dst_y;
    unsigned char rotations;
    int enable;
};
struct msm_rotator_data_info {
    int session_id;
    struct msmfb_data src, dst;
    uint32_t version_key;
    struct msmfb_data src_chroma, dst_chroma;
};
#define MSM_ROTATOR_IOCTL_START  _IOWR('R', 1, struct msm_rotator_img_info)
#define MSM_ROTATOR_IOCTL_ROTATE _IOW ('R', 2, struct msm_rotator_data_info)
#define MSM_ROTATOR_IOCTL_FINISH _IOW ('R', 3, int)
#define ROTATOR_VERSION_01 0xA5B4C301
#define PMEM_ALLOCATE      _IOW('p', 5, unsigned int)

#define MDP_Y_CRCB_H2V2       5   /* linear NV21-order semiplanar */
#define MDP_Y_CBCR_H2V2       2   /* linear NV12-order semiplanar */
#define MDP_Y_CRCB_H2V2_TILE  12  /* tiled */
#define MDP_Y_CBCR_H2V2_TILE  13  /* tiled */

static int pmem_alloc(size_t sz, void **map) {
    int fd = open("/dev/pmem_adsp", O_RDWR);
    if (fd < 0) { perror("open pmem_adsp"); return -1; }
    if (ioctl(fd, PMEM_ALLOCATE, sz) < 0) { perror("PMEM_ALLOCATE"); close(fd); return -1; }
    void *p = mmap(0, sz, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap pmem"); close(fd); return -1; }
    *map = p;
    return fd;
}

int main(int argc, char **argv) {
    if (argc < 8) {
        fprintf(stderr, "usage: %s <tiled.nv12> <frame_idx> <W> <H> <src_fmt> <dst_fmt> <out.nv12>\n"
                        "  W,H = coded (buffer) dims; src_fmt 12|13; dst_fmt 5|2\n", argv[0]);
        return 2;
    }
    const char *inpath = argv[1];
    int fidx = atoi(argv[2]);
    int W = atoi(argv[3]), H = atoi(argv[4]);
    int src_fmt = atoi(argv[5]), dst_fmt = atoi(argv[6]);
    const char *outpath = argv[7];

    size_t ysize = (size_t)W * H;
    size_t bufsz = ysize * 3 / 2;                 /* NV12: Y + CbCr/2 */

    /* --- alloc src + dst pmem --- */
    void *src_map, *dst_map;
    int src_fd = pmem_alloc(bufsz, &src_map);
    int dst_fd = pmem_alloc(bufsz, &dst_map);
    if (src_fd < 0 || dst_fd < 0) return 1;
    memset(dst_map, 0x80, bufsz);                 /* poison so we can see what got written */

    /* --- load one tiled frame into src --- */
    int in = open(inpath, O_RDONLY);
    if (in < 0) { perror("open input"); return 1; }
    if (lseek(in, (off_t)fidx * bufsz, SEEK_SET) < 0) { perror("lseek"); return 1; }
    ssize_t rd = read(in, src_map, bufsz);
    if (rd != (ssize_t)bufsz) { fprintf(stderr, "short read %zd/%zu (frame past EOF?)\n", rd, bufsz); return 1; }
    close(in);
    fprintf(stderr, "loaded frame %d (%zu bytes), W=%d H=%d ysize=%zu\n", fidx, bufsz, W, H, ysize);

    /* --- open rotator + START session --- */
    int rot = open("/dev/msm_rotator", O_RDWR);
    if (rot < 0) { perror("open msm_rotator"); return 1; }

    struct msm_rotator_img_info img;
    memset(&img, 0, sizeof img);
    img.src.width = W; img.src.height = H; img.src.format = src_fmt;
    img.dst.width = W; img.dst.height = H; img.dst.format = dst_fmt;
    img.src_rect.x = 0; img.src_rect.y = 0; img.src_rect.w = W; img.src_rect.h = H;
    img.dst_x = 0; img.dst_y = 0;
    img.rotations = 0;
    img.enable = 1;
    if (ioctl(rot, MSM_ROTATOR_IOCTL_START, &img) < 0) { perror("IOCTL_START"); return 1; }
    fprintf(stderr, "START ok: session_id=%u (src_fmt=%d dst_fmt=%d)\n", img.session_id, src_fmt, dst_fmt);

    /* --- ROTATE (de-tile) --- */
    struct msm_rotator_data_info d;
    memset(&d, 0, sizeof d);
    d.session_id = img.session_id;
    d.src.offset = 0;         d.src.memory_id = src_fd;
    d.dst.offset = 0;         d.dst.memory_id = dst_fd;
    d.version_key = ROTATOR_VERSION_01;
    d.src_chroma.offset = ysize; d.src_chroma.memory_id = src_fd;  /* tiled chroma plane */
    d.dst_chroma.offset = ysize; d.dst_chroma.memory_id = dst_fd;  /* linear CbCr after Y */
    if (ioctl(rot, MSM_ROTATOR_IOCTL_ROTATE, &d) < 0) { perror("IOCTL_ROTATE"); return 1; }
    fprintf(stderr, "ROTATE ok\n");

    /* --- benchmark: time N rotates (8th arg) --- */
    int iters = (argc > 8) ? atoi(argv[8]) : 0;
    if (iters > 0) {
        struct timespec t0, t1;
        double best = 1e9, worst = 0, sum = 0;
        for (int i = 0; i < iters; i++) {
            clock_gettime(CLOCK_MONOTONIC, &t0);
            if (ioctl(rot, MSM_ROTATOR_IOCTL_ROTATE, &d) < 0) { perror("ROTATE(bench)"); return 1; }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
            sum += ms; if (ms < best) best = ms; if (ms > worst) worst = ms;
        }
        fprintf(stderr, "BENCH %d rotates 640x384: avg=%.3fms min=%.3fms max=%.3fms  (%.1f fps de-tile, 30fps budget=33.3ms)\n",
                iters, sum / iters, best, worst, 1000.0 / (sum / iters));
    }
    ioctl(rot, MSM_ROTATOR_IOCTL_FINISH, &img.session_id);

    /* --- write linear result out --- */
    int out = open(outpath, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (out < 0) { perror("open out"); return 1; }
    ssize_t wr = write(out, dst_map, bufsz);
    close(out);
    fprintf(stderr, "wrote %zd bytes -> %s\n", wr, outpath);
    return 0;
}
