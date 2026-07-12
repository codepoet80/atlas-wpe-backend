/*
 * wpe-atlas-backend.c — custom libwpe backend for WPE WebKit on webOS / HP TouchPad.
 *
 * SKETCH / first cut. Implements the libwpe loader + view-backend + renderer-host (UIProcess) and
 * renderer-backend-egl + target (WebProcess). WebKit owns the EGL context/surface; we provide the
 * native display + native window and read back each rendered frame (glReadPixels) into a SysV-shm
 * double buffer shared with the UIProcess, which hands ARGB frames to the Atlas offscreen path.
 *
 * Frame flow:
 *   WebProcess target.frame_rendered -> glReadPixels -> shm[slot] -> socket MSG_FRAME_READY(slot)
 *                                    -> dispatch_frame_complete (let WebKit render the other slot)
 *   UIProcess  view io-watch        -> frame_handler(shm[slot]) -> dispatch_frame_displayed
 *                                    -> socket MSG_FRAME_ACK
 *
 * DEVICE TODOs are marked with  >>> DEVICE: ...  — the Adreno-220 EGL surface mode is the main one.
 */

#include "wpe-atlas-backend.h"

#include <wpe/wpe-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>   /* C: EGL_KHR_lock_surface (fast readback) */
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>   /* GL_BGRA_EXT */

#include <sys/time.h>       /* gettimeofday — per-frame PERF timing */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>

#include <glib.h>
#include <glib-unix.h>

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

/* Off unless BPWPE_DEBUG is set (checked once) — set it in the launch wrapper to debug on-device. */
static inline int _atlas_log_on(void) { static int on = -1; if (on < 0) on = getenv("BPWPE_DEBUG") ? 1 : 0; return on; }
#define ATLAS_LOG(fmt, ...) do { if (_atlas_log_on()) fprintf(stderr, "[wpe-atlas] " fmt "\n", ##__VA_ARGS__); } while (0)

/* ---- wire protocol (SOCK_SEQPACKET, fixed-size messages) ---------------------------------- */
enum atlas_msg_type { MSG_SETUP = 1, MSG_FRAME_READY = 2, MSG_FRAME_ACK = 3, MSG_RESIZE = 4 };
struct atlas_msg {
    uint32_t type;
    uint32_t shmid;    /* MSG_SETUP: SysV shm id of the 2-slot frame buffer */
    uint32_t width;    /* MSG_SETUP/MSG_RESIZE: layout size. MSG_FRAME_READY: the frame's ACTUAL width */
    uint32_t height;   /* (== scaled width/height when fit-to-screen downscaling is active) */
    uint32_t slot;     /* MSG_FRAME_READY: which slot holds the new frame */
    uint32_t swidth;   /* MSG_SETUP: fit-to-screen output size (0 = no scale). Slots are still */
    uint32_t sheight;  /* allocated at the LAYOUT size; the scaled frame packs into each slot's front */
};

#define SLOTS 2
static size_t slot_bytes(uint32_t w, uint32_t h) { return (size_t)w * h * 4; }

/* P2 strip readback: a small control block placed AFTER the SLOTS frame slots in the SAME shm.
 * BrowserPageWPE::setScrollPosition bumps `seq` with the exposed strip (y0,stripH) + the pan delta
 * before window.scrollTo; the WebProcess reads it in target_frame_rendered and reads back ONLY that
 * strip (glReadPixels sub-rect, ~50-100ms) after memmove'ing the overlap in its master buffer —
 * instead of the whole tall buffer (~1.2s). Coherent SysV shm + a seq handshake, no extra socket. */
struct atlas_ctrl { volatile uint32_t seq; int32_t y0; int32_t stripH; int32_t delta; };
/* Video damage-rect: while a <video> plays, the ONLY page region changing is its rect. BrowserPageWPE
 * publishes it here (device px, top-origin, 1:1 — same space as glReadPixels/the master buffer); the
 * WebProcess then reads back ONLY that sub-rect each frame and splices it into the master (rest of the
 * page is static → comes from master), turning the full 1024x3072 readback into ~vw*vh. seq is a
 * version guard (read seq, fields, seq again — retry-as-full on tear). active=0 → normal full readback. */
struct atlas_vrect { volatile uint32_t seq; int32_t x, y, w, h; int32_t active; };
static size_t shm_total_bytes(uint32_t w, uint32_t h) { return (size_t)SLOTS * slot_bytes(w, h) + 4096; }
static struct atlas_ctrl* shm_ctrl(uint8_t* shm, uint32_t w, uint32_t h) {
    return (struct atlas_ctrl*)(shm + (size_t)SLOTS * slot_bytes(w, h));
}
/* placed 64B after atlas_ctrl in the SAME 4096B ctrl page (atlas_ctrl is 16B) */
static struct atlas_vrect* shm_vrect(uint8_t* shm, uint32_t w, uint32_t h) {
    return (struct atlas_vrect*)(shm + (size_t)SLOTS * slot_bytes(w, h) + 64);
}

/* =========================================================================================== *
 *  UIProcess side: view backend (+ the frame receiver)                                        *
 * =========================================================================================== */
struct atlas_view {
    struct wpe_view_backend* wpe;
    wpe_atlas_frame_handler   handler;
    void*                    userdata;
    uint32_t                 width, height;      /* CURRENT layout size (shm slot granularity; changes on rotate) */
    uint32_t                 alloc_w, alloc_h;   /* size the shm was allocated for (capacity bound for resize) */
    uint32_t                 swidth, sheight;    /* fit-to-screen output size (0 = 1:1, no downscale) */

    int   shmid;
    uint8_t* shm;      /* attached SLOTS*slot_bytes + ctrl region, or NULL until initialize() */
    bool     visible;
    struct atlas_view* next;   /* P2: registry so wpe_atlas_view_backend_scroll_hint can map wpe -> view */
};

static struct atlas_view* g_atlas_views = NULL;   /* P2 wpe->view registry (few live views; linear walk) */

/* One dedicated channel PER WebProcess. WebKit spawns/swaps/prewarms processes (PSON); each must
 * get its own socketpair + SETUP so messages from different processes never collide on one channel
 * (which stalled loads with "bad SETUP" after a cross-site jump). Lives until its process exits. */
struct atlas_conn {
    struct atlas_view* v;
    int   sock_ui, sock_web;
    guint io_source;
};

static gboolean atlas_view_on_readable(gint fd, GIOCondition cond, gpointer data);

static bool atlas_view_alloc_shm(struct atlas_view* v)
{
    size_t total = shm_total_bytes(v->width, v->height);   /* P2: SLOTS slots + the ctrl page */
    v->shmid = shmget(IPC_PRIVATE, total, IPC_CREAT | 0600);
    if (v->shmid < 0) { ATLAS_LOG("shmget(%zu) failed: %s", total, strerror(errno)); return false; }
    v->shm = (uint8_t*)shmat(v->shmid, NULL, 0);
    if (v->shm == (void*)-1) { ATLAS_LOG("shmat failed: %s", strerror(errno)); v->shm = NULL; return false; }
    return true;
}

static void* view_create(void* userdata, struct wpe_view_backend* wpe)
{
    /* userdata here is the value passed to wpe_view_backend_create_with_backend_interface(), which
     * wpe_atlas_view_backend_create() sets to the heap struct atlas_view we pre-allocated. */
    struct atlas_view* v = (struct atlas_view*)userdata;
    v->wpe = wpe;
    return v;
}

static void view_destroy(void* data)
{
    struct atlas_view* v = (struct atlas_view*)data;
    if (!v) return;
    struct atlas_view** pp = &g_atlas_views;              /* P2: unlink from the registry */
    while (*pp) { if (*pp == v) { *pp = v->next; break; } pp = &(*pp)->next; }
    if (v->shm && v->shm != (void*)-1) shmdt(v->shm);
    if (v->shmid >= 0) shmctl(v->shmid, IPC_RMID, NULL);   /* drop when last detaches */
    free(v);
}

/* DOM Element.requestFullscreen() -> WebKit ViewLegacy::setFullScreen() ->
 * wpe_view_backend_platform_set_fullscreen(), which returns the value of THIS handler. Without a
 * handler libwpe returns false, so setFullScreen() bails BEFORE emitting enterFullScreen and
 * PageClientImpl fires requestExitFullScreen() -> an enter/exit toggle that freezes the view (the
 * "fullscreen hangs it" symptom). Our webview already fills the whole BrowserServer surface, so
 * there is no OS window state to change: just accept the request (return true) and let WebKit lay
 * the element out at the fixed viewport size. */
static bool view_fullscreen_handler(void* data, bool enable)
{
    (void)data;
    ATLAS_LOG("fullscreen %s (accepted; embedded viewport is already full-surface)",
              enable ? "ENTER" : "EXIT");
    return true;
}

static void view_initialize(void* data)
{
    struct atlas_view* v = (struct atlas_view*)data;

    if (!atlas_view_alloc_shm(v)) return;

    /* Accept DOM fullscreen requests (see view_fullscreen_handler). Registered here — after the
     * backend is fully constructed and before content loads — so the very first requestFullscreen
     * is handled. */
    wpe_view_backend_set_fullscreen_handler(v->wpe, view_fullscreen_handler, v);

    /* The socketpair + SETUP + frame io_source are (re)created PER WebProcess in
     * view_get_renderer_host_fd() — WebKit swaps processes on cross-site navigation (PSON), and
     * one shared channel collides (SETUP/frame messages from different processes interleave ->
     * "bad SETUP", render stalls after the 1st site). Each process gets its OWN socketpair. */

    /* WebKit needs to know the initial size + that we're ready to show content */
    wpe_view_backend_dispatch_set_size(v->wpe, v->width, v->height);
    wpe_view_backend_add_activity_state(v->wpe,
        wpe_view_activity_state_visible | wpe_view_activity_state_in_window | wpe_view_activity_state_focused);
}

