/*
 * BrowserPageWPE.h — WPE WebKit replacement for the QtWebKit-602 BrowserPage in the Isis
 * BrowserServer. Keeps the entire Isis output path (offscreen double buffer, flushBuffer,
 * msgPainted, the buffer-lock handshake with the adapter) and the QBsClient command surface;
 * swaps the QGraphicsWebView/WebOSWebPage rendering for a WebKitWebView driven by the custom
 * libwpe backend (wpe-isis-backend). The BrowserServer creates one of these per proxy instead
 * of a BrowserPage (A/B switchable, as the 602 was switched in against 537).
 *
 * Scope of this skeleton: the browse path (attach buffers, load, render→flush, input,
 * navigation, sizing, focus, freeze/thaw, load/title/url notifications). The long tail of
 * BrowserPage (downloads, dialogs, editing, hit-testing, plugins, popups) is declared and
 * stubbed with TODOs — those need WPE-API equivalents but don't block first paint.
 */
#ifndef BROWSER_PAGE_WPE_H
#define BROWSER_PAGE_WPE_H

#include "QBsClient.h"          // the Yap-generated command interface BrowserPage also implements
#include <cstring>                 // BrowserAdapterTypes.h uses strncmp without including it
#include "BrowserAdapterTypes.h"   // BATypes, UrlMatchType — interface types used by long-tail methods
#include <QtWebKit/QtWebKit>       // QWebHitTestResult (interface type); compile-only — QtWebKit link TBD
#include <wpe/webkit.h>
#include <semaphore.h>
#include <stdint.h>
#include <string>
#include <cstring>

class BrowserServer;
class YapProxy;
class BrowserOffscreenQt;
struct wpe_view_backend;

enum UrlMatchType {
    UrlMatchRedirect = 0,
    UrlMatchCommand = 1
};

class BrowserPageWPE : public QBsClient {
public:
    BrowserPageWPE(BrowserServer* server, YapProxy* proxy);
    virtual ~BrowserPageWPE();

    // ---- Isis buffer / lifecycle (kept from BrowserPage) ----
    bool attachToBuffer(uint32_t virtualPageWidth, uint32_t virtualPageHeight,
                        int sharedBufferKey1, int sharedBufferKey2, int sharedBufferSize);
    void bufferReturned(int32_t sharedBufferKey);
    void flushBuffer(int buffer);
    bool freeze();
    bool thaw(int sharedBufferKey1, int sharedBufferKey2, int sharedBufferSize);

    // ---- navigation / content ----
    void openUrl(const char* url);
    void setHTML(const char* url, const char* body);
    void pageBackward();
    void pageForward();
    void pageReload();
    void pageStop();
    bool canGoBackward() const;
    bool canGoForward() const;
    void clearHistory();

    // ---- sizing / zoom ----
    void setWindowSize(uint32_t width, uint32_t height);
    void setVirtualWindowSize(uint32_t width, uint32_t height);
    void getWindowSize(int& width, int& height);
    void getVirtualWindowSize(int& width, int& height);
    void setScrollPosition(int cx, int cy, int cw, int ch);
    bool recenterForScroll(int cx, int cy);   // P2: windowed pan re-center; also the onFrame catch-up. Returns true if a re-render was issued.
    void setZoomAndScroll(double zoom, int cx, int cy);

    // ---- autonomous scroll test (kicked with SIGUSR1; drives setScrollPosition like the adapter) ----
    void startScrollTest();      // begin a top->bottom->top sweep on this page
    bool scrollTestStep();       // one timed step; returns false when the sweep finishes
    void reportScrollTest();     // log the SCROLLTEST summary line

    // ---- input ----
    void mouseEvent(int type, int contentX, int contentY, int detail);
    bool clickAt(uint32_t contentsPosX, uint32_t contentsPosY, uint32_t numClicks);
    bool holdAt(uint32_t contentsPosX, uint32_t contentsPosY);
    void keyDown(int32_t key, int32_t modifiers, int32_t chr);
    void keyUp(int32_t key, int32_t modifiers, int32_t chr);
    void setFocus(bool enable);
    void onEditorFocus(bool focused, int purpose);   // IM-context focus → msgEditorFocused (raises webOS VKB)

    // ---- proxy / identity ----
    YapProxy* getProxy() { return m_proxy; }
    void      setProxy(YapProxy* proxy) { m_proxy = proxy; }
    void      setIdentifier(const char* id);
    void      setUserAgent(const char* ua);

