/*
 * BrowserPageWPE.h — WPE WebKit replacement for the QtWebKit-602 BrowserPage in the Atlas
 * BrowserServer. Keeps the entire Atlas output path (offscreen double buffer, flushBuffer,
 * msgPainted, the buffer-lock handshake with the adapter) and the QBsClient command surface;
 * swaps the QGraphicsWebView/WebOSWebPage rendering for a WebKitWebView driven by the custom
 * libwpe backend (wpe-atlas-backend). The BrowserServer creates one of these per proxy instead
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
#include <lunaservice.h>          // mediad system-video handoff: LSCallFromApplication on BrowserServer's handle
#include <stdint.h>
#include <string>
#include <vector>
#include <cstring>

class BrowserServer;
class YapProxy;
class BrowserOffscreenQt;
class BrowserPage;             // opaque — BrowserSyncReplyPipe's ctor takes a BrowserPage* (used only as a %p id)
class BrowserSyncReplyPipe;    // Feature 7: sync round-trip to the app for SSL/auth dialogs
struct wpe_view_backend;

enum UrlMatchType {
    UrlMatchRedirect = 0,
    UrlMatchCommand = 1
};

class BrowserPageWPE : public QBsClient {
public:
    BrowserPageWPE(BrowserServer* server, YapProxy* proxy);
    virtual ~BrowserPageWPE();

    // ---- Atlas buffer / lifecycle (kept from BrowserPage) ----
    bool attachToBuffer(uint32_t virtualPageWidth, uint32_t virtualPageHeight,
                        int sharedBufferKey1, int sharedBufferKey2, int sharedBufferSize);
    void bufferReturned(int32_t sharedBufferKey);
    void flushBuffer(int buffer);
    void clearToWhite();   // push a white frame + reset pan state on navigation (no stale last-page during load / on blank)
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
    void enterFsResize();   // fullscreen: resize the WPE surface to the visible screen (video fills it, faster readback)
    void exitFsResize();    // fullscreen: restore the tall pan-buffer + scroll

    // ---- mediad system-video handoff (fullscreen HW MDP overlay + VIDC decode) ----
    // On enter-fullscreen (opt-in: /tmp/atlas_mediad) hand the fullscreened <video> to com.palm.mediad,
    // which owns the HW overlay a plain app can't get (see mediad-handoff-investigation memory). We drive
    // it via BrowserServer's LSHandle using LSCallFromApplication(appId=org.webosports.app.atlas) so mediad
    // punches the overlay into the Atlas app's card. Direct http/file URLs only (blob:/MSE have no URL).
    void mediadBegin();     // read the fullscreen <video> currentSrc+currentTime via JS, then open the session
    void mediadEnd();       // unload + drop the /service/playback subscription (frees session) + resume <video>
    void mediadFreeSurface();    // shrink the WPE GL FBO to free pmem_smipool for mediad's decode+overlay
    void mediadRestoreSurface(); // restore the full render surface on handoff exit
    void mediadCall(const char* method, const char* payload);   // LSCallFromApplicationOneReply on m_mediadLoc
    static void onFsVideoInfo(GObject*, GAsyncResult*, gpointer);   // JS finish: "src|time" (video paused in JS)
    static gboolean mediadOpenSession(gpointer);                   // deferred /service/playback open (after decoder frees)
    static bool onMediadPlayback(LSHandle*, LSMessage*, void*);     // /service/playback reply -> per-session location
    static bool onMediadReply(LSHandle*, LSMessage*, void*);        // load/setVideoBounds/play reply logger
    static gboolean mediadFirePlay(gpointer);                      // deferred play (after load settles)
    void setVirtualWindowSize(uint32_t width, uint32_t height);
    void getWindowSize(int& width, int& height);
    void getVirtualWindowSize(int& width, int& height);
    void setScrollPosition(int cx, int cy, int cw, int ch);
    bool recenterForScroll(int cx, int cy, bool force = false);   // P2: windowed pan re-center; also the onFrame catch-up. force=true bypasses the fast-flick defer (settle). Returns true if a re-render was issued.
    static int onSettleTimer(void* self);     // settle-render: fires ~160ms after the last scroll -> render the settled position
    static void onScrollConfirm(GObject*, GAsyncResult*, gpointer);  // label buffer with ACTUAL scrollY (fixes async-scrollTo mislabel jump)
    static gboolean pollVideoRect(gpointer);                         // periodic: query playing <video> rect → onVideoRect
    static void onVideoRect(GObject*, GAsyncResult*, gpointer);      // rect result → wpe_atlas_view_backend_video_rect (partial readback while a video plays)
    guint               m_videoPollTimer = 0;                        // repeating timer id for pollVideoRect
    void setZoomAndScroll(double zoom, int cx, int cy);

    // ---- autonomous scroll test (kicked with SIGUSR1; drives setScrollPosition like the adapter) ----
    void startScrollTest();      // query the REAL DOM height, then begin the sweep (via beginScrollTest)
    void beginScrollTest();      // actual top->bottom->top sweep, using the freshly-measured page height
    void runScrape(const char* js);   // run JS in the page, result -> /tmp/atlas_scrape_out (see /tmp/atlas_scrape)
    bool scrollTestStep();       // one timed step; returns false when the sweep finishes
    void reportScrollTest();     // log the SCROLLTEST summary line
    void toggleRotationTest();   // simulate a rotate: toggle window portrait<->landscape (test harness)

    // ---- input ----
    void mouseEvent(int type, int contentX, int contentY, int detail);
    bool clickAt(uint32_t contentsPosX, uint32_t contentsPosY, uint32_t numClicks);
    bool holdAt(uint32_t contentsPosX, uint32_t contentsPosY);
    void keyDown(int32_t key, int32_t modifiers, int32_t chr);
    void keyUp(int32_t key, int32_t modifiers, int32_t chr);
    void setFocus(bool enable);
    void onEditorFocus(bool focused, int purpose);   // IM-context focus → msgEditorFocused (raises webOS VKB)
    void checkEditorFocus();                          // tap / IM focus-in: probe document.activeElement → raise/hide the VKB (public: called from the IM-context focus-in callback)
    bool m_userInteracted = false;                    // gate IME autofocus: only raise the VKB after a user tap (reset per navigation)

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
    /* Web-content drag: press at start, button-held motion per delta, release. Lets in-page draggables
     * (sliders, canvas, map tiles, HTML5 DnD) work under touch. Out-of-line (uses libwpe input structs). */
    void dragEnd(int contentX, int contentY);
    void dragProcess(int deltaX, int deltaY);
    void dragStart(int contentX, int contentY);
    void enableInterrogateClicks( bool enable ) {}
    int findString( const char* str, bool fwd ) {
        if (!m_webView) return 0;
        /* On-demand reader extraction trigger (app calls viewCall findString with this magic on
         * reader-open) — extract from the CURRENT DOM, no wait for full page load. */
        if (str && strcmp(str, "__ATLAS_EXTRACT__") == 0) { extractReaderContent(); return 1; }
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
    /* Feature 9 (context menu): WPE has no synchronous hit-test, so these run elementFromPoint JS / download
     * the image asynchronously and answer via m_server->msgHitTestResponse / msgSaveImageAtPointResponse. */
    void hitTestAsync(int32_t queryNum, int32_t cx, int32_t cy);
    void saveImageAtPointAsync(int32_t queryNum, int32_t x, int32_t y, const char* dir);
    void respondSaveImage(int32_t query, bool ok, const char* path);   // Feature 9: called from the image-download callback
    /* A payload starting with \x02 is an AUTOFILL request ("\x02" user "\x02" pass), routed through this
     * existing command to avoid new IPC. Otherwise it's a plain InsertText into the focused field. */
    void insertStringAtCursor(const char* text) {
        if (!m_webView || !text) return;
        if (text[0] == '\x02') { fillLoginForm(text); return; }
        webkit_web_view_execute_editing_command_with_argument(m_webView, "InsertText", text);
    }
    void fillLoginForm(const char* payload);   // parse "\x02user\x02pass" and inject into the page's login form via JS
    bool isEditing() { return {}; }
    bool isInteractiveAtPoint( uint32_t x, uint32_t y ) { return {}; }
    void paste() { if (m_webView) webkit_web_view_execute_editing_command(m_webView, "Paste"); }
    void pluginSpotlightEnd() {}
    void pluginSpotlightStart(int x, int y, int cx, int cy) {}
    void printFrame(const char* frameName, int lpsJobId, int width, int height, int dpi, bool landscape, bool reverseOrder) {}
    int renderToFile(const char* filename, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH);
    bool saveImageAtPoint(uint32_t x, uint32_t y, QString& filepath) { return {}; }
    void scrollLayer(int id, int deltaX, int deltaY) {}
    void selectAll();   // JS select-all of the page body, then report the selection rects for the markers
    void setDNSServers(const char *servers) {}
    void setIgnoreMetaRefreshTags(bool ignore) {}
    void setIgnoreMetaViewport(bool ignore) {}
    void setMinFontSize(int minFontSizePt) { if (m_webView) webkit_settings_set_minimum_font_size(webkit_web_view_get_settings(m_webView), minFontSizePt > 0 ? (guint)minFontSizePt : 0); }
    void setMouseMode(enum BATypes::MouseMode mode) {}
    void setNetworkInterface(const char* interfaceName) {}
    void setSelectionMode(bool on) {}
    /* Feature: tap-to-select a word of web content at (docX,docY); WebKit paints the highlight natively.
     * Reports the selection's start/end rects back on the "selectionBounds" channel for the drag markers. */
    void selectWordAt(int docX, int docY);
    /* Drag a selection marker: extend the current selection to (docX,docY). whichEnd==1 keeps the current
     * start fixed and moves the end; whichEnd==0 keeps the end fixed and moves the start. Re-reports the
     * selection rects on "selectionBounds" so the markers/popover follow. */
    void extendSelectionTo(int whichEnd, int docX, int docY);
    void setShowClickedLink(bool enable) {}
    void settingsJavaScriptEnabled(bool enable) { if (m_webView) webkit_settings_set_enable_javascript(webkit_web_view_get_settings(m_webView), enable); }
    void settingsPopupsEnabled(bool enable) { if (m_webView) webkit_settings_set_javascript_can_open_windows_automatically(webkit_web_view_get_settings(m_webView), enable); }
    // Atlas: "Allow autoplay with sound" toggle. Routed db8 -> app -> adapter -> BrowserServer -> here (like
    // the other content settings). enable=true -> unmuted audio may autoplay; default false -> tap-to-play.
    // Toggles /tmp/atlas_autoplay, which HTMLMediaElement reads at media-element creation (a WebKit preference
    // won't work: live preference changes don't propagate to the warm WebProcess in this port). Only affects
    // unmuted audio; muted video autoplay is unchanged. We STORE the value and (re)assert the flag in
    // ensureWebView() before each page load, so the state is enforced (default false -> flag removed) rather
    // than relying on the app to clear it; the command is processed before openUrl.
    void settingsAutoplayWithSound(bool enable) { m_autoplayWithSound = enable; applyAutoplayFlag(); }
    void applyAutoplayFlag() { if (m_autoplayWithSound) { FILE* f = fopen("/tmp/atlas_autoplay", "w"); if (f) fclose(f); } else remove("/tmp/atlas_autoplay"); }
    bool m_autoplayWithSound = false;
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
    /* Copy: run the native Copy command AND ferry the current selection text to the app (msgActionData
     * "copiedText") so it can place it on the webOS system clipboard (WebKit's own clipboard isn't wired
     * to LunaSysMgr). Async read via the proven evaluate_javascript path. */
    bool copy() {
        if (m_webView) {
            webkit_web_view_execute_editing_command(m_webView, "Copy");
            webkit_web_view_evaluate_javascript(m_webView,
                "(function(){var a=document.activeElement;"
                "if(a&&(a.tagName=='INPUT'||a.tagName=='TEXTAREA')&&a.selectionStart!=a.selectionEnd)"
                "return a.value.substring(a.selectionStart,a.selectionEnd);"
                "return window.getSelection?window.getSelection().toString():'';})()",
                -1, nullptr, nullptr, nullptr, &BrowserPageWPE::onCopyText, this);
        }
        return true;
    }

    // ---- manager/main extras ----
    uint32_t getPriority() { return m_priority; }
    void setPriority(uint32_t p) { m_priority = p; }
    static void setInspectorPort(int port) { (void)port; }

private:
    // backend frame sink (UIProcess thread): copy ARGB → current offscreen, flushBuffer
    static void onFrame(void* ud, const uint8_t* argb, uint32_t w, uint32_t h, uint32_t stride);

    // WPE WebKit signal handlers
    static void onLoadChanged(WebKitWebView*, WebKitLoadEvent, gpointer);
    static gboolean perfTimingTimeout(gpointer);                     // 1.2s after finish -> dump Navigation Timing
    static gboolean contentSizePollTimeout(gpointer);                // post-load: re-measure page height for a bounded window so JS-grown pages (html5test) get their scroll range extended
    static void onPerfTiming(GObject*, GAsyncResult*, gpointer);     // timing result -> WLOG("PERF ...")
    static void onLoadFailed(WebKitWebView*, WebKitLoadEvent, const char* uri, GError*, gpointer);
    static void onTitleChanged(GObject*, GParamSpec*, gpointer);
    static void onUriChanged(GObject*, GParamSpec*, gpointer);
    static void onProgressChanged(GObject*, GParamSpec*, gpointer);
    static int  onDecidePolicy(WebKitWebView*, WebKitPolicyDecision*, WebKitPolicyDecisionType, gpointer);  // unsupported-mime → download handoff

    // Feature 6: atlas: internal scheme (atlas:about diagnostics page)
    static void     onAtlasScheme(WebKitURISchemeRequest* req, gpointer ud);
    // Feature 7: SSL cert + HTTP auth dialogs (round-trip to app UI via the sync reply pipe)
    static gboolean onAuthenticate(WebKitWebView*, WebKitAuthenticationRequest*, gpointer);
    static gboolean onTlsErrors(WebKitWebView*, const char* uri, GTlsCertificate*, GTlsCertificateFlags, gpointer);
    static void onWebProcessTerminated(WebKitWebView*, WebKitWebProcessTerminationReason, gpointer);
    void applyUserAgentQuirk(const char* url);   // per-site UA (MS Teams cluster gets macOS Safari)
    static const char* const kAtlasDefaultUA;
    static const char* const kAtlasMacSafariUA;

    void ensureWebView();                    // create m_viewBackend + m_webView once size is known
    void dispatchPointer(int x, int y, uint32_t button, bool down);
    void updateContentsSize();               // async-query the real page height for scroll range
    static void onContentHeight(GObject*, GAsyncResult*, gpointer);
    static void onScrollTestHeight(GObject*, GAsyncResult*, gpointer);  // scroll-test: real height -> beginScrollTest
    void extractReaderContent();                                   // extract current DOM's article, push to app via msgActionData (on-demand from reader-open)
    static void onReaderProbe(GObject*, GAsyncResult*, gpointer);  // extraction result -> msgActionData("readerContent")
    static void onEditorCheckResult(GObject*, GAsyncResult*, gpointer);
    static void onSmartZoomResult(GObject*, GAsyncResult*, gpointer);   // double-tap zoom: block rect → response
    int m_smartZoomX = 0, m_smartZoomY = 0;          // echoed back in the smart-zoom response
    static void onHitTestResult(GObject*, GAsyncResult*, gpointer);      // Feature 9: JS elementFromPoint result → msgHitTestResponse
    static void onSaveImgSrcResult(GObject*, GAsyncResult*, gpointer);   // Feature 9: image src at point → start download
    int m_hitTestQuery = 0;                  // Feature 9: queryNum echoed in the hit-test/save-image response
    int m_saveImgQuery = 0;
    static void onLoginCapture(GObject*, GAsyncResult*, gpointer);   // save-login: form-submit snapshot → msgActionData("saveLogin")
    static void onScrapeResult(GObject*, GAsyncResult*, gpointer);   // -> /tmp/atlas_scrape_out
    static gboolean onLoginDeferTimeout(gpointer);                   // failsafe: resume a deferred login-snapshot navigation if the capture callback never fires
    static void onCopyText(GObject*, GAsyncResult*, gpointer);       // copy: selection text → msgActionData("copiedText")
    static void onSelectResult(GObject*, GAsyncResult*, gpointer);   // select: start/end rects → msgActionData("selectionBounds")
    void reportCurrentSelection();                                   // eval current selection → "selectionBounds" (len:0 if none) so the app markers track it
    static gboolean onSelReportTimer(gpointer ud);                  // deferred report after a long-press release (resync markers)
    void clearSelectionAndReport();                                  // removeAllRanges + report {len:0} → app dismisses markers/popover
    static gboolean onSelClearTimer(gpointer ud);                   // deferred clear+report after a QUICK tap (dismiss the selection)
    int m_dragX = 0, m_dragY = 0;            // web-content drag: tracked document-space pointer position
    std::string m_saveImgDir;                // Feature 9: dir to save the image into (/media/internal or /tmp)
    std::string m_saveImgInFlight;           // dest of an in-progress save — drop a duplicate so two downloads don't collide on one file
    WebKitPolicyDecision* m_loginDecision = nullptr;  // held form-submit decision — navigation deferred until the save-login snapshot completes
    gint64 m_lastLoginDeferMs = 0;                    // monotonic ms of the last deferral — suppression window so the post-login redirect (also type=OTHER) isn't stalled

    // ---- Atlas members (mirror BrowserPage) ----
    BrowserServer*      m_server;
    YapProxy*           m_proxy;
    char*               m_identifier;
    BrowserOffscreenQt* m_offscreen0;
    BrowserOffscreenQt* m_offscreen1;
    bool                m_ownOffscreen0;
    bool                m_ownOffscreen1;
    int                 m_windowWidth, m_windowHeight;
    int                 m_virtualWindowWidth, m_virtualWindowHeight;
    int                 m_screenWidth;          // real fb width; layout may be wider (fit-to-screen via contentZoom)
    int                 m_screenHeight;         // real fb height (visible rows) — the pannable window height
    bool                m_fsPending = false;    // fullscreen: a deferred fill/resize is queued (cleared on leave)
    bool                m_fsActive = false;     // fullscreen: screen-sized surface resize is active
    int                 m_fsRestoreH = 0;       // tall renderHeight to restore on fullscreen exit
    int                 m_fsRestoreScrollY = 0; // scroll pos to restore on fullscreen exit
    // mediad handoff state
    bool                m_mediadActive = false; // a system-media session is live for this page's fullscreen
    LSMessageToken      m_mediadSubToken = 0;   // /service/playback subscription token (open == session alive)
    std::string         m_mediadLoc;            // per-session service, e.g. com.palm.mediad.MediaPlayer_1234
    std::string         m_mediadSrc;            // the video URL handed off (for the load payload)
    double              m_mediadResumeTime = 0; // in-browser <video>.currentTime to restore on exit
    std::vector<LSMessageToken> m_mediadCallTokens;  // held load/play/... subscriptions (mediad ties pipeline to them)
    int                 m_mediadSavedW = 0, m_mediadSavedH = 0;  // render size to restore after freeing the surface
    int                 m_renderedY;            // pan model: content-Y of the buffer's top; buffer holds [m_renderedY, +m_renderHeight]
    int                 m_deliveredY;           // renderedY of the LAST delivered frame (what the adapter is actually showing) — scroll-test coverage metric
    gint64              m_touchDownUs = 0;      // monotonic timestamp of the last touch-down, to tell a quick tap (dismiss selection) from a long-press (select)
    int                 m_lastSelDocX = -100000, m_lastSelDocY = -100000;  // last selectWordAt point
    gint64              m_lastSelMs = 0;        // when — so clickAt can swallow the long-press RELEASE click (same point, right after) instead of collapsing the fresh selection
    int                 m_renderWidth, m_renderHeight;  // ACTUAL frame size the backend outputs (== screen size when downscaling; else == layout). onFrame validates against these; bufferWidth = m_renderWidth.
    double              m_uiZoom = 1.0;                 // adapter's pinch/content zoom (mZoomLevel). Used to convert the (zoomed) scroll the adapter sends into document space.
    double              m_webkitZoom = 1.0;             // the webkit_web_view zoom level we render the buffer at (= m_uiZoom/fit, clamped >=1). onFrame reports contentZoom = fit*m_webkitZoom so the adapter blit divides back to a SHARP 1:1 after the crisp re-render.
    int                 m_scrollX, m_scrollY;   // last scroll pos (content→view tap mapping)
    long                m_lastScrollMs;         // predictive pan: timestamp + Y of the previous scroll event
    int                 m_lastScrollY, m_scrollVel;  // m_scrollVel = px/sec, signed (down +). Re-center earlier + biased in this direction.
    int                 m_dirStreak;    // consecutive same-direction scroll moves (signed); predictive lead only applies when SUSTAINED (not oscillation)
    int                 m_pendingOldY;  // renderedY before the in-flight re-center — for the confirmed strip delta (confirm mode)
    int                 m_pageHeight;           // P1: scaled document height; if <= m_renderHeight the whole page fits -> never re-render
    int                 m_lastReportedH;        // last height pushed to the adapter (msgContentsSizeChanged) — only push on change
    int                 m_contentPollTicks;     // post-load height re-measure ticks (JS-grown pages e.g. html5test expand AFTER load-finished)
    bool                m_navByHistory; // pageBackward/Forward set this -> LOAD_COMMITTED skips the blank-flush (bf-cache restore is instant + needs the buffer)
    bool                m_focused;
    bool                m_frozen;
    bool                m_private;     // ephemeral session (private browsing) — set by the openUrl marker
    bool                m_simpleMode;  // MODE 2: viewport-only render (mult=1), no tall pan buffer — for non-scrolling
                                       // cards (OAuth popups, app-style SPAs like web.whatsapp.com). Set by the
                                       // "atlas-simple:" openUrl marker (app JS adds it from an optional launch param).
    bool                m_prewarmBlank; // BPWPE_PRESPAWN: WebView pre-warmed with about:blank (non-private)

    // --- DeviceOrientation / DeviceMotion injection (fed by the atlas-sensord HAL helper) ---
    // A document-start shim exposes window.__atlasWant (bitmask 1=orientation 2=motion, set when the page
    // adds a listener) + window.__atlasTick(ax,ay,az,gx,gy,gz) which builds+dispatches the events. A low-rate
    // PROBE reads __atlasWant via evaluate_javascript (the proven-safe path — NOT script-message-received,
    // which crashed the BS); when a page wants sensors we open /tmp/atlas_sensord.sock and PUSH samples.
    int                 m_sensorSock  = -1;   // Unix socket to atlas-sensord, -1 = disconnected
    guint               m_sensorProbe = 0;    // ~400ms timer: reads window.__atlasWant
    guint               m_sensorPush  = 0;    // ~40ms timer: pushes samples while a page wants them
    int                 m_sensorWant  = 0;    // last __atlasWant read (1=orientation, 2=motion)
    GCancellable*       m_sensorCancel = nullptr; // cancels in-flight probe evaluates on teardown
    double              m_sensLast[6] = {};   // last-pushed ax,ay,az,gx,gy,gz — skip near-identical samples
    gint64              m_sensLastPushUs = 0; // monotonic us of last push (for the idle heartbeat)
    void                setupSensorInjection();
    void                startSensorProbe();
    void                startSensorPush();
    bool                connectSensord();   // (re)connect m_sensorSock to atlas-sensord; true if connected
    void                stopSensors();
    static gboolean     sensorProbeTimer(gpointer self);
    static void         onSensorWant(GObject* obj, GAsyncResult* res, gpointer self);
    static gboolean     sensorPushTimer(gpointer self);
    bool                m_renderPending; // pan re-render in flight; don't queue another (m_renderedY would race ahead of the delivered buffer)
    long                m_renderPendingMs; // _wlog_ms() when m_renderPending was set — staleness timeout so a lost frame can't deadlock the pan
    unsigned int        m_settleTimer;   // settle-render: g_timeout id armed while a fast flick defers re-render; fires the settled re-render
    long                m_lastRenderMs;  // settle-render: _wlog_ms() of the last ISSUED re-render — max-defer safeguard so continuous motion never freezes
    uint32_t            m_priority = 0;

    // ---- WPE members (replace m_graphicsView/m_scene/m_webView/m_webPage) ----
    struct wpe_view_backend* m_viewBackend;
    WebKitWebView*           m_webView;
    std::string              m_userAgent;
    std::string              m_lastFind;     // last find query → new vs next/prev (highlight-all stays up)
    int                      m_memLimit = 480;        // Feature 6: WebProcess memory budget (MB) shown in atlas:about
    BrowserSyncReplyPipe*    m_syncReplyPipe = nullptr; // Feature 7: lazily-created sync pipe for SSL/auth dialogs
};

#endif /* BROWSER_PAGE_WPE_H */