static int view_get_renderer_host_fd(void* data)
{
    struct atlas_view* v = (struct atlas_view*)data;
    ATLAS_LOG("host_fd requested (new WebProcess render channel)");

    /* Fresh dedicated channel for THIS WebProcess. NO tear-down of prior channels — that raced
     * (closing a process's channel before it read SETUP -> "bad SETUP", 2nd page in a card failed).
     * Each conn lives independently until its own WebProcess exits, then HUP frees it. */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
        ATLAS_LOG("socketpair failed: %s", strerror(errno));
        return -1;
    }
    struct atlas_conn* c = (struct atlas_conn*)calloc(1, sizeof(*c));
    if (!c) { close(sv[0]); close(sv[1]); return -1; }
    c->v = v; c->sock_ui = sv[0]; c->sock_web = sv[1];

    /* tell THIS WebProcess where the shared frame buffer is (+ the fit-to-screen output size) */
    struct atlas_msg m = { .type = MSG_SETUP, .shmid = (uint32_t)v->shmid,
                          .width = v->width, .height = v->height, .slot = 0,
                          .swidth = v->swidth, .sheight = v->sheight };
    if (send(c->sock_ui, &m, sizeof(m), 0) != (ssize_t)sizeof(m))
        ATLAS_LOG("send(SETUP) failed: %s", strerror(errno));

    /* receive THIS process's MSG_FRAME_READY on the UIProcess main loop */
    c->io_source = g_unix_fd_add(c->sock_ui, G_IO_IN | G_IO_HUP | G_IO_ERR, atlas_view_on_readable, c);

    /* Keep OUR sock_web open for now — WebKit dup()s the returned fd for the WebProcess
     * (asynchronously), so closing it here would hand the process a dead fd (kills even the 1st
     * load). It's dropped in atlas_view_on_readable on the process's FIRST message (which proves the
     * dup already happened), so the socketpair can then HUP on process exit and reap the conn. */
    return c->sock_web;
}

static gboolean atlas_view_on_readable(gint fd, GIOCondition cond, gpointer data)
{
    struct atlas_conn* c = (struct atlas_conn*)data;
    struct atlas_view* v = c->v;
    if (cond & (G_IO_HUP | G_IO_ERR)) {     /* this WebProcess exited — drop its channel */
        if (c->sock_ui  >= 0) close(c->sock_ui);
        if (c->sock_web >= 0) close(c->sock_web);
        free(c);
        return G_SOURCE_REMOVE;
    }

    struct atlas_msg m;
    ssize_t n = recv(fd, &m, sizeof(m), 0);
    if (n != (ssize_t)sizeof(m)) return G_SOURCE_CONTINUE;

    /* First real message proves WebKit already dup'd sock_web into the WebProcess, so drop OUR ref
     * to it now (race-free: the process has its own copy). Without this the kept-open sock_web pins
     * the socketpair so it never HUPs -> the conn (FD + io_source) leaks on every cross-site PSON
     * spawn; this lets the pair HUP when the process exits, reaping the channel. */
    if (c->sock_web >= 0) { close(c->sock_web); c->sock_web = -1; }

    if (m.type == MSG_FRAME_READY && v->shm && v->handler) {
        uint32_t slot = m.slot % SLOTS;
        /* Slot addressing uses the LAYOUT size (allocation granularity); the frame's ACTUAL size
         * (m.width/m.height — the scaled size when fit-to-screen downscaling is on) is packed tightly
         * at the front of the slot, so hand the handler those dims + a tightly-packed stride. */
        uint32_t fw = m.width  ? m.width  : v->width;
        uint32_t fh = m.height ? m.height : v->height;
        const uint8_t* argb = v->shm + (size_t)slot * slot_bytes(v->width, v->height);
        v->handler(v->userdata, argb, fw, fh, fw * 4);
        wpe_view_backend_dispatch_frame_displayed(v->wpe);

        struct atlas_msg ack = { .type = MSG_FRAME_ACK, 0, 0, 0, slot };
        send(fd, &ack, sizeof(ack), 0);
    }
    return G_SOURCE_CONTINUE;
}

static struct wpe_view_backend_interface s_view_iface = {
    view_create, view_destroy, view_initialize, view_get_renderer_host_fd,
    NULL, NULL, NULL, NULL
};

/* ---- public API ---------------------------------------------------------------------------- */
struct wpe_view_backend* wpe_atlas_view_backend_create(uint32_t width, uint32_t height,
                                                      uint32_t scaledWidth, uint32_t scaledHeight,
                                                      wpe_atlas_frame_handler handler, void* userdata)
{
    struct atlas_view* v = (struct atlas_view*)calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->handler = handler; v->userdata = userdata;
    v->width = width ? width : 1; v->height = height ? height : 1;
    v->alloc_w = v->width; v->alloc_h = v->height;   /* shm capacity = the launch-orientation slot size */
    /* Only downscale when a smaller-than-layout output was requested; otherwise 1:1 (0 = disabled). */
    if (scaledWidth && scaledHeight && scaledWidth < v->width) {
        v->swidth = scaledWidth; v->sheight = scaledHeight;
    } else {
        v->swidth = 0; v->sheight = 0;
    }
    v->shmid = -1;
    v->visible = true;
    /* create the wpe_view_backend with OUR interface + the atlas_view as interface data */
    struct wpe_view_backend* b = wpe_view_backend_create_with_backend_interface(&s_view_iface, v);
    v->wpe = b;                          /* also set in view_create; set here so the registry is usable immediately */
    v->next = g_atlas_views; g_atlas_views = v;   /* P2 registry */
    return b;
}

void wpe_atlas_view_backend_resize(struct wpe_view_backend* wpe, uint32_t width, uint32_t height)
{
    /* In-place resize (rotation): the shm SLOT is a fixed max-size region and the WebProcess already packs
     * the ACTUAL frame size tightly at the front of the slot (the same path fit-to-screen downscaling uses),
     * so we do NOT realloc or re-SETUP — we just tell WebKit the new layout size and it re-flows + repaints
     * into the existing slot. Works as long as the new layout fits the allocated slot (it does when the app
     * launched in the widest orientation, i.e. landscape on the TouchPad). */
    struct atlas_view* v = g_atlas_views;
    while (v && v->wpe != wpe) v = v->next;
    if (!v || width == 0 || height == 0) return;
    /* Rotation is an IN-PLACE relayout (no re-create). The shm slot was sized for the launch
     * orientation; a rotated layout on the 4:3 panel has equal-or-fewer pixels, so it fits. Bound by
     * CAPACITY (total pixels) rather than per-dimension — a per-dim check wrongly rejects the taller
     * portrait height and clamps portrait to the landscape height (the "short content" band). Then
     * UPDATE v->width/height so the host's slot + ctrl-block addressing tracks the target's (which
     * resizes via target_resize) — otherwise slot-1 reads/pan-ctrl desync after a rotate. */
    if ((size_t)width * height > (size_t)v->alloc_w * v->alloc_h) {
        ATLAS_LOG("resize %ux%u exceeds slot capacity %ux%u — ignored", width, height, v->alloc_w, v->alloc_h);
        return;
    }
    v->width = width; v->height = height;
    ATLAS_LOG("resize -> layout %ux%u (cap %ux%u)", width, height, v->alloc_w, v->alloc_h);
    wpe_view_backend_dispatch_set_size(wpe, width, height);
}

void wpe_atlas_view_backend_set_visible(struct wpe_view_backend* wpe, bool visible)
{
    if (visible) wpe_view_backend_add_activity_state(wpe, wpe_view_activity_state_visible);
    else         wpe_view_backend_remove_activity_state(wpe, wpe_view_activity_state_visible);
}

/* P2: publish the next pan re-render's exposed strip + delta into the shm control block. The BS calls
 * this BEFORE window.scrollTo; the WebProcess consumes the bumped seq in target_frame_rendered and
 * reads back ONLY [y0, y0+stripH] (rest of the tall buffer comes from the memmove'd overlap). */
void wpe_atlas_view_backend_scroll_hint(struct wpe_view_backend* wpe, int y0, int stripH, int delta)
{
    struct atlas_view* v = g_atlas_views;
    while (v && v->wpe != wpe) v = v->next;
    if (!v || !v->shm || v->shm == (void*)-1) return;
    struct atlas_ctrl* c = shm_ctrl(v->shm, v->width, v->height);
    c->y0 = y0; c->stripH = stripH; c->delta = delta;
    __sync_synchronize();               /* fields visible before the seq bump the WebProcess polls on */
    c->seq = c->seq + 1;
    __sync_synchronize();
}

/* Publish the playing <video>'s damage rect (device px, top-origin, 1:1). BrowserPageWPE polls the DOM
 * and calls this; the WebProcess reads it in target_frame_rendered for sub-rect readback. active=0 ends
 * it (resume full readback). Cheap enough to call every poll even when unchanged (seq guards the read). */
void wpe_atlas_view_backend_video_rect(struct wpe_view_backend* wpe, int x, int y, int w, int h, int active)
{
    struct atlas_view* v = g_atlas_views;
    while (v && v->wpe != wpe) v = v->next;
    if (!v || !v->shm || v->shm == (void*)-1) return;
    struct atlas_vrect* r = shm_vrect(v->shm, v->width, v->height);
    r->x = x; r->y = y; r->w = w; r->h = h; r->active = active;
    __sync_synchronize();               /* fields visible before the seq bump */
    r->seq = r->seq + 1;
    __sync_synchronize();
}

/* =========================================================================================== *
 *  UIProcess side: renderer host (general WebProcess connection — display only, minimal)        *
 * =========================================================================================== */
struct atlas_host { int dummy_a, dummy_b; };

static void* host_create(void)              { return calloc(1, sizeof(struct atlas_host)); }
static void  host_destroy(void* d)          { free(d); }
static int   host_create_client(void* d)
{
    /* WebKit passes this fd to the WebProcess as the renderer-backend-egl create() arg. We don't
     * carry data on it (the display is EGL_DEFAULT_DISPLAY), but it must be a valid fd. */
    struct atlas_host* h = (struct atlas_host*)d;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return -1;
    h->dummy_a = sv[0];   /* keep our end open so the peer doesn't HUP */
    return sv[1];
}
static struct wpe_renderer_host_interface s_host_iface = {
    host_create, host_destroy, host_create_client, NULL, NULL, NULL, NULL
};

