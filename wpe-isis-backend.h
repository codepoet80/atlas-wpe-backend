/*
 * wpe-isis-backend — custom libwpe backend bridging WPE WebKit into the Isis BrowserServer.
 * Public API used by the BrowserServer (UIProcess). See ARCHITECTURE.md.
 */
#ifndef WPE_ISIS_BACKEND_H
#define WPE_ISIS_BACKEND_H

#include <stdint.h>
#include <wpe/wpe.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WPE_ISIS_EXPORT __attribute__((visibility("default")))

/*
 * Invoked on the UIProcess (BrowserServer) main thread when a freshly rendered frame is ready.
 *   argb   : width*height*4 bytes, ARGB32-premultiplied (BGRA byte order on little-endian ARM),
 *            top-left origin — drop straight into a QImage::Format_ARGB32_Premultiplied buffer.
 *   stride : bytes per row (== width*4 for tightly packed frames).
 * The handler MUST copy out of `argb` before returning; the slot may be recycled afterwards.
 * Typical handler: memcpy into m_offscreenN->rasterBuffer() then BrowserPage::flushBuffer(N).
 */
typedef void (*wpe_isis_frame_handler)(void* userdata,
                                       const uint8_t* argb,
                                       uint32_t width, uint32_t height,
                                       uint32_t stride);

/*
 * Create a wpe_view_backend bound to an Isis frame sink. Pass the result to
 * webkit_web_view_backend_new(view_backend, NULL, NULL). width/height are the LAYOUT (page) size in
 * device pixels — WebKit lays the page out and composites into a width×height FBO.
 *
 * scaledWidth/scaledHeight are the FIT-TO-SCREEN output size: if non-zero and smaller than the
 * layout, each rendered frame is DOWNSCALED (GPU pass, CPU-bilinear fallback) from width×height to
 * scaledWidth×scaledHeight before readback, so the frame the handler receives (and thus the Isis
 * offscreen buffer) genuinely IS the page rendered at contentZoom = scaledWidth/width — matching the
 * legacy BrowserOffscreenInfo contract (renderedWidth==bufferWidth, zoom baked into the buffer).
 * Pass 0/0 (or a size >= layout) for no scaling (1:1). The frame handler is invoked with the ACTUAL
 * (scaled) width/height.
 *
 * The backend is provided in-process (create_with_backend_interface) so it can reach the owning
 * BrowserPage via `userdata`.
 */
WPE_ISIS_EXPORT
struct wpe_view_backend* wpe_isis_view_backend_create(uint32_t width, uint32_t height,
                                                      uint32_t scaledWidth, uint32_t scaledHeight,
                                                      wpe_isis_frame_handler handler,
                                                      void* userdata);

/* Page resized: re-sizes the shared frame buffers and tells WebKit. */
WPE_ISIS_EXPORT
void wpe_isis_view_backend_resize(struct wpe_view_backend*, uint32_t width, uint32_t height);

/* Visibility: forwards to WebKit so it can throttle rendering when the card is backgrounded. */
WPE_ISIS_EXPORT
void wpe_isis_view_backend_set_visible(struct wpe_view_backend*, bool visible);

/*
 * Input is dispatched with the stock libwpe entry points directly on the returned
 * wpe_view_backend, e.g. wpe_view_backend_dispatch_pointer_event(...). No wrappers needed —
 * the BrowserServer translates Yap adapter events into wpe_input_* structs and calls them.
 */

#ifdef __cplusplus
}
#endif
#endif /* WPE_ISIS_BACKEND_H */
