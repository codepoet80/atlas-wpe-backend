/*
 * frame-dump.c — standalone WPE WebKit render test for the Isis backend. No BrowserServer/Qt/Yap.
 * Loads a URL through libWPEBackend-isis and writes each rendered frame to a PNG, so the EGL surface
 * path + readback can be validated in isolation on-device before touching the BrowserServer.
 *
 * Build:  build.sh  (produces ./frame-dump alongside libWPEBackend-isis.so)
 *
 * Run on device (musl binaries on a glibc device — needs the musl loader present):
 *   # one-time: make the musl interpreter resolvable for the exec'd WPEWebProcess
 *   mount -o remount,rw /; ln -sf /media/internal/isis2/lib/ld-musl-arm.so.1 /lib/ld-musl-arm.so.1
 *   export LD_LIBRARY_PATH=/media/internal/isis2/lib
 *   export WPE_BACKEND_LIBRARY=/media/internal/isis2/lib/libWPEBackend-isis.so
 *   export WEBKIT_EXEC_PATH=/media/internal/isis2/libexec/wpe-webkit-1.0   # where WPEWebProcess lives
 *   export WEBKIT_INJECTED_BUNDLE_PATH=/media/internal/isis2/lib/wpe-webkit-1.0/injected-bundle
 *   /media/internal/isis2/lib/ld-musl-arm.so.1 --library-path /media/internal/isis2/lib \
 *       /media/internal/isis2/frame-dump https://example.com /tmp/frame_
 *
 * Watch stderr: "[harness] wrote frame_000.png" = success; WebKit "Cannot create EGL ... surface"
 * means the Adreno rejected the target_get_native_window strategy (try the alternatives there).
 */
#include "wpe-isis-backend.h"
#include <wpe/webkit.h>
#include <glib.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ctx {
    GMainLoop* loop;
    int        frame;
    int        max_frames;
    const char* prefix;
    WebKitWebView* view;   /* for the phase-2 re-navigation */
    const char* nav2;      /* WPE_DUMP_NAV2: 2nd URL to test in-process re-render */
    int        phase;      /* 1 = first URL, 2 = after re-navigation */
};

/* ARGB32-premultiplied (BGRA bytes) -> RGBA PNG. */
static void write_png(const char* path, const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride)
{
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[harness] cannot open %s\n", path); return; }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop   info = png_create_info_struct(png);
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info); fclose(f); return;
    }
    png_init_io(png, f);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    uint8_t* row = (uint8_t*)malloc((size_t)w * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = bgra + (size_t)(h - 1 - y) * stride;   /* flip: buffer is bottom-up */
        for (uint32_t x = 0; x < w; ++x) {
            row[x*4+0] = src[x*4+2];  /* R <- B */
            row[x*4+1] = src[x*4+1];  /* G */
            row[x*4+2] = src[x*4+0];  /* B <- R */
            row[x*4+3] = src[x*4+3];  /* A */
        }
        png_write_row(png, row);
    }
    free(row);
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    fclose(f);
}

static void on_frame(void* ud, const uint8_t* argb, uint32_t w, uint32_t h, uint32_t stride)
{
    struct ctx* c = (struct ctx*)ud;
    char path[512];
    /* Always overwrite a single "last" frame; the timeout below captures the settled page
     * (JS-heavy pages like html5test need many seconds to compute past the spinner). */
    snprintf(path, sizeof(path), "%sp%d_last.png", c->prefix, c->phase);
    write_png(path, argb, w, h, stride);
    if ((++c->frame % 10) == 1) fprintf(stderr, "[harness] phase%d frame %d (%ux%u)\n", c->phase, c->frame, w, h);
}

static void on_load_changed(WebKitWebView* view, WebKitLoadEvent ev, gpointer ud)
{
    (void)view; (void)ud;
    const char* s = ev == WEBKIT_LOAD_STARTED ? "started"
                  : ev == WEBKIT_LOAD_COMMITTED ? "committed"
                  : ev == WEBKIT_LOAD_FINISHED ? "finished" : "?";
    fprintf(stderr, "[harness] load %s\n", s);
}