/* =========================================================================================== *
 *  WebProcess side: renderer backend EGL (provides the native display)                          *
 * =========================================================================================== */
struct atlas_egl { int host_fd; };

static void* egl_create(int host_fd)
{
    struct atlas_egl* e = (struct atlas_egl*)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->host_fd = host_fd;   /* unused: display is EGL_DEFAULT_DISPLAY */
    return e;
}
static void egl_destroy(void* d) { free(d); }
static EGLNativeDisplayType egl_get_native_display(void* d)
{
    (void)d;
    /* >>> DEVICE: Adreno-220 webOS EGL. EGL_DEFAULT_DISPLAY is the usual fbdev/null choice; if the
     * device needs a specific native display handle (e.g. an fbdev fd or gbm device) set it here. */
    return (EGLNativeDisplayType)EGL_DEFAULT_DISPLAY;
}
static uint32_t egl_get_platform(void* d) { (void)d; return 0; }  /* 0 = default EGL platform */
static struct wpe_renderer_backend_egl_interface s_egl_iface = {
    egl_create, egl_destroy, egl_get_native_display, egl_get_platform, NULL, NULL, NULL
};

/* =========================================================================================== *
 *  WebProcess side: render target (native window + per-frame readback + IPC to UIProcess)       *
 * =========================================================================================== */
struct atlas_target {
    struct wpe_renderer_backend_egl_target* wpe;  /* for dispatch_frame_complete */
    int      host_fd;       /* the view_backend's peer socket (frames + acks) */
    int      shmid;
    uint8_t* shm;           /* SLOTS*slot_bytes, mapped from the UIProcess's shm */
    uint32_t width, height;
    uint32_t cur_slot;
    uint8_t* scratch;       /* width*height*4 readback scratch (RGBA, pre-swizzle) */
    uint8_t* master;        /* P2: persistent BGRA copy of the whole tall buffer (1:1 only); strip readback + overlap memmove happen here */
    uint32_t last_seq;      /* P2: last consumed atlas_ctrl.seq (0 = none yet) */
    int      force_full;    /* set on resize/rotation: master holds the OLD-orientation frame, so skip the
                             * strip/vrect (which memmove/splice stale content -> tearing) until a full
                             * readback re-seeds master at the new size. Cleared once master is re-seeded. */
    uint8_t* vscratch;      /* video sub-rect readback scratch (RGBA/BGRA, up to one video rect) */
    size_t   vscratch_sz;
    uint32_t vframe;        /* video sub-rect frames since the last full readback (periodic full refresh) */
    int      slots_to_seed; /* video path: after a full/strip readback, this many upcoming vrect frames must
                             * full-copy master->slot to seed each slot's static (non-video) content; once
                             * both SLOTS are seeded we copy ONLY the changed video rows (~0.9MB vs 12.5MB). */
    GLuint   fbo, tex, depth;   /* offscreen render target WebKit composites into (surfaceless ctx) */
    uint32_t fbo_w, fbo_h;
    /* ---- fit-to-screen downscale (see wpe_atlas_view_backend_create) ---- */
    uint32_t swidth, sheight;   /* scaled output size (0 = no downscale) */
    int      ds_state;          /* -1 unknown, 1 GPU pass ready, 0 GPU unavailable -> CPU bilinear */
    GLuint   ds_prog, ds_fbo, ds_tex;   /* downscale program + output FBO/texture */
    GLint    ds_uSampler;
    uint32_t ds_w, ds_h;        /* current ds_fbo/ds_tex size */
    uint8_t* ds_scratch;        /* layout-size RGBA readback scratch for the CPU fallback */
    /* ---- C: EGL_KHR_lock_surface fast readback (map GPU pixels to CPU, skip glReadPixels) ---- */
    EGLSurface lock_surf;       /* lockable pbuffer we blit the FBO into */
    EGLContext lock_ctx;        /* shares WebKit's context so it can sample t->tex */
    uint32_t   lock_w, lock_h;  /* current pbuffer size */
    int        lock_state;      /* -1 untried, 1 ready, 0 unavailable -> fall back to glReadPixels */
    void*      lock_cfg;        /* EGLConfig chosen for the lockable pbuffer (reused by the vrect sub-rect) */
    EGLSurface vlock_surf;      /* small lockable pbuffer for the video sub-rect (vrect fast path) */
    uint32_t   vlock_w, vlock_h;
};

static bool target_read_setup(struct atlas_target* t)
{
    struct atlas_msg m;
    ssize_t n = recv(t->host_fd, &m, sizeof(m), 0);   /* blocks until UIProcess sent SETUP */
    if (n != (ssize_t)sizeof(m) || m.type != MSG_SETUP) {
        ATLAS_LOG("target: bad SETUP (n=%zd)", n); return false;
    }
    t->shmid = (int)m.shmid; t->width = m.width; t->height = m.height;
    t->swidth = m.swidth; t->sheight = m.sheight;
    t->ds_state = -1;
    t->shm = (uint8_t*)shmat(t->shmid, NULL, 0);
    if (t->shm == (void*)-1) { ATLAS_LOG("target shmat failed: %s", strerror(errno)); t->shm = NULL; return false; }
    t->scratch = (uint8_t*)malloc(slot_bytes(t->width, t->height));
    if (t->swidth && t->sheight)
        ATLAS_LOG("target: fit-to-screen downscale %ux%u -> %ux%u", t->width, t->height, t->swidth, t->sheight);
    return true;
}

static void* target_create(struct wpe_renderer_backend_egl_target* wpe, int host_fd)
{
    struct atlas_target* t = (struct atlas_target*)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->wpe = wpe; t->host_fd = host_fd; t->shmid = -1;
    t->lock_state = -1;   /* -1 = untried (calloc's 0 would mean "unavailable" and skip the probe) */
    ATLAS_LOG("target_create (host_fd=%d)", host_fd);
    return t;
}
static void target_destroy(void* d)
{
    struct atlas_target* t = (struct atlas_target*)d;
    if (t->shm && t->shm != (void*)-1) shmdt(t->shm);
    free(t->scratch);
    free(t->ds_scratch);
    free(t);
}
static void target_initialize(void* d, void* backend, uint32_t width, uint32_t height)
{
    struct atlas_target* t = (struct atlas_target*)d;
    (void)backend;
    t->width = width; t->height = height;
    ATLAS_LOG("target_initialize %ux%u", width, height);
    if (!target_read_setup(t)) ATLAS_LOG("target_initialize: no shared buffer");
    /* width/height from SETUP should match; trust WebKit's here for the GL viewport size. */
    t->width = width; t->height = height;
}
static EGLNativeWindowType target_get_native_window(void* d)
{
    (void)d;
    /* >>> DEVICE: THE key decision. Returning 0 makes WebKit try, in order:
     *   1. EGL_KHR_surfaceless_context (renders to an FBO; swapBuffers is a no-op)
     *   2. the WPE offscreen target (eglCreateWindowSurface on its native window)
     *   3. eglCreatePbufferSurface
     * Start here (0) on-device; if the Adreno lacks surfaceless + the pbuffer fallback is 1x1,
     * provide a real offscreen native window instead (fbdev window / gbm_surface / a sized
     * pbuffer-config window) and read it back the same way. */
    return (EGLNativeWindowType)0;
}
static void target_resize(void* d, uint32_t width, uint32_t height)
{
    struct atlas_target* t = (struct atlas_target*)d;
    if (width == t->width && height == t->height) return;
    ATLAS_LOG("target_resize %ux%u -> %ux%u", t->width, t->height, width, height);
    t->width = width; t->height = height;
    t->force_full = 1;   /* master now holds the old-orientation frame; force a full re-seed before any strip/vrect */
    t->vframe = 0;       /* video seed too */
    /* The offscreen FBO and the lock-surface pbuffer both recreate themselves when they notice the size
     * change (frame_will_render checks fbo_w/h; the lock path checks lock_w/h). The readback scratch/master
     * are sized to the fixed, max-size shm slot which the UIProcess guards from shrinking, so they stay big
     * enough. Just reset the fit-to-screen downscale so it rebuilds at the new layout size. */
    t->ds_state = -1;
}
/* WebKit's surfaceless GL context has no default framebuffer (FB 0 is incomplete), so we provide a
 * real offscreen FBO and bind it here. TextureMapperGL::beginPainting() captures GL_FRAMEBUFFER_BINDING
 * as its render target, so binding our FBO before the compositor paints makes it render the whole page
 * into it; frame_rendered then reads it back. (Bind also survives the surfaceless OR 1x1-pbuffer path.) */
/* Phase 0 zero-copy probe (env BPWPE_GLPROBE=1): the make-or-break test for the EGLImage zero-copy
 * display path — can THIS Adreno 220 driver create an EGLImage from a GL texture and import it into
 * another texture via glEGLImageTargetTexture2DOES? (Documented to fail GL_INVALID_OPERATION on some
 * Adreno 2xx driver builds.) Also logs GL_IMPLEMENTATION_COLOR_READ_FORMAT so Phase 1 can format-match
 * the readback and drop the NEON RGBA->BGRA swizzle. Runs once, in the live WebProcess GL context. */