    // ---- long-tail stubs (declared to match BrowserPage; TODO real WPE impls) ----
    void addUrlRedirect(const char* urlRe, UrlMatchType matchType, bool redirect, const char* userData) {}
    void clearSelection() { if (m_webView) webkit_web_view_execute_editing_command(m_webView, "Unselect"); }
    void cut() { if (m_webView) webkit_web_view_execute_editing_command(m_webView, "Cut"); }
    void disableEnhancedViewport(bool disable) {}
    void downloadCancel( const char* url ) {}
    void dragEnd(int contentX, int contentY) {}
    void dragProcess(int deltaX, int deltaY) {}
    void dragStart(int contentX, int contentY) {}
    void enableInterrogateClicks( bool enable ) {}
    int findString( const char* str, bool fwd ) {
        if (!m_webView) return 0;
        /* On-demand reader extraction trigger (app calls viewCall findString with this magic on
         * reader-open) — extract from the CURRENT DOM, no wait for full page load. */
        if (str && strcmp(str, "__ISIS_EXTRACT__") == 0) { extractReaderContent(); return 1; }
        WebKitFindController* fc = webkit_web_view_get_find_controller(m_webView);
        if (!str || !*str) { webkit_find_controller_search_finish(fc); m_lastFind.clear(); return 0; }  /* "" = reset/close (legacy findInPage("")) */
        guint32 o = WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND |
                    (fwd ? 0u : (guint32)WEBKIT_FIND_OPTIONS_BACKWARDS);
        if (m_lastFind == str) {                         /* same query -> next/prev; the all-match highlight stays up */
            if (fwd) webkit_find_controller_search_next(fc); else webkit_find_controller_search_previous(fc);
        } else {                                         /* new query -> highlight ALL matches (legacy HighlightAllOccurrences) + go to first */
            m_lastFind = str;
            webkit_find_controller_count_matches(fc, str, o, G_MAXUINT);
            webkit_find_controller_search(fc, str, o, G_MAXUINT);
        }
        return 1;
    }
    void gestureEvent(int type, int contentX, int contentY, double scale, double rotation, int centerX, int centerY);
    void getInteractiveNodeRects(int32_t mouseX, int32_t mouseY) {}
    void getTextCaretBounds(int& left, int& top, int& right, int& bottom) {}
    void hideSpellingWidget() {}
    QWebHitTestResult hitTest (uint32_t x, uint32_t y) { return {}; }
    void insertStringAtCursor(const char* text) { if (m_webView && text) webkit_web_view_execute_editing_command_with_argument(m_webView, "InsertText", text); }
    bool isEditing() { return {}; }
    bool isInteractiveAtPoint( uint32_t x, uint32_t y ) { return {}; }
    void paste() { if (m_webView) webkit_web_view_execute_editing_command(m_webView, "Paste"); }
    void pluginSpotlightEnd() {}
    void pluginSpotlightStart(int x, int y, int cx, int cy) {}
    void printFrame(const char* frameName, int lpsJobId, int width, int height, int dpi, bool landscape, bool reverseOrder) {}
    int renderToFile(const char* filename, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH) { return {}; }
    bool saveImageAtPoint(uint32_t x, uint32_t y, QString& filepath) { return {}; }
    void scrollLayer(int id, int deltaX, int deltaY) {}
    void selectAll() { if (m_webView) webkit_web_view_execute_editing_command(m_webView, "SelectAll"); }
    void setDNSServers(const char *servers) {}
    void setIgnoreMetaRefreshTags(bool ignore) {}
    void setIgnoreMetaViewport(bool ignore) {}
    void setMinFontSize(int minFontSizePt) { if (m_webView) webkit_settings_set_minimum_font_size(webkit_web_view_get_settings(m_webView), minFontSizePt > 0 ? (guint)minFontSizePt : 0); }
    void setMouseMode(enum BATypes::MouseMode mode) {}
    void setNetworkInterface(const char* interfaceName) {}
    void setSelectionMode(bool on) {}
    void setShowClickedLink(bool enable) {}
    void settingsJavaScriptEnabled(bool enable) { if (m_webView) webkit_settings_set_enable_javascript(webkit_web_view_get_settings(m_webView), enable); }
    void settingsPopupsEnabled(bool enable) { if (m_webView) webkit_settings_set_javascript_can_open_windows_automatically(webkit_web_view_get_settings(m_webView), enable); }
    void smartZoomCalculate( uint32_t pointX, uint32_t pointY );
    void touchEvent(int type, int32_t touchCount, int32_t modifiers, const char *touchesJson);
    int getPageX() { return m_windowWidth/2; }
    int getPageY() { return m_windowHeight/2; }
    double getZoomLevel() { return 1.0; }
    /* Fit-to-screen factor baked into the offscreen buffer: screenW/layoutW = renderW/virtualW.
     * 1.0 when not downscaling. This is the contentZoom reported in the BrowserOffscreenInfo header;
     * the adapter divides on-screen taps by it to recover document coords (== our layout space). */
    double contentZoom() const {
        return (m_virtualWindowWidth > 0 && m_renderWidth > 0)
               ? (double)m_renderWidth / (double)m_virtualWindowWidth : 1.0;
    }
    void getElementInfoAtPoint(int32_t x, int32_t y) { (void)x; (void)y; }
    bool init(uint32_t w, uint32_t h, int k1, int k2, int sz) {
        m_windowWidth = w; m_windowHeight = h; m_virtualWindowWidth = w; m_virtualWindowHeight = h;
        return attachToBuffer(w, h, k1, k2, sz); }