static void on_load_failed(WebKitWebView* view, WebKitLoadEvent ev, const char* uri, GError* err, gpointer ud)
{
    (void)view; (void)ev; (void)ud;
    fprintf(stderr, "[harness] LOAD FAILED %s: %s\n", uri, err ? err->message : "?");
}

static gboolean on_timeout(gpointer ud)
{
    struct ctx* c = (struct ctx*)ud;
    /* PHASE2: if a 2nd URL was requested, re-navigate IN-PROCESS and watch for fresh frames.
     * This isolates the "compositor renders once then stops" bug without the app/adapter. */
    if (c->nav2 && c->phase == 1) {
        fprintf(stderr, "[harness] PHASE1 done (%d frame(s)). PHASE2: re-navigating to %s\n", c->frame, c->nav2);
        c->phase = 2; c->frame = 0;
        webkit_web_view_load_uri(c->view, c->nav2);
        g_timeout_add_seconds(8, on_timeout, c);   /* phase-2 dump window */
        return G_SOURCE_REMOVE;
    }
    fprintf(stderr, "[harness] timeout — phase %d produced %d frame(s)%s\n",
            c->phase, c->frame, (c->phase == 2 && c->frame == 0) ? "  <-- 2nd page never painted!" : "");
    g_main_loop_quit(c->loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char** argv)
{
    const char* url    = argc > 1 ? argv[1] : "https://example.com";
    const char* prefix = argc > 2 ? argv[2] : "frame_";
    int W = (argc > 3) ? atoi(argv[3]) : 1024;
    int H = (argc > 4) ? atoi(argv[4]) : 768;

    /* Only set if the user didn't (allows overriding with a full path). */
    setenv("WPE_BACKEND_LIBRARY", "libWPEBackend-isis.so", 0);

    const char* nav2 = getenv("WPE_DUMP_NAV2");   /* set to a 2nd URL to test in-process re-render */
    struct ctx c = { g_main_loop_new(NULL, FALSE), 0, /*max_frames*/ 6, prefix, NULL, nav2, 1 };

    fprintf(stderr, "[m] pre-backend\n"); fflush(stderr);
    /* no fit-to-screen downscale in the dump harness (0,0 = 1:1) */
    struct wpe_view_backend* vb = wpe_isis_view_backend_create((uint32_t)W, (uint32_t)H, 0, 0, on_frame, &c);
    fprintf(stderr, "[m] vb=%p\n", (void*)vb); fflush(stderr);
    WebKitWebViewBackend* wvb = webkit_web_view_backend_new(vb, NULL, NULL);
    fprintf(stderr, "[m] wvb=%p\n", (void*)wvb); fflush(stderr);
    WebKitWebView* view = webkit_web_view_new(wvb);
    fprintf(stderr, "[m] view=%p\n", (void*)view); fflush(stderr);
    g_object_ref_sink(view);
    c.view = view;

    g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), &c);
    g_signal_connect(view, "load-failed",  G_CALLBACK(on_load_failed), &c);
    int secs = getenv("WPE_DUMP_SECONDS") ? atoi(getenv("WPE_DUMP_SECONDS")) : 25;
    g_timeout_add_seconds(secs, on_timeout, &c);   /* capture the settled page; override via WPE_DUMP_SECONDS */

    fprintf(stderr, "[harness] backend=%s  loading %s at %dx%d (max %d frames)\n",
            getenv("WPE_BACKEND_LIBRARY"), url, W, H, c.max_frames);
    webkit_web_view_load_uri(view, url);

    g_main_loop_run(c.loop);
    fprintf(stderr, "[harness] done (%d frame(s) written)\n", c.frame);
    return c.frame > 0 ? 0 : 1;
}