#ifndef EGL_IMAGE_PRESERVED_KHR
#define EGL_IMAGE_PRESERVED_KHR 0x30D2
#endif
#ifndef EGL_GL_TEXTURE_2D_KHR
#define EGL_GL_TEXTURE_2D_KHR 0x30B1
#endif
static void atlas_gl_probe(EGLDisplay dpy)
{
    while (glGetError() != GL_NO_ERROR) { }
    GLint rf = 0, rt = 0;
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &rf);
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &rt);
    ATLAS_LOG("PROBE read_format=0x%x read_type=0x%x (BGRA_EXT=0x80E1 => swizzle-free; RGBA=0x1908, UBYTE=0x1401)", rf, rt);

    PFNEGLCREATEIMAGEKHRPROC  c_img = (PFNEGLCREATEIMAGEKHRPROC)  eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC d_img = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC t_img =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress("glEGLImageTargetTexture2DOES");
    ATLAS_LOG("PROBE procs eglCreateImageKHR=%p glEGLImageTargetTexture2DOES=%p", (void*)c_img, (void*)t_img);
    if (!c_img || !t_img) { ATLAS_LOG("PROBE VERDICT: EGLImage procs MISSING -> zero-copy (Phase 2a) NOT viable"); return; }

    GLuint src = 0; glGenTextures(1, &src); glBindTexture(GL_TEXTURE_2D, src);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    while (glGetError() != GL_NO_ERROR) { }

    EGLint attr[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    EGLImageKHR img = c_img(dpy, eglGetCurrentContext(), EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(uintptr_t)src, attr);
    ATLAS_LOG("PROBE eglCreateImageKHR(GL_TEXTURE_2D) -> %p eglerr=0x%x", (void*)img, eglGetError());
    if (img == EGL_NO_IMAGE_KHR) {
        ATLAS_LOG("PROBE VERDICT: eglCreateImageKHR FAILED -> EGLImage-from-GL-texture NOT viable (try native-buffer/QUALCOMM path)");
        glDeleteTextures(1, &src); return;
    }
    GLuint dst = 0; glGenTextures(1, &dst); glBindTexture(GL_TEXTURE_2D, dst);
    while (glGetError() != GL_NO_ERROR) { }
    t_img(GL_TEXTURE_2D, (GLeglImageOES)img);
    GLenum e = glGetError();
    ATLAS_LOG("PROBE glEGLImageTargetTexture2DOES -> glerr=0x%x  ==> %s", e,
              e == GL_NO_ERROR ? "*** IMPORT OK - ZERO-COPY (Phase 2a) VIABLE ***"
                               : "*** IMPORT FAILED (Adreno GL_INVALID_OPERATION 0x502?) - use Phase 2b MDP overlay ***");
    if (d_img) d_img(dpy, img);
    glDeleteTextures(1, &src); glDeleteTextures(1, &dst);
}

static void target_frame_will_render(void* d)
{
    struct atlas_target* t = (struct atlas_target*)d;
    { static int wr_n = 0; if (wr_n < 3 || wr_n % 30 == 0) ATLAS_LOG("frame_will_render #%d (%ux%u)", wr_n, t->width, t->height); wr_n++; }
    if (!t->fbo || t->fbo_w != t->width || t->fbo_h != t->height) {
        if (t->fbo)   glDeleteFramebuffers(1, &t->fbo);
        if (t->tex)   glDeleteTextures(1, &t->tex);
        if (t->depth) glDeleteRenderbuffers(1, &t->depth);
        {   /* P0: GL caps — the Adreno 220 GLES2 texture/renderbuffer cap gates the tall scroll buffer */
            static int s_caps = 0;
            if (!s_caps) { s_caps = 1;
                GLint mt = 0, mr = 0, mv[2] = {0, 0};
                glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mt);
                glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &mr);
                glGetIntegerv(GL_MAX_VIEWPORT_DIMS, mv);
                ATLAS_LOG("GLCAPS max_texture=%d max_renderbuffer=%d max_viewport=%dx%d renderer='%s'",
                         mt, mr, mv[0], mv[1], (const char*)glGetString(GL_RENDERER));
                /* C-investigation: which zero-copy / fast-readback paths exist? Look for
                 * OES_EGL_image, EXT_texture_format_BGRA8888, OES_mapbuffer, *pixel_buffer*, and on the
                 * EGL side KHR_image / ANDROID_image_native_buffer (gralloc/dmabuf-like map). */
                const char* ge = (const char*)glGetString(GL_EXTENSIONS);
                ATLAS_LOG("GLEXT: %.1200s", ge ? ge : "(null)");
                EGLDisplay ed = eglGetCurrentDisplay();
                if (ed != EGL_NO_DISPLAY) {
                    const char* ee = eglQueryString(ed, EGL_EXTENSIONS);
                    ATLAS_LOG("EGLEXT: %.1000s", ee ? ee : "(null)");
                    if (getenv("BPWPE_GLPROBE")) atlas_gl_probe(ed);
                }
            }
        }
        glGenTextures(1, &t->tex);
        glBindTexture(GL_TEXTURE_2D, t->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)t->width, (GLsizei)t->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenRenderbuffers(1, &t->depth);
        glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, (GLsizei)t->width, (GLsizei)t->height);
        glGenFramebuffers(1, &t->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) ATLAS_LOG("target FBO incomplete: 0x%x (%ux%u)", st, t->width, t->height);
        t->fbo_w = t->width; t->fbo_h = t->height;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, (GLsizei)t->width, (GLsizei)t->height);
}

#include <arm_neon.h>
/* RGBA -> ARGB32-premul BGRA (swap R<->B, keep G,A; NO vertical flip — WebKit's WPE FBO is already
 * top-down on this stack, verified on-device). NEON-vectorised: vld4q deinterleaves the 4 channels,
 * we swap the R and B planes, vst4q re-interleaves — 16 px/iter vs the old scalar per-pixel shuffle
 * (~4x on Cortex-A8). This runs every frame because the Adreno (RGBA-native) rejects BGRA readback. */
static void swizzle_flip(const uint8_t* rgba, uint8_t* bgra, uint32_t w, uint32_t h)
{
    size_t n = (size_t)w * h, i = 0;
    for (; i + 16 <= n; i += 16) {
        uint8x16x4_t px = vld4q_u8(rgba + i * 4);   /* val[0]=R val[1]=G val[2]=B val[3]=A */
        uint8x16_t r = px.val[0];
        px.val[0] = px.val[2];                       /* out ch0 = B */
        px.val[2] = r;                               /* out ch2 = R */
        vst4q_u8(bgra + i * 4, px);                  /* interleave back as B G R A */
    }
    for (; i < n; ++i) {                             /* scalar tail (< 16 px) */
        bgra[i*4+0] = rgba[i*4+2]; bgra[i*4+1] = rgba[i*4+1];
        bgra[i*4+2] = rgba[i*4+0]; bgra[i*4+3] = rgba[i*4+3];
    }
}

static int s_bgra_readback = -1;   /* -1 unknown, 1 driver can readback BGRA directly, 0 must swizzle */

/* ---- fit-to-screen downscale (GPU pass; CPU bilinear fallback) ------------------------------ *
 * WebKit composites the page into t->fbo at the LAYOUT size (t->width x t->height). To satisfy the
 * legacy BrowserOffscreenInfo contract the OFFSCREEN BUFFER must already hold the page scaled to the
 * SCREEN size, so we downscale t->tex into a smaller output before readback. Preferred path: a
 * textured-quad blit into ds_fbo (Adreno 220 GLES2, LINEAR sampling). If the program/FBO can't be
 * built the CPU bilinear fallback keeps the SAME output dims, so the contract holds either way. */
static GLuint atlas_compile_shader(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[256] = {0}; glGetShaderInfoLog(s, sizeof(log) - 1, NULL, log);
               ATLAS_LOG("downscale shader compile failed: %s", log); glDeleteShader(s); return 0; }
    return s;
}

/* Build the downscale program once. Sets t->ds_state to 1 (GPU) or 0 (CPU fallback). */
static void ds_init_gpu(struct atlas_target* t)
{
    static const char* VS =
        "attribute vec2 a_pos;\n attribute vec2 a_uv;\n varying vec2 v_uv;\n"
        "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    static const char* FS =
        "precision mediump float;\n uniform sampler2D u_tex;\n varying vec2 v_uv;\n"
        "void main(){ gl_FragColor = texture2D(u_tex, v_uv); }\n";
    GLuint vs = atlas_compile_shader(GL_VERTEX_SHADER, VS);
    GLuint fs = atlas_compile_shader(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) { t->ds_state = 0; if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return; }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_uv");
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[256] = {0}; glGetProgramInfoLog(p, sizeof(log) - 1, NULL, log);
               ATLAS_LOG("downscale program link failed: %s", log); glDeleteProgram(p); t->ds_state = 0; return; }
    t->ds_prog = p;
    t->ds_uSampler = glGetUniformLocation(p, "u_tex");
    t->ds_state = 1;
    ATLAS_LOG("downscale: GPU pass ready");
}

/* Ensure the ds_fbo/ds_tex output target exists at swidth x sheight. Returns false on failure. */
static bool ds_ensure_target(struct atlas_target* t)
{
    if (t->ds_fbo && t->ds_w == t->swidth && t->ds_h == t->sheight) return true;
    if (t->ds_fbo) glDeleteFramebuffers(1, &t->ds_fbo);
    if (t->ds_tex) glDeleteTextures(1, &t->ds_tex);
    t->ds_fbo = 0; t->ds_tex = 0;
    glGenTextures(1, &t->ds_tex);
    glBindTexture(GL_TEXTURE_2D, t->ds_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)t->swidth, (GLsizei)t->sheight, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &t->ds_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->ds_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->ds_tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) { ATLAS_LOG("downscale FBO incomplete: 0x%x", st); return false; }
    t->ds_w = t->swidth; t->ds_h = t->sheight;
    return true;
}

/* GPU downscale: blit t->tex (layout FBO colour) into ds_fbo at swidth x sheight. On success ds_fbo
 * is left bound for the subsequent readback and true is returned. Orientation is preserved (uv maps
 * 1:1 to the target's GL space) so the readback row order matches the direct-FBO path. */