    // ---- additional drop-in stubs ----
    bool copy() { if (m_webView) webkit_web_view_execute_editing_command(m_webView, "Copy"); return true; }

    // ---- manager/main extras ----
    uint32_t getPriority() { return m_priority; }
    void setPriority(uint32_t p) { m_priority = p; }
    static void setInspectorPort(int port) { (void)port; }

private:
    // backend frame sink (UIProcess thread): copy ARGB → current offscreen, flushBuffer
    static void onFrame(void* ud, const uint8_t* argb, uint32_t w, uint32_t h, uint32_t stride);

    // WPE WebKit signal handlers
    static void onLoadChanged(WebKitWebView*, WebKitLoadEvent, gpointer);
    static void onLoadFailed(WebKitWebView*, WebKitLoadEvent, const char* uri, GError*, gpointer);
    static void onTitleChanged(GObject*, GParamSpec*, gpointer);
    static void onUriChanged(GObject*, GParamSpec*, gpointer);
    static void onProgressChanged(GObject*, GParamSpec*, gpointer);
    static int  onDecidePolicy(WebKitWebView*, WebKitPolicyDecision*, WebKitPolicyDecisionType, gpointer);  // unsupported-mime → download handoff

    void ensureWebView();                    // create m_viewBackend + m_webView once size is known
    void dispatchPointer(int x, int y, uint32_t button, bool down);
    void updateContentsSize();               // async-query the real page height for scroll range
    static void onContentHeight(GObject*, GAsyncResult*, gpointer);
    void extractReaderContent();                                   // extract current DOM's article, push to app via msgActionData (on-demand from reader-open)
    static void onReaderProbe(GObject*, GAsyncResult*, gpointer);  // extraction result -> msgActionData("readerContent")
    void checkEditorFocus();                 // after a tap: poll document.activeElement → raise/hide the VKB
    static void onEditorCheckResult(GObject*, GAsyncResult*, gpointer);
    static void onSmartZoomResult(GObject*, GAsyncResult*, gpointer);   // double-tap zoom: block rect → response
    int m_smartZoomX, m_smartZoomY;          // echoed back in the smart-zoom response

    // ---- Isis members (mirror BrowserPage) ----
    BrowserServer*      m_server;
    YapProxy*           m_proxy;
    char*               m_identifier;
    BrowserOffscreenQt* m_offscreen0;
    BrowserOffscreenQt* m_offscreen1;
    bool                m_ownOffscreen0;
    bool                m_ownOffscreen1;
    sem_t*              m_bufferLock;
    char*               m_bufferLockName;
    int                 m_windowWidth, m_windowHeight;
    int                 m_virtualWindowWidth, m_virtualWindowHeight;
    int                 m_screenWidth;          // real fb width; layout may be wider (fit-to-screen via contentZoom)
    int                 m_screenHeight;         // real fb height (visible rows) — the pannable window height
    int                 m_renderedY;            // pan model: content-Y of the buffer's top; buffer holds [m_renderedY, +m_renderHeight]
    int                 m_deliveredY;           // renderedY of the LAST delivered frame (what the adapter is actually showing) — scroll-test coverage metric
    int                 m_renderWidth, m_renderHeight;  // ACTUAL frame size the backend outputs (== screen size when downscaling; else == layout). onFrame validates against these; bufferWidth = m_renderWidth.
    int                 m_scrollX, m_scrollY;   // last scroll pos (content→view tap mapping)
    long                m_lastScrollMs;         // predictive pan: timestamp + Y of the previous scroll event
    int                 m_lastScrollY, m_scrollVel;  // m_scrollVel = px/sec, signed (down +). Re-center earlier + biased in this direction.
    int                 m_pageHeight;           // P1: scaled document height; if <= m_renderHeight the whole page fits -> never re-render
    bool                m_focused;
    bool                m_frozen;
    bool                m_private;     // ephemeral session (private browsing) — set by the openUrl marker
    bool                m_prewarmBlank; // BPWPE_PRESPAWN: WebView pre-warmed with about:blank (non-private)
    bool                m_renderPending; // pan re-render in flight; don't queue another (m_renderedY would race ahead of the delivered buffer)
    long                m_renderPendingMs; // _wlog_ms() when m_renderPending was set — staleness timeout so a lost frame can't deadlock the pan
    uint32_t            m_priority;

    // ---- WPE members (replace m_graphicsView/m_scene/m_webView/m_webPage) ----
    struct wpe_view_backend* m_viewBackend;
    WebKitWebView*           m_webView;
    std::string              m_userAgent;
    std::string              m_lastFind;     // last find query → new vs next/prev (highlight-all stays up)
};

#endif /* BROWSER_PAGE_WPE_H */
