/*
 * atlas-wpe-glue.cpp — SKETCH of the BrowserServer (UIProcess) side wiring the Atlas WPE backend
 * into a WebKitWebView. This is reference glue for the BrowserServer port, not a standalone file;
 * the members (m_offscreen0/1, flushBuffer, m_proxy ...) map onto the existing BrowserPage.
 *
 * Run env (autostart job):
 *   WPE_BACKEND_LIBRARY=/media/internal/atlas2/lib/libWPEBackend-atlas.so
 *   LD_LIBRARY_PATH=/media/internal/atlas2/lib   (deploy bundle; device Adreno provides libEGL/GLESv2)
 *   ld-musl-arm.so.1 --library-path /media/internal/atlas2/lib ./WPEWebProcess  (musl interp)
 */

#include "wpe-atlas-backend.h"
#include <wpe/webkit.h>          /* WPE WebKit C API: WebKitWebView, webkit_web_view_backend_new */
#include <cstring>

// Forward decls onto the real BrowserPage. ---------------------------------------------------
class BrowserOffscreenQt { public: unsigned char* rasterBuffer() const; int rasterSize() const; int key() const; };
class BrowserPageWPE {
public:
    void init(uint32_t width, uint32_t height);
    void loadUrl(const char* uri);
    // Atlas ↔ WebKit input bridges (called from the Yap command handlers):
    void mouseEvent(int type, int x, int y, int button);
    void keyEvent(int type, int keysym, int modifiers);

private:
    static void onFrame(void* ud, const uint8_t* argb, uint32_t w, uint32_t h, uint32_t stride);

    BrowserOffscreenQt* m_offscreen0 = nullptr;   // existing double buffers (attachToBuffer())
    BrowserOffscreenQt* m_offscreen1 = nullptr;
    int                 m_writeBuf   = 0;          // which offscreen we render into next
    uint32_t            m_width = 0, m_height = 0;

    struct wpe_view_backend* m_viewBackend = nullptr;
    WebKitWebView*           m_webView     = nullptr;

    // existing BrowserPage machinery (kept verbatim from the 602 port):
    void flushBuffer(int buffer);                 // -> m_server->msgPainted(m_proxy, key)
};

// 1) Bring up a WPE web view backed by the Atlas frame sink. ----------------------------------
void BrowserPageWPE::init(uint32_t width, uint32_t height)
{
    m_width = width; m_height = height;

    // Our custom view backend; `this` is delivered back to onFrame() as userdata.
    m_viewBackend = wpe_atlas_view_backend_create(width, height, &BrowserPageWPE::onFrame, this);

    WebKitWebViewBackend* vb = webkit_web_view_backend_new(m_viewBackend, nullptr, nullptr);

    // One shared web context per BrowserServer; cookies/cache point at /media/internal/atlas2.
    WebKitWebContext* ctx = webkit_web_context_get_default();
    m_webView = webkit_web_view_new_with_context(vb, ctx);   // (or _new(vb) on this API level)

    // JSC JIT on this armv7 was a SIGILL source for 602; if WPE's JSC also misbehaves, export
    // JavaScriptCoreUseJIT=0 in the job env (runtime, no rebuild).
}

void BrowserPageWPE::loadUrl(const char* uri)
{
    webkit_web_view_load_uri(m_webView, uri);
}

// 2) THE SEAM: a rendered ARGB frame arrives on the UIProcess main thread. --------------------
//    Drop it into the current Atlas offscreen and flush exactly like the 602 path did.
void BrowserPageWPE::onFrame(void* ud, const uint8_t* argb, uint32_t w, uint32_t h, uint32_t stride)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    BrowserOffscreenQt* off = (self->m_writeBuf == 0) ? self->m_offscreen0 : self->m_offscreen1;
    if (!off || w != self->m_width || h != self->m_height) return;

    unsigned char* dst = off->rasterBuffer();          // ARGB32-premultiplied, top-left origin
    const uint32_t dstStride = self->m_width * 4;
    if (stride == dstStride) {
        memcpy(dst, argb, (size_t)h * dstStride);
    } else {                                            // stride mismatch: row-by-row
        for (uint32_t y = 0; y < h; ++y)
            memcpy(dst + (size_t)y * dstStride, argb + (size_t)y * stride, dstStride);
    }

    self->flushBuffer(self->m_writeBuf);                // -> msgPainted(off->key()) -> adapter blit
    self->m_writeBuf ^= 1;                              // ping-pong with the WebProcess's slots
}

// 3) Input: translate Yap adapter events into libwpe events on the view backend. --------------
void BrowserPageWPE::mouseEvent(int type, int x, int y, int button)
{
    struct wpe_input_pointer_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type   = (type == /*move*/0) ? wpe_input_pointer_event_type_motion
                                     : wpe_input_pointer_event_type_button;
    ev.x = x; ev.y = y; ev.button = button; ev.state = (type == /*down*/1) ? 1 : 0;
    ev.modifiers = 0;
    wpe_view_backend_dispatch_pointer_event(m_viewBackend, &ev);
}

void BrowserPageWPE::keyEvent(int type, int keysym, int modifiers)
{
    struct wpe_input_keyboard_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.key_code = keysym;                  // map Atlas key → xkb keysym (wpe/keysyms.h)
    ev.modifiers = modifiers;
    ev.pressed = (type == /*down*/1);
    wpe_view_backend_dispatch_keyboard_event(m_viewBackend, &ev);
}

/*
 * Status / next on-device steps:
 *  - The backend .so builds (libWPEBackend-atlas.so). This glue is the BrowserServer integration
 *    point; it slots into BrowserPage where the 602 QGraphicsView render used to live.
 *  - First device bring-up: confirm the Adreno EGL surface mode (target_get_native_window strategy).
 *    Run WPEWebProcess once, watch for "Cannot create EGL ... surface" — that decides surfaceless
 *    vs a real offscreen native window.
 *  - Then verify onFrame() fires and the first paint reaches the adapter (msgPainted), same proof
 *    point as the 602 "example.com renders" milestone.
 */