static bool ds_gpu_pass(struct atlas_target* t)
{
    if (!ds_ensure_target(t)) return false;
    static const GLfloat quad[] = {   /* x, y, u, v  (TRIANGLE_STRIP) */
        -1.f, -1.f, 0.f, 0.f,
         1.f, -1.f, 1.f, 0.f,
        -1.f,  1.f, 0.f, 1.f,
         1.f,  1.f, 1.f, 1.f,
    };
    glBindFramebuffer(GL_FRAMEBUFFER, t->ds_fbo);
    glViewport(0, 0, (GLsizei)t->swidth, (GLsizei)t->sheight);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(t->ds_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->tex);       /* the layout FBO colour texture (LINEAR filtered) */
    glUniform1i(t->ds_uSampler, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);           /* use client arrays, not any VBO WebKit left bound */
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    return true;
}

/* CPU bilinear fallback: resample srcRGBA (sw0 x sh0) down to dstRGBA (sw1 x sh1). */
static void cpu_bilinear(const uint8_t* src, uint32_t sw0, uint32_t sh0,
                         uint8_t* dst, uint32_t sw1, uint32_t sh1)
{
    float fx = (float)sw0 / sw1, fy = (float)sh0 / sh1;
    for (uint32_t y = 0; y < sh1; ++y) {
        float sy = (y + 0.5f) * fy - 0.5f; if (sy < 0) sy = 0;
        uint32_t y0 = (uint32_t)sy; uint32_t y1 = y0 + 1 < sh0 ? y0 + 1 : y0;
        float wy = sy - y0;
        const uint8_t* r0 = src + (size_t)y0 * sw0 * 4;
        const uint8_t* r1 = src + (size_t)y1 * sw0 * 4;
        uint8_t* drow = dst + (size_t)y * sw1 * 4;
        for (uint32_t x = 0; x < sw1; ++x) {
            float sx = (x + 0.5f) * fx - 0.5f; if (sx < 0) sx = 0;
            uint32_t x0 = (uint32_t)sx; uint32_t x1 = x0 + 1 < sw0 ? x0 + 1 : x0;
            float wx = sx - x0;
            const uint8_t* p00 = r0 + (size_t)x0 * 4; const uint8_t* p01 = r0 + (size_t)x1 * 4;
            const uint8_t* p10 = r1 + (size_t)x0 * 4; const uint8_t* p11 = r1 + (size_t)x1 * 4;
            uint8_t* o = drow + (size_t)x * 4;
            for (int c = 0; c < 4; ++c) {
                float top = p00[c] * (1 - wx) + p01[c] * wx;
                float bot = p10[c] * (1 - wx) + p11[c] * wx;
                o[c] = (uint8_t)(top * (1 - wy) + bot * wy + 0.5f);
            }
        }
    }
}

/* C: fast readback via EGL_KHR_lock_surface. Blit the FBO colour (t->tex) into a lockable pbuffer,
 * map its pixels to a CPU pointer, and swizzle-copy into the slot — skipping glReadPixels' GPU->CPU
 * de-tile/convert (the ~1s wall). A context sharing WebKit's lets us sample t->tex; we restore
 * WebKit's current surface/context before returning. Gated by /tmp/atlas_locksurf (or BPWPE_LOCKSURF).
 * Returns false (and leaves lock_state=0) on any failure so the caller falls back to glReadPixels. */
static bool lock_readback(struct atlas_target* t, uint8_t* slot, uint32_t rw, uint32_t rh, long* out_us)
{
    static PFNEGLLOCKSURFACEKHRPROC   p_lock   = NULL;
    static PFNEGLUNLOCKSURFACEKHRPROC p_unlock = NULL;
    static int s_proc = 0;
    if (!s_proc) { s_proc = 1;
        p_lock   = (PFNEGLLOCKSURFACEKHRPROC)  eglGetProcAddress("eglLockSurfaceKHR");
        p_unlock = (PFNEGLUNLOCKSURFACEKHRPROC)eglGetProcAddress("eglUnlockSurfaceKHR");
    }
    if (t->lock_state == 0 || !p_lock || !p_unlock) { if (t->lock_state < 0) { ATLAS_LOG("locksurf: no eglLockSurfaceKHR"); t->lock_state = 0; } return false; }

    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext webctx = eglGetCurrentContext();
    EGLSurface pDraw = eglGetCurrentSurface(EGL_DRAW), pRead = eglGetCurrentSurface(EGL_READ);
    if (dpy == EGL_NO_DISPLAY || webctx == EGL_NO_CONTEXT) return false;

    if (t->ds_state < 0) ds_init_gpu(t);                 /* need the blit program (shared via lock_ctx) */
    if (!t->ds_prog) { ATLAS_LOG("locksurf: no blit program"); t->lock_state = 0; return false; }

    if (t->lock_state < 0 || t->lock_w != rw || t->lock_h != rh) {
        if (t->lock_surf) { eglDestroySurface(dpy, t->lock_surf); t->lock_surf = 0; }
        /* Enumerate what the driver actually offers (once) — pbuffer+lock+exact-RGBA didn't match. */
        { EGLint qa[] = { EGL_SURFACE_TYPE, EGL_LOCK_SURFACE_BIT_KHR, EGL_NONE };
          EGLConfig cs[32]; EGLint nc = 0; eglChooseConfig(dpy, qa, cs, 32, &nc);
          ATLAS_LOG("locksurf: %d configs w/ LOCK_SURFACE_BIT", nc);
          for (int i = 0; i < nc && i < 6; i++) { EGLint st=0,r=0,g=0,b=0,a=0,mf=0;
            eglGetConfigAttrib(dpy,cs[i],EGL_SURFACE_TYPE,&st); eglGetConfigAttrib(dpy,cs[i],EGL_RED_SIZE,&r);
            eglGetConfigAttrib(dpy,cs[i],EGL_GREEN_SIZE,&g); eglGetConfigAttrib(dpy,cs[i],EGL_BLUE_SIZE,&b);
            eglGetConfigAttrib(dpy,cs[i],EGL_ALPHA_SIZE,&a); eglGetConfigAttrib(dpy,cs[i],EGL_MATCH_FORMAT_KHR,&mf);
            ATLAS_LOG("  lockcfg[%d] surftype=0x%x rgba=%d/%d/%d/%d matchfmt=0x%x", i, st, r,g,b,a, mf); } }
        /* Try progressively looser attribs; use whichever yields a pbuffer-capable lockable config. */
        EGLConfig cfg; EGLint n = 0;
        EGLint a1[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT | EGL_LOCK_SURFACE_BIT_KHR, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                        EGL_MATCH_FORMAT_KHR, EGL_FORMAT_RGBA_8888_EXACT_KHR, EGL_NONE };
        EGLint a2[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT | EGL_LOCK_SURFACE_BIT_KHR, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8, EGL_NONE };
        EGLint a3[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT | EGL_LOCK_SURFACE_BIT_KHR, EGL_NONE };
        if      (eglChooseConfig(dpy, a1, &cfg, 1, &n) && n>=1) ATLAS_LOG("locksurf: cfg via exact-RGBA match");
        else if (eglChooseConfig(dpy, a2, &cfg, 1, &n) && n>=1) ATLAS_LOG("locksurf: cfg via rgba8 sizes");
        else if (eglChooseConfig(dpy, a3, &cfg, 1, &n) && n>=1) ATLAS_LOG("locksurf: cfg via pbuffer+lock only");
        else { ATLAS_LOG("locksurf: no pbuffer-lockable config"); t->lock_state = 0; return false; }
        EGLint sa[] = { EGL_WIDTH, (EGLint)rw, EGL_HEIGHT, (EGLint)rh, EGL_NONE };
        t->lock_surf = eglCreatePbufferSurface(dpy, cfg, sa);
        if (t->lock_surf == EGL_NO_SURFACE) { ATLAS_LOG("locksurf: pbuffer fail 0x%x", eglGetError()); t->lock_state = 0; return false; }
        if (!t->lock_ctx) {
            EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
            t->lock_ctx = eglCreateContext(dpy, cfg, webctx, xa);   /* share WebKit's ctx -> sample t->tex */
            if (t->lock_ctx == EGL_NO_CONTEXT) { ATLAS_LOG("locksurf: ctx fail 0x%x", eglGetError()); t->lock_state = 0; return false; }
        }
        t->lock_cfg = (void*)cfg;   /* reused by the vrect sub-rect pbuffer */
        t->lock_w = rw; t->lock_h = rh; t->lock_state = 1;
        ATLAS_LOG("locksurf: ready %ux%u", rw, rh);
    }

    struct timeval a, b; gettimeofday(&a, NULL);
    glFinish();   /* WebKit's render into t->tex must COMPLETE + be visible to the shared lock_ctx,
                   * else the blit samples an incomplete/empty texture -> black frames. glReadPixels
                   * did this implicitly; our blit path must do it explicitly. */
    if (!eglMakeCurrent(dpy, t->lock_surf, t->lock_surf, t->lock_ctx)) {
        ATLAS_LOG("locksurf: makeCurrent fail 0x%x", eglGetError());
        eglMakeCurrent(dpy, pDraw, pRead, webctx); return false;
    }
    /* blit t->tex -> pbuffer default framebuffer (same quad/orientation as ds_gpu_pass) */
    static const GLfloat quad[] = { -1.f,-1.f,0.f,0.f,  1.f,-1.f,1.f,0.f,  -1.f,1.f,0.f,1.f,  1.f,1.f,1.f,1.f };
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, (GLsizei)rw, (GLsizei)rh);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glUseProgram(t->ds_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glUniform1i(t->ds_uSampler, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0); glDisableVertexAttribArray(1);
    glFinish();   /* blit must complete before we lock/map the surface */

    EGLint la[] = { EGL_MAP_PRESERVE_PIXELS_KHR, EGL_TRUE, EGL_LOCK_USAGE_HINT_KHR, EGL_READ_SURFACE_BIT_KHR, EGL_NONE };
    if (!p_lock(dpy, t->lock_surf, la)) {
        ATLAS_LOG("locksurf: lock fail 0x%x", eglGetError());
        eglMakeCurrent(dpy, pDraw, pRead, webctx); return false;
    }
    EGLint ptr_i = 0, pitch = 0, origin = EGL_LOWER_LEFT_KHR;
    eglQuerySurface(dpy, t->lock_surf, EGL_BITMAP_POINTER_KHR, &ptr_i);
    eglQuerySurface(dpy, t->lock_surf, EGL_BITMAP_PITCH_KHR, &pitch);
    eglQuerySurface(dpy, t->lock_surf, EGL_BITMAP_ORIGIN_KHR, &origin);
    uint8_t* ptr = (uint8_t*)(uintptr_t)ptr_i;
    bool ok = false;
    if (ptr && pitch > 0) {
        /* The EGL-locked bitmap is delivered as BGRA on this Adreno — i.e. ALREADY the adapter's format —
         * NOT RGBA. Verified: a solid-red page rendered blue with swizzle_flip here (double-swap) and
         * correct via the RGBA glReadPixels path. So COPY straight through (no R<->B swap); only the
         * vertical flip is needed. content-top at GL bottom: LOWER_LEFT map row0 = content top (no flip);
         * UPPER_LEFT map row0 = content bottom (flip). (glReadPixels/strip/CPU paths DO read RGBA -> keep swizzle.) */
        for (uint32_t y = 0; y < rh; y++) {
            uint32_t sy = (origin == EGL_UPPER_LEFT_KHR) ? (rh - 1 - y) : y;
            memcpy(slot + (size_t)y * rw * 4, ptr + (size_t)sy * pitch, (size_t)rw * 4);
        }
        ok = true;
    }
    p_unlock(dpy, t->lock_surf);
    eglMakeCurrent(dpy, pDraw, pRead, webctx);
    gettimeofday(&b, NULL);
    if (out_us) *out_us = (b.tv_sec - a.tv_sec) * 1000000L + (b.tv_usec - a.tv_usec);
    return ok;
}

/* Sub-rect variant of lock_readback for the video damage rect: blit ONLY [vx,vy,vw,vh] of t->tex into a
 * small lockable pbuffer, map it, and copy into dst (the master buffer, stride rw*4) at (vx,vy). Reuses
 * lock_ctx/lock_cfg warmed up by the full lock path (a seed/periodic-full frame runs it first). ~vw*vh
 * work instead of the whole frame, and it maps GPU memory directly — skipping glReadPixels' ~200ms fixed
 * GPU->CPU stall. Orientation matches the full path: slot(X,Y)=texture(X,Y) via the same v-texcoords +
 * origin flip (derived over vh). The locked bitmap is BGRA already (like the full path) — no swizzle.
 * Returns false on any failure so the caller falls back to the glReadPixels sub-rect. */
static bool lock_readback_rect(struct atlas_target* t, uint8_t* dst, uint32_t rw, uint32_t rh,
                               int vx, int vy, int vw, int vh, long* out_us)
{
    static PFNEGLLOCKSURFACEKHRPROC   p_lock   = NULL;
    static PFNEGLUNLOCKSURFACEKHRPROC p_unlock = NULL;
    static int s_proc = 0;
    if (!s_proc) { s_proc = 1;
        p_lock   = (PFNEGLLOCKSURFACEKHRPROC)  eglGetProcAddress("eglLockSurfaceKHR");
        p_unlock = (PFNEGLUNLOCKSURFACEKHRPROC)eglGetProcAddress("eglUnlockSurfaceKHR");
    }
    if (!p_lock || !p_unlock || !t->lock_ctx || !t->lock_cfg || !t->ds_prog) return false;  /* full path not warmed up yet */
    if (vw <= 0 || vh <= 0) return false;

    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext webctx = eglGetCurrentContext();
    EGLSurface pDraw = eglGetCurrentSurface(EGL_DRAW), pRead = eglGetCurrentSurface(EGL_READ);
    if (dpy == EGL_NO_DISPLAY || webctx == EGL_NO_CONTEXT) return false;

    /* Lockable pbuffer sized to the SUB-RECT (vw x vh), NOT the full buffer. eglLockSurfaceKHR maps/syncs
     * the WHOLE surface, so a full 1024x3072 pbuffer cost ~150ms per lock even for a 640x360 read (measured:
     * lock+copy=135-161ms). Sizing it to the actual rect shrinks the mapped area ~13x -> the lock drops to
     * ~10-15ms. Cached; recreated only when the rect size changes (the video rect is stable -> created once;
     * scroll strips vary in height -> occasional recreate, still a big net win vs the full-surface lock).
     * The blit fills the whole pbuffer (viewport 0,0,vw,vh); map + copy all vh rows below. */
    if (!t->vlock_surf || t->vlock_w != (uint32_t)vw || t->vlock_h != (uint32_t)vh) {
        if (t->vlock_surf) { eglDestroySurface(dpy, t->vlock_surf); t->vlock_surf = 0; }
        EGLint sa[] = { EGL_WIDTH, vw, EGL_HEIGHT, vh, EGL_NONE };
        t->vlock_surf = eglCreatePbufferSurface(dpy, (EGLConfig)t->lock_cfg, sa);
        if (t->vlock_surf == EGL_NO_SURFACE) { t->vlock_surf = 0; return false; }
        t->vlock_w = (uint32_t)vw; t->vlock_h = (uint32_t)vh;
    }

    struct timeval a, aF, aB, b; gettimeofday(&a, NULL);
    glFinish();   /* WebKit's render into t->tex must complete before the shared lock_ctx samples it */
    gettimeofday(&aF, NULL);   /* SPLIT: time spent waiting on WebKit's composite (the suspected fps ceiling) */
    if (!eglMakeCurrent(dpy, t->vlock_surf, t->vlock_surf, t->lock_ctx)) {
        eglMakeCurrent(dpy, pDraw, pRead, webctx); return false;
    }
    float u0 = (float)vx / rw, u1 = (float)(vx + vw) / rw;
    float v0 = (float)vy / rh, v1 = (float)(vy + vh) / rh;
    const GLfloat quad[] = { -1.f,-1.f,u0,v0,  1.f,-1.f,u1,v0,  -1.f,1.f,u0,v1,  1.f,1.f,u1,v1 };
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, vw, vh);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glUseProgram(t->ds_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glUniform1i(t->ds_uSampler, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0); glDisableVertexAttribArray(1);
    glFinish();
    gettimeofday(&aB, NULL);   /* SPLIT: our sub-rect blit into the pbuffer */

    EGLint la[] = { EGL_MAP_PRESERVE_PIXELS_KHR, EGL_TRUE, EGL_LOCK_USAGE_HINT_KHR, EGL_READ_SURFACE_BIT_KHR, EGL_NONE };
    bool ok = false;
    if (p_lock(dpy, t->vlock_surf, la)) {
        EGLint ptr_i = 0, pitch = 0, origin = EGL_LOWER_LEFT_KHR;
        eglQuerySurface(dpy, t->vlock_surf, EGL_BITMAP_POINTER_KHR, &ptr_i);
        eglQuerySurface(dpy, t->vlock_surf, EGL_BITMAP_PITCH_KHR, &pitch);
        eglQuerySurface(dpy, t->vlock_surf, EGL_BITMAP_ORIGIN_KHR, &origin);
        uint8_t* ptr = (uint8_t*)(uintptr_t)ptr_i;
        if (ptr && pitch > 0) {
            size_t stride = (size_t)rw * 4;
            /* The sub-rect was blitted to fill the whole vw x vh pbuffer (GL viewport 0,0,vw,vh). An
             * UPPER_LEFT bitmap has its row 0 at the pbuffer TOP -> the blitted content's row y is at map
             * row (vh-1-y). LOWER_LEFT: row 0 is the bottom corner -> row y. dst is the full-width master. */
            for (int y = 0; y < vh; y++) {
                int sy = (origin == EGL_UPPER_LEFT_KHR) ? (vh - 1 - y) : y;
                memcpy(dst + (size_t)(vy + y) * stride + (size_t)vx * 4, ptr + (size_t)sy * pitch, (size_t)vw * 4);
            }
            ok = true;
        }
        p_unlock(dpy, t->vlock_surf);
    }
    eglMakeCurrent(dpy, pDraw, pRead, webctx);
    gettimeofday(&b, NULL);
    { static int _rn = 0; if (_rn++ % 30 == 0) ATLAS_LOG("LRR-SPLIT finish1(render-wait)=%ldus blit+finish2=%ldus lock+copy=%ldus (%dx%d)",
        (aF.tv_sec-a.tv_sec)*1000000L+(aF.tv_usec-a.tv_usec),
        (aB.tv_sec-aF.tv_sec)*1000000L+(aB.tv_usec-aF.tv_usec),
        (b.tv_sec-aB.tv_sec)*1000000L+(b.tv_usec-aB.tv_usec), vw, vh); }
    if (out_us) *out_us = (b.tv_sec - a.tv_sec) * 1000000L + (b.tv_usec - a.tv_usec);
    return ok;
}

/* DIAG: categorize why each frame took the fast (vrect) vs slow (full readback) path — dumped periodically
 * so we can see if frequent full readbacks come from doFull, an inactive/torn vrect, or a lock failure. */
static struct { unsigned vrect, dofull, gotfail, ina_active, ina_seq; } s_vstat;

static void target_frame_rendered(void* d)
{
    struct atlas_target* t = (struct atlas_target*)d;
    { static int fr_n = 0; if (fr_n < 3 || fr_n % 30 == 0) {
        ATLAS_LOG("frame_rendered #%d (%ux%u)", fr_n, t->width, t->height);
        if (fr_n % 60 == 0) ATLAS_LOG("VSTAT vrect=%u dofull=%u gotfail=%u inactive(active0=%u seqtear=%u)",
            s_vstat.vrect, s_vstat.dofull, s_vstat.gotfail, s_vstat.ina_active, s_vstat.ina_seq);
      } fr_n++; }
    struct timeval _pt0, _pt1; gettimeofday(&_pt0, NULL);
    static struct timeval _plast = {0, 0};
    if (t->shm) {
        /* Decide the output (readback) size: scaled if fit-to-screen is on, else the layout size. */
        bool wantScale = (t->swidth && t->sheight);
        uint32_t rw = wantScale ? t->swidth : t->width;
        uint32_t rh = wantScale ? t->sheight : t->height;
        uint8_t* slot = t->shm + (size_t)t->cur_slot * slot_bytes(t->width, t->height);
        bool gpuScaled = false;

        /* WebKit's GL context is current here; our FBO (bound in frame_will_render) is still the target. */
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        if (wantScale) {
            if (t->ds_state < 0) ds_init_gpu(t);         /* lazy one-time program build */
            if (t->ds_state == 1) gpuScaled = ds_gpu_pass(t);   /* leaves ds_fbo bound on success */
            if (!gpuScaled) {
                /* CPU fallback: read the full layout FBO (RGBA) then bilinear-resample to rw x rh. */
                if (!t->ds_scratch) t->ds_scratch = (uint8_t*)malloc(slot_bytes(t->width, t->height));
                glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
                glReadPixels(0, 0, (GLsizei)t->width, (GLsizei)t->height, GL_RGBA, GL_UNSIGNED_BYTE, t->ds_scratch);
                cpu_bilinear(t->ds_scratch, t->width, t->height, t->scratch, rw, rh);
                swizzle_flip(t->scratch, slot, rw, rh);   /* RGBA -> BGRA into the slot */
                goto sent;                                 /* slot already filled */
            }
        }

        /* ---- P2 strip readback (1:1, both BGRA-direct and RGBA+swizzle paths): if the BS bumped the
         * scroll-hint seq, read back ONLY the newly-exposed strip into the persistent master buffer and
         * memmove the overlap within it, then publish master -> slot. Turns the full-buffer readback
         * (~1.4s at 3072 rows on this Adreno) into ~stripH/bufH of that. Any invalid/oversized hint
         * (or a reflow beating the scroll frame) consumes the seq and falls through to the full read. */
        static int s_noStrip = -1;   /* isolate crashes: `touch /tmp/atlas_no_strip` disables the strip path */
        if (s_noStrip < 0) s_noStrip = (access("/tmp/atlas_no_strip", F_OK) == 0 || getenv("BPWPE_NO_STRIP")) ? 1 : 0;
        if (!wantScale && !s_noStrip && !t->force_full) {   /* NOTE: no s_bgra_readback gate — lock_surface is the default full path and never runs the probe, so it stays -1; the strip read below uses lock_readback_rect / RGBA+swizzle, neither of which needs it */
            if (!t->master) t->master = (uint8_t*)malloc(slot_bytes(t->width, t->height));
            struct atlas_ctrl* ctrl = shm_ctrl(t->shm, t->width, t->height);
            uint32_t seq = ctrl->seq;
            if (seq != t->last_seq) {
                t->last_seq = seq;                       /* consume, even if we fall back to full */
                __sync_synchronize();
                int y0 = ctrl->y0, sh = ctrl->stripH, delta = ctrl->delta;
                size_t stride = (size_t)rw * 4;
                int ad = delta > 0 ? delta : -delta;
                if (t->master && sh > 0 && y0 >= 0 &&
                    (uint32_t)(y0 + sh) <= rh && ad > 0 && (uint32_t)ad < rh) {
                    struct timeval _a, _b; gettimeofday(&_a, NULL);
                    if (delta > 0)      /* scrolled down: keep old rows [delta,rh) at [0,rh-delta) */
                        memmove(t->master, t->master + (size_t)delta * stride, (size_t)(rh - delta) * stride);
                    else                /* scrolled up:   keep old rows [0,rh-ad) at [ad,rh) */
                        memmove(t->master + (size_t)ad * stride, t->master, (size_t)(rh - ad) * stride);
                    /* Fast strip: lock_surface sub-rect maps GPU mem (~sh rows) instead of paying
                     * glReadPixels' ~300ms fixed stall (measured: a 133-row glReadPixels strip = 370ms!).
                     * The lock pbuffer is now full-size + reused, so no per-strip surface churn. Falls back
                     * to glReadPixels if the lock path isn't warmed up / is disabled (/tmp/atlas_no_vlock). */
                    long _sus = 0;
                    static int s_noVlockS = -1;
                    if (s_noVlockS < 0) s_noVlockS = (access("/tmp/atlas_no_vlock", F_OK) == 0 || getenv("BPWPE_NO_VLOCK")) ? 1 : 0;
                    if (s_noVlockS || !lock_readback_rect(t, t->master, rw, rh, 0, y0, (int)rw, sh, &_sus)) {
                        if (s_bgra_readback == 1) {
                            glReadPixels(0, y0, (GLsizei)rw, (GLsizei)sh, 0x80E1 /*GL_BGRA_EXT*/, GL_UNSIGNED_BYTE,
                                         t->master + (size_t)y0 * stride);
                        } else {        /* swizzle path: read the strip RGBA, swizzle into master at y0 */
                            glReadPixels(0, y0, (GLsizei)rw, (GLsizei)sh, GL_RGBA, GL_UNSIGNED_BYTE, t->scratch);
                            swizzle_flip(t->scratch, t->master + (size_t)y0 * stride, rw, (uint32_t)sh);
                        }
                    }
                    memcpy(slot, t->master, (size_t)rh * stride);
                    t->slots_to_seed = SLOTS - 1;   /* other slot(s) must re-seed static content before rect-only video copies */
                    gettimeofday(&_b, NULL);
                    { static int _sn = 0; if (_sn++ % 8 == 0) ATLAS_LOG("STRIP y0=%d h=%d delta=%d %ldus (%ux%u bgra=%d)",
                        y0, sh, delta, (_b.tv_sec-_a.tv_sec)*1000000L+(_b.tv_usec-_a.tv_usec), rw, rh, s_bgra_readback); }
                    goto sent;
                }
            }
        }

        /* Video damage-rect readback: while a <video> plays, only its rect changes, so read back ONLY
         * that sub-rect and splice into master (static rest of page comes from master). Turns the full
         * ~1024x3072 readback into ~vw*vh. A periodic FULL frame (every VFULL) refreshes non-video page
         * changes; the first active frame also falls through to full to seed master. Gated /tmp/atlas_no_vrect. */
        /* NOTE: do NOT gate on s_bgra_readback here — with lock_surface ON (the default full-readback
         * path) the BGRA probe below never runs, so it stays -1 forever. The sub-rect read below falls
         * back to RGBA+swizzle when bgra!=1, which needs no probe. */
        static int s_noVrect = -1;
        if (s_noVrect < 0) s_noVrect = (access("/tmp/atlas_no_vrect", F_OK) == 0 || getenv("BPWPE_NO_VRECT")) ? 1 : 0;
        if (!wantScale && !s_noVrect) {
            if (!t->master) t->master = (uint8_t*)malloc(slot_bytes(t->width, t->height));   /* strip path is bgra-gated & may never alloc it; seeded by the full/lock readback on doFull frames */
        }
        if (!wantScale && !s_noVrect && t->master && !t->force_full) {   /* force_full after a resize: re-seed master first */
            /* Periodic FULL readback to refresh non-video page content. During video the non-video area is
             * essentially static, and a full 1024x3072 readback costs ~300ms (a visible hitch), so keep this
             * rare — every VFULL frames. Env BPWPE_VFULL overrides for tuning. */
            static uint32_t VFULL = 0;
            if (!VFULL) { const char* e = getenv("BPWPE_VFULL"); VFULL = e ? (uint32_t)atoi(e) : 150; if (!VFULL) VFULL = 150; }
            struct atlas_vrect* vr = shm_vrect(t->shm, t->width, t->height);
            uint32_t vseq = vr->seq; __sync_synchronize();
            int active = vr->active, vx = vr->x, vy = vr->y, vw = vr->w, vh = vr->h;
            __sync_synchronize();
            if (vr->seq == vseq && active) {                 /* consistent (no writer tear) + a video is playing */
                if (vx < 0) { vw += vx; vx = 0; }            /* clamp rect to the buffer */
                if (vy < 0) { vh += vy; vy = 0; }
                if (vx + vw > (int)rw) vw = (int)rw - vx;
                if (vy + vh > (int)rh) vh = (int)rh - vy;
                bool doFull = (t->vframe == 0) || (t->vframe % VFULL == 0);   /* seed + periodic full refresh */
                t->vframe++;
                if (doFull) s_vstat.dofull++;
                if (!doFull && vw > 0 && vh > 0) {
                    size_t stride = (size_t)rw * 4;
                    struct timeval _va, _vb; gettimeofday(&_va, NULL);
                    long vus = 0; int used_lock = 0, got = 0;
                    /* Fast path: lock_surface sub-rect (maps GPU mem, ~vw*vh) — skips glReadPixels' ~200ms
                     * fixed stall. Falls back to glReadPixels if the lock path isn't warmed up / fails.
                     * Gate /tmp/atlas_no_vlock. */
                    static int s_noVlock = -1;
                    if (s_noVlock < 0) s_noVlock = (access("/tmp/atlas_no_vlock", F_OK) == 0 || getenv("BPWPE_NO_VLOCK")) ? 1 : 0;
                    if (!s_noVlock && lock_readback_rect(t, t->master, rw, rh, vx, vy, vw, vh, &vus)) { got = 1; used_lock = 1; }
                    if (!got) {                              /* glReadPixels fallback (robust, ~200ms) */
                        size_t need = (size_t)vw * vh * 4;
                        if (t->vscratch_sz < need) { free(t->vscratch); t->vscratch = (uint8_t*)malloc(need); t->vscratch_sz = t->vscratch ? need : 0; }
                        if (t->vscratch) {
                            if (s_bgra_readback == 1) {
                                glReadPixels(vx, vy, (GLsizei)vw, (GLsizei)vh, 0x80E1 /*GL_BGRA_EXT*/, GL_UNSIGNED_BYTE, t->vscratch);
                                for (int ry = 0; ry < vh; ry++)
                                    memcpy(t->master + (size_t)(vy+ry)*stride + (size_t)vx*4, t->vscratch + (size_t)ry*vw*4, (size_t)vw*4);
                            } else {                         /* swizzle each row into the strided master */
                                glReadPixels(vx, vy, (GLsizei)vw, (GLsizei)vh, GL_RGBA, GL_UNSIGNED_BYTE, t->vscratch);
                                for (int ry = 0; ry < vh; ry++)
                                    swizzle_flip(t->vscratch + (size_t)ry*vw*4, t->master + (size_t)(vy+ry)*stride + (size_t)vx*4, (uint32_t)vw, 1);
                            }
                            got = 1;
                        }
                    }
                    if (got) {
                        /* Publish master -> slot. The full 12.5MB copy is only needed to seed a slot's STATIC
                         * (non-video) content; once both SLOTS carry it, copy ONLY the changed video rows
                         * (~0.9MB) — non-video refreshes on doFull frames anyway, which re-arm slots_to_seed. */
                        if (t->slots_to_seed > 0) {
                            memcpy(slot, t->master, (size_t)rh * stride);
                            t->slots_to_seed--;
                        } else {
                            for (int ry = 0; ry < vh; ry++)
                                memcpy(slot   + (size_t)(vy+ry)*stride + (size_t)vx*4,
                                       t->master + (size_t)(vy+ry)*stride + (size_t)vx*4, (size_t)vw*4);
                        }
                        s_vstat.vrect++;
                        gettimeofday(&_vb, NULL);
                        { static int _vn = 0; if (_vn++ % 30 == 0) ATLAS_LOG("VRECT %d,%d %dx%d %ldus lock=%d seed=%d (buf %ux%u)",
                            vx, vy, vw, vh, (_vb.tv_sec-_va.tv_sec)*1000000L+(_vb.tv_usec-_va.tv_usec), used_lock, t->slots_to_seed, rw, rh); }
                        goto sent;                           /* else fall through to a full readback (seed/periodic) */
                    }
                    if (!got) s_vstat.gotfail++;
                }
            } else {
                if (!active) s_vstat.ina_active++; else s_vstat.ina_seq++;   /* categorize the fall-through */
                t->vframe = 0;                               /* no video → next active frame seeds master with a full read */
            }
        }

        /* C: lock_surface fast readback (full 1:1 frame). Gated by /tmp/atlas_locksurf. On success it
         * bypasses glReadPixels entirely; on any failure it disables itself and falls through below. */
        static int s_locksurf = -1;   /* default ON (~3x faster full readback, verified correct); off via /tmp/atlas_no_locksurf */
        if (s_locksurf < 0) s_locksurf = (access("/tmp/atlas_no_locksurf", F_OK) == 0 || getenv("BPWPE_NO_LOCKSURF")) ? 0 : 1;
        if (s_locksurf && !wantScale) {
            long us = 0;
            if (lock_readback(t, slot, rw, rh, &us)) {
                { static int _ln = 0; if (_ln++ % 12 == 0) ATLAS_LOG("LOCKSURF readback %ldus (%ux%u) [glReadPixels was ~1.3s]", us, rw, rh); }
                if (t->master) { memcpy(t->master, slot, (size_t)rw * rh * 4); t->force_full = 0; }   /* master re-seeded at the new size */
                t->slots_to_seed = SLOTS - 1;   /* other slot(s) re-seed static content before rect-only video copies */
                goto sent;
            }
        }

        /* Readback from the currently-bound FBO (ds_fbo if we GPU-scaled, else the layout fbo) at rw x rh.
         * PERF: glReadPixels blocks until the render completes, so no glFinish() is needed; and when the
         * driver can readback BGRA we read straight into the slot's native byte order, skipping swizzle. */
        if (s_bgra_readback < 0) {
            /* PROBE, don't trust the extension string: glGetString(GL_EXTENSIONS) came back without
             * GL_EXT_read_format_bgra under 2.38 even though the Adreno reads BGRA fine (2.34 used the
             * fast path). Try a real BGRA readback and check glGetError — empirical + driver-accurate. */
            while (glGetError() != GL_NO_ERROR) { }   /* drain stale errors */
            glReadPixels(0, 0, (GLsizei)rw, (GLsizei)rh, 0x80E1 /*GL_BGRA_EXT*/, GL_UNSIGNED_BYTE, slot);
            GLenum perr = glGetError();
            if (perr == GL_NO_ERROR) {
                s_bgra_readback = 1;
                ATLAS_LOG("BGRA readback supported (fast path, probed)");
            } else {
                s_bgra_readback = 0;
                ATLAS_LOG("BGRA readback unsupported (GL err 0x%x), swizzle fallback", perr);
                glReadPixels(0, 0, (GLsizei)rw, (GLsizei)rh, GL_RGBA, GL_UNSIGNED_BYTE, t->scratch);
                swizzle_flip(t->scratch, slot, rw, rh);
            }
        } else if (s_bgra_readback) {
            glReadPixels(0, 0, (GLsizei)rw, (GLsizei)rh, 0x80E1 /*GL_BGRA_EXT*/, GL_UNSIGNED_BYTE, slot);
        } else {
            struct timeval _sa, _sb, _sc, _sd;
            gettimeofday(&_sa, NULL);
            glFinish();                                   /* isolate GPU render time from readback */
            gettimeofday(&_sb, NULL);
            glReadPixels(0, 0, (GLsizei)rw, (GLsizei)rh, GL_RGBA, GL_UNSIGNED_BYTE, t->scratch);
            gettimeofday(&_sc, NULL);
            swizzle_flip(t->scratch, slot, rw, rh);
            gettimeofday(&_sd, NULL);
            { static int _sn = 0; if (_sn % 20 == 0)
                ATLAS_LOG("PERF-SPLIT render(glFinish)=%ldus readback=%ldus swizzle=%ldus (%ux%u)",
                    (_sb.tv_sec-_sa.tv_sec)*1000000L+(_sb.tv_usec-_sa.tv_usec),
                    (_sc.tv_sec-_sb.tv_sec)*1000000L+(_sc.tv_usec-_sb.tv_usec),
                    (_sd.tv_sec-_sc.tv_sec)*1000000L+(_sd.tv_usec-_sc.tv_usec), rw, rh);
              _sn++; }
        }

        /* P2: a full 1:1 readback just refreshed the slot — mirror it into the master buffer so the
         * NEXT pan re-center can strip-update from a correct base (covers loads/reflows/resizes too).
         * The strip path above did goto sent, skipping this; wantScale never uses master. */
        if (!wantScale && t->master) { memcpy(t->master, slot, (size_t)rw * rh * 4); t->force_full = 0; t->slots_to_seed = SLOTS - 1; }   /* master re-seeded; other slot(s) re-seed before rect-only video copies */

    sent:;
        gettimeofday(&_pt1, NULL);
        { static int _pn = 0; if (_pn % 30 == 0) {
            long _rb = (_pt1.tv_sec - _pt0.tv_sec) * 1000000L + (_pt1.tv_usec - _pt0.tv_usec);
            long _iv = _plast.tv_sec ? ((_pt1.tv_sec - _plast.tv_sec) * 1000000L + (_pt1.tv_usec - _plast.tv_usec)) : 0;
            ATLAS_LOG("PERF %ux%u readback+swizzle=%ldus interval=%ldus (%.1f fps) bgra=%d",
                     rw, rh, _rb, _iv, _iv ? 1000000.0 / _iv : 0.0, s_bgra_readback);
          } _pn++; }
        _plast = _pt1;
        struct atlas_msg m = { .type = MSG_FRAME_READY, .width = rw, .height = rh, .slot = t->cur_slot };
        send(t->host_fd, &m, sizeof(m), 0);
        t->cur_slot = (t->cur_slot + 1) % SLOTS;
    }
    /* let WebKit proceed to the next frame (into the other slot). With double-buffering we don't
     * wait for the UIProcess ack here; drain acks opportunistically for flow control if needed. */
    wpe_renderer_backend_egl_target_dispatch_frame_complete(t->wpe);
}
static void target_deinitialize(void* d) { (void)d; }

static struct wpe_renderer_backend_egl_target_interface s_target_iface = {
    target_create, target_destroy, target_initialize, target_get_native_window,
    target_resize, target_frame_will_render, target_frame_rendered, target_deinitialize,
    NULL, NULL, NULL
};

/* offscreen target (secondary GL contexts, e.g. WebGL/canvas) — surfaceless/pbuffer is fine */
static void* offscreen_create(void) { return calloc(1, 1); }
static void  offscreen_destroy(void* d) { free(d); }
static void  offscreen_initialize(void* d, void* backend) { (void)d; (void)backend; }
static EGLNativeWindowType offscreen_get_native_window(void* d) { (void)d; return (EGLNativeWindowType)0; }
static struct wpe_renderer_backend_egl_offscreen_target_interface s_offscreen_iface = {
    offscreen_create, offscreen_destroy, offscreen_initialize, offscreen_get_native_window,
    NULL, NULL, NULL, NULL
};

/* =========================================================================================== *
 *  Loader — libwpe asks for interfaces by name                                                 *
 * =========================================================================================== */
static void* atlas_load_object(const char* name)
{
    if (!strcmp(name, "_wpe_renderer_host_interface"))                    return &s_host_iface;
    if (!strcmp(name, "_wpe_renderer_backend_egl_interface"))            return &s_egl_iface;
    if (!strcmp(name, "_wpe_renderer_backend_egl_target_interface"))     return &s_target_iface;
    if (!strcmp(name, "_wpe_renderer_backend_egl_offscreen_target_interface")) return &s_offscreen_iface;
    if (!strcmp(name, "_wpe_view_backend_interface"))                    return &s_view_iface;
    ATLAS_LOG("load_object: unknown '%s'", name);
    return NULL;
}

/* libwpe REQUIRES the backend .so to export this exact symbol. */
__attribute__((visibility("default")))
struct wpe_loader_interface _wpe_loader_interface = {
    atlas_load_object, NULL, NULL, NULL, NULL
};
