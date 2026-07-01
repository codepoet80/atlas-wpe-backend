/*
 * BrowserPageWPE.cpp — WPE WebKit BrowserPage for the Isis BrowserServer. See BrowserPageWPE.h.
 * Real browse path; long-tail commands stubbed with TODO. Uses the WPE WebKit C API + the custom
 * libwpe backend (wpe_isis_view_backend_create). Input is dispatched on the wpe_view_backend (it
 * forwards to the WebProcess); rendering returns via onFrame() → Isis offscreen → msgPainted.
 */
#include "BrowserPageWPE.h"
#include "BrowserServer.h"
#include "BrowserOffscreenQt.h"
#include "YapProxy.h"
#include "wpe-isis-backend.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <sys/time.h>

/* Log to a file (webOS syslog goes to PmLog, hard to read on-device). Timestamped (ms) so we can
 * measure load phases. */
static inline long _wlog_ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return (tv.tv_sec % 10000) * 1000 + tv.tv_usec / 1000; }
/* Off unless BPWPE_DEBUG is set (checked once) — keeps the per-frame/per-event file I/O out of
 * production while staying available on-device by setting the env var in the launch wrapper. */
static inline bool _wlog_on(void) { static int on = -1; if (on < 0) on = getenv("BPWPE_DEBUG") ? 1 : 0; return on; }
#define WLOG(fmt, ...) do { if (_wlog_on()) { FILE* _f = fopen("/tmp/bpwpe.log", "a"); \
    if (_f) { fprintf(_f, "[%08ld] " fmt "\n", _wlog_ms(), ##__VA_ARGS__); fclose(_f); } } } while (0)

/* ---- Crash capture: on a fatal signal log a backtrace to /tmp/bpwpe.log so an abrupt BS death
 * (the heavy-page interaction segfault) leaves the fault + call stack to resolve offline. ---- */
#include <execinfo.h>
#include <signal.h>
static void bpwpe_crash_handler(int sig)
{
    void* bt[40]; int n = backtrace(bt, 40);
    FILE* f = fopen("/tmp/bpwpe.log", "a");
    if (f) { fprintf(f, "[CRASH] sig=%d frames=%d\n", sig, n);
        char** s = backtrace_symbols(bt, n);
        for (int i = 0; i < n; i++) fprintf(f, "  #%d %s\n", i, (s && s[i]) ? s[i] : "?");
        fclose(f); }
    signal(sig, SIG_DFL); raise(sig);
}
__attribute__((constructor)) static void bpwpe_install_crash_handler(void)
{
    signal(SIGSEGV, bpwpe_crash_handler);
    signal(SIGABRT, bpwpe_crash_handler);
    signal(SIGBUS,  bpwpe_crash_handler);
}

/* Device display resolution comes from webOS config (/etc/palm/luna.conf: DisplayWidth/DisplayHeight),
 * NOT hardcoded. Returns <=0 if the key isn't found so callers can fall back to the framebuffer. */
static int lunaConfInt(const char* key)
{
    FILE* f = fopen("/etc/palm/luna.conf", "r");
    if (!f) return -1;
    char line[256]; int val = -1; size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') { val = atoi(line + klen + 1); break; }
    fclose(f);
    return val;
}

/* ---- Editor-focus → webOS VKB bridge (2.52 / 2.0 API). WPE has no QtWebKit microFocusChanged
 * signal; the WebView instead drives a WebKitInputMethodContext when an editable element gains or
 * loses focus. We subclass it, override notify_focus_in/out, and forward to the page, which raises
 * the webOS virtual keyboard via msgEditorFocused — the same adapter signal the legacy QtWebKit
 * BrowserServer used. ---- */
struct IsisIMContext      { WebKitInputMethodContext      parent_instance; BrowserPageWPE* page; };
struct IsisIMContextClass { WebKitInputMethodContextClass parent_class; };
G_DEFINE_TYPE(IsisIMContext, isis_im_context, WEBKIT_TYPE_INPUT_METHOD_CONTEXT)
static void isis_im_focus_in(WebKitInputMethodContext* c) {
    IsisIMContext* s = reinterpret_cast<IsisIMContext*>(c);
    if (s->page) s->page->onEditorFocus(true, (int)webkit_input_method_context_get_input_purpose(c));
}
static void isis_im_focus_out(WebKitInputMethodContext* c) {
    IsisIMContext* s = reinterpret_cast<IsisIMContext*>(c);
    if (s->page) s->page->onEditorFocus(false, 0);
}
static void isis_im_context_class_init(IsisIMContextClass* k) {
    WebKitInputMethodContextClass* c = WEBKIT_INPUT_METHOD_CONTEXT_CLASS(k);
    c->notify_focus_in  = isis_im_focus_in;
    c->notify_focus_out = isis_im_focus_out;
}
static void isis_im_context_init(IsisIMContext*) {}

#include <set>
/* Live-page guard: a destroyed BrowserPageWPE's backend/conn can still deliver a queued
 * MSG_FRAME_READY — the WebView isn't always finalized synchronously on unref, so view_destroy
 * hasn't run, the isis_view lingers, and v->userdata still points at the freed page. onFrame checks
 * this set so a stale frame on a freed page is dropped instead of dereferencing freed memory.
 * Everything (ctor/dtor/onFrame) runs on the glib main loop, so no locking is needed. */
static std::set<BrowserPageWPE*> g_livePages;

BrowserPageWPE::BrowserPageWPE(BrowserServer* server, YapProxy* proxy)
    : m_server(server), m_proxy(proxy), m_identifier(0)
    , m_offscreen0(0), m_offscreen1(0), m_ownOffscreen0(false), m_ownOffscreen1(false)
    , m_bufferLock(0), m_bufferLockName(0)
    , m_windowWidth(1024), m_windowHeight(768)
    , m_virtualWindowWidth(0), m_virtualWindowHeight(0)
    , m_screenWidth(0), m_screenHeight(0), m_renderedY(0)
    , m_renderWidth(0), m_renderHeight(0)
    , m_scrollX(0), m_scrollY(0)
    , m_focused(true), m_frozen(false), m_private(false), m_prewarmBlank(false)
    , m_viewBackend(0), m_webView(0)
{
    g_livePages.insert(this);
    WLOG("ctor %p", this);
}

BrowserPageWPE::~BrowserPageWPE()
{
    g_livePages.erase(this);   // stop onFrame touching this page the moment we start tearing down
    if (m_webView) {
        /* Force-kill the WebProcess. Do NOT rely on WebView finalization to terminate it — the WebView
         * frequently lingers (stray refs; same reason the stale-frame UAF existed), so on card close the
         * WebProcess (tens of MB each) + backend + conns leak. They pile up until webOS OOMs with
         * "Too Many Cards". Terminating explicitly reclaims the WebProcess memory immediately. */
        webkit_web_view_terminate_web_process(m_webView);
        g_object_unref(m_webView);   // also destroys the backend + view backend
    }
    free(m_identifier);
    free(m_bufferLockName);
    delete m_offscreen0;
    delete m_offscreen1;
}

/* ---- create the WPE view bound to the Isis frame sink (once size is known) ----------------- */
/* The page asked to open a new window/popup (window.open, target=_blank). Many cookie-consent
 * CMPs open the consent UI this way. Isis has one WebView per card, so load the popup target in
 * THIS view rather than dropping it (which is why "no consent popup" appeared). */
static WebKitWebView* bpwpe_on_create(WebKitWebView* view, WebKitNavigationAction* action, gpointer)
{
    WebKitURIRequest* req = action ? webkit_navigation_action_get_request(action) : nullptr;
    const char* uri = req ? webkit_uri_request_get_uri(req) : nullptr;
    WLOG("CREATE (popup/window.open) uri=%s", uri ? uri : "(none)");
    if (uri && *uri) webkit_web_view_load_uri(view, uri);
    return nullptr;   /* no separate WebView */
}

/* Permission prompts (notifications, geolocation, media…). Log what's asked; grant notifications
 * (some consent flows gate on them) and deny the rest by default. */
static gboolean bpwpe_on_permission(WebKitWebView*, WebKitPermissionRequest* req, gpointer)
{
    const char* type = G_OBJECT_TYPE_NAME(req);
    bool isNotif = WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(req);
    WLOG("PERMISSION request type=%s -> %s", type ? type : "?", isNotif ? "allow" : "deny");
    if (isNotif) webkit_permission_request_allow(req);
    else         webkit_permission_request_deny(req);
    return TRUE;
}

/* Download handoff: WebKit can't DISPLAY this response (a file — .zip/.pdf/.apk/etc), so emit
 * msgDownloadStart. The adapter forwards it to the Isis app (BrowserApp.js downloadResource),
 * which hands the URL to com.palm.downloadmanager — the webOS system download service. We do NOT
 * download in the BS; webOS owns the download queue/notifications/storage. */
int BrowserPageWPE::onDecidePolicy(WebKitWebView*, WebKitPolicyDecision* decision,
                                   WebKitPolicyDecisionType type, gpointer ud)
{
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        WebKitResponsePolicyDecision* rd = WEBKIT_RESPONSE_POLICY_DECISION(decision);
        if (!webkit_response_policy_decision_is_mime_type_supported(rd)) {
            WebKitURIResponse* resp = webkit_response_policy_decision_get_response(rd);
            const char* uri  = resp ? webkit_uri_response_get_uri(resp) : nullptr;
            const char* mime = resp ? webkit_uri_response_get_mime_type(resp) : nullptr;
            BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
            /* msgMimeHandoffUrl (NOT msgDownloadStart) — the adapter maps it to the app's onFileLoad →
             * handleResource → resourceInfoService → downloadResource → com.palm.downloadmanager. */
            WLOG("download: unsupported mime '%s' -> msgMimeHandoffUrl %s", mime ? mime : "?", uri ? uri : "?");
            if (uri && self->m_server)
                self->m_server->msgMimeHandoffUrl(self->m_proxy, mime ? mime : "application/octet-stream", uri);
            webkit_policy_decision_ignore(decision);
            return TRUE;
        }
    }
    return FALSE;   /* default policy for navigation / new-window / displayable responses */
}

/* ---- ad/tracker content blocker (WKContentRuleList) ---------------------------------------- *
 * Loads a precompiled rule list if present (fast); otherwise compiles blocklist.json once and
 * caches it. blocklist.json is WebKit content-blocker JSON; /media/internal/wpe-glibc/update-
 * blocklist.sh regenerates it from a public source (optionally daily). All async so it never
 * blocks page creation — the filter applies from the next load if it isn't ready for the first. */
#define CB_STORE_DIR "/media/internal/wpe-glibc/cb-store"
#define CB_JSON      "/media/internal/wpe-glibc/blocklist.json"
#define CB_ID        "adblock"
static WebKitUserContentFilterStore* s_cbStore = nullptr;

static void cb_on_saved(GObject* store, GAsyncResult* res, gpointer ud)
{
    WebKitUserContentManager* ucm = WEBKIT_USER_CONTENT_MANAGER(ud);
    GError* err = nullptr;
    WebKitUserContentFilter* f = webkit_user_content_filter_store_save_finish(
        WEBKIT_USER_CONTENT_FILTER_STORE(store), res, &err);
    if (f) { webkit_user_content_manager_add_filter(ucm, f); webkit_user_content_filter_unref(f);
             WLOG("content blocker: compiled + applied"); }
    else   { WLOG("content blocker compile FAILED: %s", err ? err->message : "?"); if (err) g_error_free(err); }
    g_object_unref(ucm);
}
static void cb_on_loaded(GObject* store, GAsyncResult* res, gpointer ud)
{
    WebKitUserContentManager* ucm = WEBKIT_USER_CONTENT_MANAGER(ud);
    GError* err = nullptr;
    WebKitUserContentFilter* f = webkit_user_content_filter_store_load_finish(
        WEBKIT_USER_CONTENT_FILTER_STORE(store), res, &err);
    if (f) { webkit_user_content_manager_add_filter(ucm, f); webkit_user_content_filter_unref(f);
             WLOG("content blocker: applied (cached)"); g_object_unref(ucm); return; }
    if (err) g_error_free(err);
    gchar* json = nullptr; gsize len = 0;
    if (g_file_get_contents(CB_JSON, &json, &len, nullptr) && len > 0) {
        GBytes* b = g_bytes_new_take(json, len);
        webkit_user_content_filter_store_save(s_cbStore, CB_ID, b, nullptr, cb_on_saved, ud); /* passes ucm ref */
        g_bytes_unref(b);
    } else { WLOG("content blocker: no %s", CB_JSON); g_object_unref(ucm); }
}
static void applyContentBlocker(WebKitUserContentManager* ucm)
{
    if (!ucm) return;
    if (!s_cbStore) s_cbStore = webkit_user_content_filter_store_new(CB_STORE_DIR);
    webkit_user_content_filter_store_load(s_cbStore, CB_ID, nullptr, cb_on_loaded, g_object_ref(ucm));
}

void BrowserPageWPE::ensureWebView()
{
    if (m_webView) return;

    /* TRUE single-process (PSON off) is still unreachable on 2.38 (needs a WebKit source patch — the
     * usesSingleWebProcess flag is internal-C-API-only). But the memory-pressure TUNING is reachable:
     * it needs a CUSTOM context (construct-time property). The custom context's one real downside was an
     * isolated, MEMORY-ONLY cookie jar that re-triggered nu.nl/DPG's consent every session — but cookies
     * were never persisted for the default context either, so the real bug is missing cookie persistence.
     * We add it below, so we get the tuned pressure AND cookies that finally survive across BS restarts. */
    WebKitMemoryPressureSettings* webMem = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(webMem, 480);             /* MB budget / web process */
    webkit_memory_pressure_settings_set_conservative_threshold(webMem, 0.40);  /* purge discardable @ ~192MB */
    webkit_memory_pressure_settings_set_strict_threshold(webMem, 0.60);        /* aggressive purge + GC @ ~288MB */
    webkit_memory_pressure_settings_set_kill_threshold(webMem, 0.95);          /* force-kill runaway @ ~456MB */
    webkit_memory_pressure_settings_set_poll_interval(webMem, 2.0);            /* seconds */
    WebKitWebContext* ctx = WEBKIT_WEB_CONTEXT(g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
        "memory-pressure-settings", webMem, NULL));
    webkit_memory_pressure_settings_free(webMem);

    /* Persistent cookie storage — the missing piece. Cookies were memory-only (no store on disk), so
     * consent/logins died on every BS restart and consent-gated sites (nu.nl/DPG) bounced to their
     * consent page each session. Point the jar at a SQLite file on persistent storage: accept once, it
     * sticks across reloads + restarts. ACCEPT_ALWAYS so 1st-party consent cookies are actually saved. */
    /* 2.0 API (2.52): cookies/data/network live on a WebKitNetworkSession, not the context. Make a
     * PERSISTENT session (on-disk dirs) so cookies/logins survive BS restarts, and pin the cookie jar to
     * an explicit SQLite file + ACCEPT_ALWAYS (the consent-persistence fix carried over from the 1.0 port). */
    /* Private browsing: an ephemeral session holds cookies/cache/history in memory only — nothing
     * touches disk and it's discarded with the card. Triggered PER-CARD by a "private" marker in the
     * identifier (the app tags a private card via setIdentifier), or globally via BPWPE_PRIVATE for
     * testing. Otherwise the normal PERSISTENT session (consent/logins survive BS restarts). */
    bool isPrivate = getenv("BPWPE_PRIVATE") || m_private;
    WebKitNetworkSession* session;
    if (isPrivate) {
        session = webkit_network_session_new_ephemeral();
        WLOG("PRIVATE browsing (id=%s) -> ephemeral session, no persistence", m_identifier ? m_identifier : "(env)");
    } else {
        session = webkit_network_session_new("/media/internal/wpe-252/netdata",
                                             "/media/internal/wpe-252/netcache");
        WebKitCookieManager* cm = webkit_network_session_get_cookie_manager(session);
        webkit_cookie_manager_set_persistent_storage(cm, "/media/internal/wpe-252/cookies.db",
                                                     WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
        webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
    }

    /* (webkit_web_context_set_web_process_count_limit removed in the 2.0 API — it was a no-op anyway.)
     * DOCUMENT_VIEWER = lowest-memory cache model (zeroes bf-cache); stops suspended-process swap thrash. */
    webkit_web_context_set_cache_model(ctx, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);

    /* Render at the FULL device width. The adapter seeds m_virtualWindowWidth at ~960, which
     * leaves a grey bar on the 1024-wide viewport; and the backend size is fixed at create
     * (resize is a no-op), so we can't widen later. Read the real screen width from the
     * framebuffer; keep the virtual (buffer) height for scroll room. Set m_virtualWindowWidth to
     * match so onFrame's size check + the offscreen header agree with what we actually render. */
    int renderW = m_virtualWindowWidth, renderH = m_virtualWindowHeight;
    /* Screen dims from webOS config (luna.conf), falling back to the framebuffer — never hardcoded. */
    int screenW = lunaConfInt("DisplayWidth");
    int screenH = lunaConfInt("DisplayHeight");
    if (screenW <= 0 || screenH <= 0) {
        int fbfd = open("/dev/fb0", O_RDONLY);
        if (fbfd >= 0) { struct fb_var_screeninfo vi;
            if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vi) == 0) {
                if (screenW <= 0) screenW = (int)vi.xres;
                if (screenH <= 0) screenH = (int)vi.yres;
            }
            close(fbfd); } }
    if (screenW < renderW) screenW = renderW;   /* never narrower than the adapter's layout width */
    m_screenWidth  = screenW;
    m_screenHeight = screenH;                    /* 0 if unknown → setScrollPosition uses the safe path */
    /* Fit-to-screen ("overview" on load) — the PROPER webOS path matching the legacy
     * BrowserOffscreenInfo contract: lay the page out at a WIDER layout width W so desktop sites use
     * the desktop layout, then DOWNSCALE each rendered frame to the SCREEN width so the offscreen
     * BUFFER genuinely holds the page rendered at contentZoom = screenW/W (renderedWidth==bufferWidth,
     * zoom baked in). The backend does the scale (GPU pass, CPU-bilinear fallback); onFrame reports
     * contentZoom. Keep the tall virtual height (the adapter's pan/scroll needs it). Layout width is
     * tunable via BPWPE_LAYOUT_WIDTH; default = screen width (1:1, no scaling, safe). */
    const char* lwEnv = getenv("BPWPE_LAYOUT_WIDTH");
    int layoutW = lwEnv ? atoi(lwEnv) : screenW;
    if (layoutW < screenW) layoutW = screenW;                         // never narrower than the screen
    if ((long)layoutW * renderH * 4 > 12000000L) layoutW = screenW;   // guard the ~12MB FBO/shm
    renderW = layoutW;
    m_virtualWindowWidth = renderW;                                   // layout = document coordinate space

    /* The backend downscales the layout-size FBO to this SCREEN size before readback, so the buffer
     * the adapter receives already IS the fit-to-screen content. 0 => backend renders 1:1. */
    int scaledW = 0, scaledH = 0;
    if (screenW > 0 && renderW > screenW) {
        scaledW = screenW;
        scaledH = (int)(((long)renderH * screenW + renderW / 2) / renderW);   // preserve aspect
    }
    m_renderWidth  = scaledW ? scaledW : renderW;    // what the backend ACTUALLY outputs (onFrame validates)
    m_renderHeight = scaledH ? scaledH : renderH;
    WLOG("ensureWebView layout %dx%d -> render %dx%d (screen=%d contentZoom=%.3f)",
         renderW, renderH, m_renderWidth, m_renderHeight, screenW, (double)m_renderWidth/renderW);

    m_viewBackend = wpe_isis_view_backend_create(renderW, renderH, scaledW, scaledH,
                                                 &BrowserPageWPE::onFrame, this);
    WebKitWebViewBackend* vb = webkit_web_view_backend_new(m_viewBackend, nullptr, nullptr);
    m_webView = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,    // 2.0: context + network session
                                             "backend", vb, "web-context", ctx,
                                             "network-session", session, NULL));
    g_object_ref_sink(m_webView);
    g_object_unref(ctx);       // the WebView holds its own refs now
    g_object_unref(session);

    /* Viewport scale-to-fit: device-scale-factor sets CSS layout width = backend_width / DSF. DSF<1
     * lays the page out WIDER than the screen and renders it scaled down (the legacy "overview" so
     * desktop sites fit on load). Tunable via BPWPE_DSF (default 1.0 = 1:1 at 1024) to dial in live. */
    const char* dsfEnv = getenv("BPWPE_DSF");
    /* g_ascii_strtod, NOT atof: atof honors the locale and returns 0.0 for "0.66" under nl_NL (comma
     * decimal), which silently disabled this. g_ascii_strtod always parses '.' regardless of locale. */
    float dsf = dsfEnv ? (float)g_ascii_strtod(dsfEnv, nullptr) : 1.0f;
    if (dsf > 0.0f && dsf != 1.0f) {
        WLOG("device-scale-factor = %.3f (layout width ~%d)", dsf, (int)(renderW / dsf));
        wpe_view_backend_dispatch_set_device_scale_factor(m_viewBackend, dsf);
    }

    WebKitSettings* s = webkit_web_view_get_settings(m_webView);
    webkit_settings_set_javascript_can_open_windows_automatically(s, TRUE);
    webkit_settings_set_enable_developer_extras(s, FALSE);
    webkit_settings_set_enable_page_cache(s, FALSE);   /* save memory on this 941MB device (OOM "Too Many Cards") */
    /* ALWAYS use a modern WebKit/Safari UA — the adapter otherwise sends the legacy
     * "HP TouchPad webOS 3.0.5" UA, making sites think we're the old engine.
     * WebKit 620 / Safari 18 = WPE 2.52 (was 612/15.0 = WPE 2.34-era). */
    /* Drop the hp-tablet/hpwOS tokens — Google (search/suggest) blocks them with
     * "Update your browser". webOS/6.0 reads as a modern webOS engine. */
    webkit_settings_set_user_agent(s,
        "Mozilla/5.0 (Linux; webOS/6.0; U; en-US) AppleWebKit/620.1.16 "
        "(KHTML, like Gecko) Version/18.0 Safari/620.1.16");
    WLOG("UA set to: %s", webkit_settings_get_user_agent(s) ? webkit_settings_get_user_agent(s) : "?");

    g_signal_connect(m_webView, "load-changed",  G_CALLBACK(onLoadChanged), this);
    g_signal_connect(m_webView, "load-failed",   G_CALLBACK(onLoadFailed), this);
    g_signal_connect(m_webView, "notify::title", G_CALLBACK(onTitleChanged), this);
    g_signal_connect(m_webView, "notify::uri",   G_CALLBACK(onUriChanged), this);
    g_signal_connect(m_webView, "notify::estimated-load-progress", G_CALLBACK(onProgressChanged), this);
    g_signal_connect(m_webView, "create",             G_CALLBACK(bpwpe_on_create), this);
    g_signal_connect(m_webView, "permission-request", G_CALLBACK(bpwpe_on_permission), this);
    g_signal_connect(m_webView, "decide-policy",      G_CALLBACK(onDecidePolicy), this);  // unsupported-mime → download

    applyContentBlocker(webkit_web_view_get_user_content_manager(m_webView));

    /* Editor-focus → VKB is driven by checkEditorFocus() after each tap (see clickAt/mouseEvent): it
     * polls document.activeElement via evaluate_javascript — the same proven-safe path as the page-
     * height query. The script-message-received channel was tried but its signal value crashed the BS
     * (jsc_value_* on a non-JSCValue at the UIProcess), so it is intentionally not used. */
}

/* ---- Isis buffer attach (mirrors BrowserPage::attachToBuffer) ------------------------------ */
bool BrowserPageWPE::attachToBuffer(uint32_t vWidth, uint32_t vHeight,
                                    int key1, int key2, int size)
{
    WLOG("attachToBuffer v=%ux%u keys=%d,%d size=%d", vWidth, vHeight, key1, key2, size);
    m_virtualWindowWidth = vWidth; m_virtualWindowHeight = vHeight;

    if (key1 && size > 0) { m_offscreen0 = BrowserOffscreenQt::attach(key1, size); m_ownOffscreen0 = (m_offscreen0 != 0); }
    if (key2 && size > 0) { m_offscreen1 = BrowserOffscreenQt::attach(key2, size); m_ownOffscreen1 = (m_offscreen1 != 0); }
    if (!m_offscreen0 || !m_offscreen1) { WLOG("attach failed"); return false; }

    /* Do NOT create the WebView here — defer it to the first content load (openUrl/setHTML). That
     * lets the private-browsing marker on the first openUrl decide the session type (ephemeral)
     * BEFORE the session is created. The buffers are attached; the WebView is built on first nav.
     * EXCEPTION (BPWPE_PRESPAWN): pre-warm a non-private WebView with about:blank now so the ~900ms
     * WebProcess spawn is hidden before the user's URL. openUrl tears it down if the card is private. */
    if (getenv("BPWPE_PRESPAWN") && !m_webView && m_virtualWindowHeight > 0) {
        WLOG("pre-spawn: warming WebProcess with about:blank");
        ensureWebView();
        if (m_webView) { webkit_web_view_load_uri(m_webView, "about:blank"); m_prewarmBlank = true; }
    }
    return true;
}

/* ---- THE SEAM: a rendered ARGB frame arrived (UIProcess glib loop) -------------------------- *
 * Non-blocking double buffer: pick an offscreen WE own, copy, msgPainted (hands it to the
 * adapter), flip. If neither is free (adapter holds both) drop the frame — the WPE backend's
 * ack-based flow control already throttles the WebProcess, and bufferReturned() frees them. */
void BrowserPageWPE::onFrame(void* ud, const uint8_t* argb, uint32_t w, uint32_t h, uint32_t stride)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    /* live-page guard: a stale conn/backend can deliver a frame after this page was freed (the
     * WebView wasn't finalized synchronously, so view_destroy never ran). The set lookup compares
     * the pointer value only — safe even if self is dangling (it never dereferences self). */
    if (g_livePages.find(self) == g_livePages.end()) return;
    int buf = self->m_ownOffscreen0 ? 0 : (self->m_ownOffscreen1 ? 1 : -1);
    static unsigned s_fc = 0;                             // gate per-frame logging (file I/O per paint = slow)
    if ((s_fc++ % 120) == 0)
        WLOG("onFrame #%u w=%u h=%u buf=%d own=%d,%d", s_fc, w, h, buf, self->m_ownOffscreen0, self->m_ownOffscreen1);
    if (buf < 0) { WLOG("onFrame DROP both-held own=0,0 w=%u h=%u", w, h); return; }   // both held by adapter — drop
    BrowserOffscreenQt* off = (buf == 0) ? self->m_offscreen0 : self->m_offscreen1;
    /* The backend hands us the ACTUAL frame size: the fit-to-screen SCALED size (m_renderWidth/Height)
     * when downscaling, else the layout size. Validate against that — the frame is smaller than the
     * layout when scaling, so the old exact-vs-virtual check would drop every frame. */
    if (!off || w != (uint32_t)self->m_renderWidth || h != (uint32_t)self->m_renderHeight) {
        WLOG("onFrame DROP off=%p sizecheck %ux%u vs render %dx%d (virt %dx%d)", (void*)off, w, h,
             self->m_renderWidth, self->m_renderHeight, self->m_virtualWindowWidth, self->m_virtualWindowHeight);
        return;
    }

    unsigned char* dst = off->rasterBuffer();             // ARGB32-premultiplied, top-left origin
    const uint32_t dstStride = w * 4;
    /* WebKit 2.38 composites top-down (2.34 was bottom-up) — no vertical flip (verified on-device:
     * the h-1-y flip rendered upside-down under 2.38). Straight row copy. The backend already packed
     * the scaled frame tightly (stride == w*4). */
    for (uint32_t y = 0; y < h; ++y)
        memcpy(dst + (size_t)y * dstStride, argb + (size_t)y * stride, dstStride);

    /* Set the offscreen header per the legacy BrowserOffscreenInfo contract: the buffer NOW genuinely
     * holds the page rendered at contentZoom (the backend downscaled it), so bufferWidth == renderedWidth
     * == the scaled width, and contentZoom = scaledWidth / layoutWidth (= screenW/W, e.g. 0.8). The
     * adapter divides on-screen taps by this to recover document coords — which equal our layout space,
     * so dispatchPointer needs NO further scaling. (contentZoom == 1.0 when not downscaling.) */
    BrowserOffscreenInfo* hdr = off->header();
    if (hdr) {
        hdr->bufferWidth = (int)w; hdr->bufferHeight = (int)h;
        hdr->contentZoom = (self->m_virtualWindowWidth > 0)
                           ? (double)w / (double)self->m_virtualWindowWidth : 1.0;
        hdr->renderedX = 0; hdr->renderedY = self->m_renderedY;   /* pan model: buffer's content-top */
        hdr->renderedWidth = (int)w; hdr->renderedHeight = (int)h;
    }

    if (getenv("BPWPE_DUMPFRAME")) {   /* dump the offscreen the adapter will show → diagnose the 3× repeat */
        FILE* fp = fopen("/tmp/isis_frame.bgra", "wb");
        if (fp) { fprintf(fp, "ISIS %u %u %u\n", w, h, dstStride);
                  fwrite(dst, 1, (size_t)h * dstStride, fp); fclose(fp); }
        WLOG("dumped offscreen %ux%u stride=%u renderedY=%d", w, h, dstStride, self->m_renderedY);
    }

    self->flushBuffer(buf);

    /* Keep the scroll range fresh while a long page loads: re-query the page height (throttled) so the
     * adapter's scroll clamp grows to the real bottom BEFORE LOAD_FINISHED (slow on heavy sites). */
    {
        static struct timeval s_lastCH = {0, 0};
        struct timeval now; gettimeofday(&now, NULL);
        long dtms = (now.tv_sec - s_lastCH.tv_sec) * 1000 + (now.tv_usec - s_lastCH.tv_usec) / 1000;
        if (dtms > 800 && self->m_webView && webkit_web_view_is_loading(self->m_webView)) {
            s_lastCH = now;
            self->updateContentsSize();
        }
    }
}

/* hands the offscreen to the adapter; non-blocking (no sem_wait — would deadlock the glib loop) */
void BrowserPageWPE::flushBuffer(int buffer)
{
    if (!m_offscreen0 || !m_offscreen1) return;
    if (buffer == 0) m_ownOffscreen0 = false; else m_ownOffscreen1 = false;   // adapter owns it now
    int key = buffer == 0 ? m_offscreen0->key() : m_offscreen1->key();
    m_server->msgPainted(m_proxy, key);
}

void BrowserPageWPE::bufferReturned(int32_t key)
{
    if (m_offscreen0 && m_offscreen0->key() == key)      m_ownOffscreen0 = true;
    else if (m_offscreen1 && m_offscreen1->key() == key) m_ownOffscreen1 = true;
    WLOG("bufferReturned key=%d -> own now %d,%d", key, m_ownOffscreen0, m_ownOffscreen1);
}

/* ---- navigation / content ------------------------------------------------------------------ */
void BrowserPageWPE::openUrl(const char* url)
{
    WLOG("openUrl %s", url ? url : "(null)");
    std::string u = url ? url : "";
    /* Private-browsing marker — the only reliable per-card signal (the adapter does NOT forward
     * setIdentifier to the BS). The app opens a private card with "isis-private:" + the real target,
     * BUT the app prepends http:// to unknown schemes, so it can arrive as "isis-private:X" OR
     * "http://isis-private:X". Strip an optional leading http:// before matching. Detect it BEFORE
     * ensureWebView so the session is created ephemeral, then strip the marker and load the target. */
    {
        std::string c = (u.compare(0, 7, "http://") == 0) ? u.substr(7) : u;
        if (c.compare(0, 13, "isis-private:") == 0) {
            m_private = true;
            u = c.substr(13);
            if (u.empty() || u == "about:blank") u = "about:blank";
            WLOG("private marker -> ephemeral session; target='%s'", u.c_str());
        }
    }
    /* A pre-warmed (about:blank, non-private) WebView can't serve a private card — tear it down so
     * ensureWebView() rebuilds it ephemeral. Non-private navigations reuse the warm WebProcess. */
    if (m_prewarmBlank) {
        m_prewarmBlank = false;
        if (m_private && m_webView) { g_object_unref(m_webView); m_webView = nullptr; }
    }
    ensureWebView();
    /* WebKit needs a scheme or it returns WebKitPolicyError 101 "URL can't be shown".
     * The adapter passes the raw typed text (e.g. "cnn.com"), so add http:// if missing. */
    if (!u.empty() && u.find("://") == std::string::npos
        && u.compare(0, 5, "data:") != 0 && u.compare(0, 6, "about:") != 0
        && u.compare(0, 5, "file:") != 0 && u.compare(0, 7, "mailto:") != 0)
        u = "http://" + u;
    webkit_web_view_load_uri(m_webView, u.c_str());
}
void BrowserPageWPE::setHTML(const char* url, const char* body) { ensureWebView(); webkit_web_view_load_html(m_webView, body, url); }
void BrowserPageWPE::pageBackward()             { if (m_webView) webkit_web_view_go_back(m_webView); }
void BrowserPageWPE::pageForward()              { if (m_webView) webkit_web_view_go_forward(m_webView); }
void BrowserPageWPE::pageReload()               { if (m_webView) webkit_web_view_reload(m_webView); }
void BrowserPageWPE::pageStop()                 { if (m_webView) webkit_web_view_stop_loading(m_webView); }
bool BrowserPageWPE::canGoBackward() const      { return m_webView && webkit_web_view_can_go_back(m_webView); }
bool BrowserPageWPE::canGoForward() const       { return m_webView && webkit_web_view_can_go_forward(m_webView); }
void BrowserPageWPE::clearHistory()             { /* TODO: WebKit2/WPE has no back-forward-list clear API; clear via the WebsiteDataManager (webkit_website_data_manager_clear, WEBKIT_WEBSITE_DATA_*) or recreate the view. */ }

/* ---- sizing -------------------------------------------------------------------------------- */
void BrowserPageWPE::setWindowSize(uint32_t w, uint32_t h)
{
    WLOG("setWindowSize %ux%u (was win=%dx%d virt=%dx%d webView=%p)", w, h, m_windowWidth, m_windowHeight, m_virtualWindowWidth, m_virtualWindowHeight, (void*)m_webView);
    m_windowWidth = (int)w; m_windowHeight = (int)h;
    if (m_viewBackend) wpe_isis_view_backend_resize(m_viewBackend, w, h);
}
void BrowserPageWPE::setVirtualWindowSize(uint32_t w, uint32_t h)
{
    WLOG("setVirtualWindowSize %ux%u (was virt=%dx%d)", w, h, m_virtualWindowWidth, m_virtualWindowHeight);
    m_virtualWindowWidth = (int)w; m_virtualWindowHeight = (int)h;
    /* TODO: drive the CSS viewport width via device-scale / a viewport policy so pages lay out at
     * the virtual width (the 602 used the QtWebKit viewport meta path). */
}
void BrowserPageWPE::getWindowSize(int& w, int& h)        { w = m_windowWidth; h = m_windowHeight; }
void BrowserPageWPE::getVirtualWindowSize(int& w, int& h) { w = m_virtualWindowWidth; h = m_virtualWindowHeight; }
void BrowserPageWPE::setScrollPosition(int cx, int cy, int /*cw*/, int /*ch*/)
{
    if (!m_webView) return;
    /* The adapter sends scroll in SCREEN (scaled) space; convert to DOCUMENT space by dividing by
     * contentZoom — matching the legacy BrowserPage::setScrollPosition (`cx = cx / m_zoomLevel`). Taps
     * arrive in document space, so m_scrollX must be document space too. No-op at 1:1 (contentZoom==1). */
    double cz = contentZoom();
    if (cz > 0.0 && cz != 1.0) { cx = (int)(cx / cz); cy = (int)(cy / cz); }
    m_scrollX = cx; m_scrollY = cy;
    static int s_noPan = -1;
    if (s_noPan < 0) s_noPan = getenv("BPWPE_NO_PAN") ? 1 : 0;
    if (s_noPan) {   /* re-render-every-step path (safety net). renderedY = scroll so the adapter's pan
                        offset (mScrollPos.y - renderedY) cancels to the identity blit — behavior unchanged. */
        m_renderedY = cy;
        char js[96]; snprintf(js, sizeof(js), "window.scrollTo(%d,%d)", cx, cy);
        webkit_web_view_evaluate_javascript(m_webView, js, -1, nullptr, nullptr, nullptr, nullptr, nullptr);
        return;
    }
    /* PAN MODEL (matches the legacy Palm browser, smooth on this same HW+adapter): the offscreen holds
     * a TALL rendered region [m_renderedY, m_renderedY+m_renderHeight]; the adapter PANS within it
     * (translate -mScrollPos) with NO readback. window.scrollTo re-renders + does the ~500ms glReadPixels
     * EVERY step — the whole slowness. So only re-render when the scroll nears the buffer edge; otherwise
     * let the adapter pan the buffer we already sent. Re-render is centred so panning works both ways. */
    if (m_screenHeight <= 0) {   /* resolution unknown → safe re-render path, never a hardcoded size */
        m_renderedY = cy;        /* keep renderedY == scroll so the adapter's pan offset cancels */
        char js[96]; snprintf(js, sizeof(js), "window.scrollTo(%d,%d)", cx, cy);
        webkit_web_view_evaluate_javascript(m_webView, js, -1, nullptr, nullptr, nullptr, nullptr, nullptr);
        return;
    }
    int screenH = m_screenHeight;
    int slack   = m_renderHeight - screenH;                 /* pannable travel inside the buffer */
    if (slack < 0) slack = 0;
    int guard   = screenH / 3;                              /* re-render within ~1/3 screen of an edge */
    bool inBuffer = (slack > 2 * guard) && (cy >= m_renderedY + guard) && (cy <= m_renderedY + slack - guard);
    WLOG("setScrollPosition %d,%d renderedY=%d slack=%d inBuffer=%d", cx, cy, m_renderedY, slack, inBuffer);
    if (inBuffer) return;                                   /* adapter pans — no re-render, no readback */
    int newTop = cy - slack / 2;
    if (newTop < 0) newTop = 0;
    m_renderedY = newTop;
    char js[96];
    snprintf(js, sizeof(js), "window.scrollTo(%d,%d)", cx, newTop);
    webkit_web_view_evaluate_javascript(m_webView, js, -1, nullptr, nullptr, nullptr, nullptr, nullptr);
}

/* async JS: report the real page height so the adapter knows the scroll range */
void BrowserPageWPE::onContentHeight(GObject* obj, GAsyncResult* res, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    GError* err = nullptr;
    JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (!v) { if (err) g_error_free(err); return; }
    int h = (int)jsc_value_to_double(v);
    g_object_unref(v);   /* 2.0: _finish returns an owned JSCValue (replaces WebKitJavascriptResult) */
    WLOG("content height = %d (virt %d)", h, self->m_virtualWindowHeight);
    /* Ignore the exact-viewport-height artifact: document.scrollHeight transiently returns the WebView
     * viewport height (== m_virtualWindowHeight) during reflow / subframe loads. Reporting it clobbers the
     * real page height and clamps the adapter's scroll to one buffer. Real heights are never exactly it. */
    if (h > 0 && h != self->m_virtualWindowHeight)
        self->m_server->msgContentsSizeChanged(self->m_proxy, self->m_virtualWindowWidth, h);
}
/* On-demand: extract the article from the CURRENT DOM (works mid-load — the article text is present
 * long before the page "finishes" loading ads/trackers). Compact (<16KB yap limit). */
void BrowserPageWPE::extractReaderContent()
{
    if (!m_webView) return;
    webkit_web_view_evaluate_javascript(m_webView,
        "(function(){try{function T(e){return e?(e.innerText||e.textContent||'').trim():'';}"
        "var t=T(document.querySelector('h1'))||document.title||'';"
        "var c=document.querySelector('article,[itemprop=articleBody],[role=main],main');"
        "if(!c){var dv=document.querySelectorAll('div,section'),bl=0,bi=null;"
        "for(var d=0;d<dv.length;d++){var ps=dv[d].querySelectorAll('p'),L=0;for(var q=0;q<ps.length;q++)L+=T(ps[q]).length;if(L>bl){bl=L;bi=dv[d];}}c=bi||document.body;}"
        "var KW=['navbar','navigation','menu','footer','sidebar','comment','reactie','related','promo','advert','share','social','breadcrumb','cookie','consent','newsletter','widget','recommend'];"
        "function B(el){for(var n=el;n&&n!==c;n=n.parentElement){var tg=n.tagName;if(tg==='NAV'||tg==='ASIDE'||tg==='FOOTER'||tg==='HEADER')return true;var z=((n.id||'')+' '+(typeof n.className==='string'?n.className:'')).toLowerCase();for(var j=0;j<KW.length;j++){if(z.indexOf(KW[j])>=0)return true;}}return false;}"
        "var ns=c.querySelectorAll('p,h2,h3,h4,li,blockquote'),o=[],s=0;"
        "for(var i=0;i<ns.length&&s<14000;i++){var el=ns[i];var x=T(el);var g=el.tagName.toLowerCase();"
        "if(!x)continue;if((g==='p'||g==='li')&&x.length<40)continue;if(B(el))continue;"
        "o.push(g.charAt(0)==='h'?('<h3>'+x+'</h3>'):('<p>'+x+'</p>'));s+=x.length;}"
        "return JSON.stringify({title:t,html:o.join('')});}"
        "catch(e){return JSON.stringify({title:'',html:''});}})()",
        -1, nullptr, nullptr, nullptr, &BrowserPageWPE::onReaderProbe, this);
}
/* Extraction result → msgActionData("readerContent") → adapter "actionData" event → app cache → reader. */
void BrowserPageWPE::onReaderProbe(GObject* obj, GAsyncResult* res, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    GError* err = nullptr;
    JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (!v) { if (err) g_error_free(err); return; }
    char* s = jsc_value_to_string(v);
    size_t n = s ? strlen(s) : 0;
    WLOG("reader: extracted loaded-page article (%zu bytes) -> msgActionData(readerContent)", n);
    if (self->m_server && s && n > 2) self->m_server->msgActionData(self->m_proxy, "readerContent", s);
    if (s) g_free(s);
    g_object_unref(v);
}
void BrowserPageWPE::updateContentsSize()
{
    if (!m_webView) return;
    webkit_web_view_evaluate_javascript(m_webView,
        "Math.max(document.documentElement.scrollHeight, document.body?document.body.scrollHeight:0)",
        -1, nullptr, nullptr, nullptr, &BrowserPageWPE::onContentHeight, this);
}
void BrowserPageWPE::setZoomAndScroll(double zoom, int, int)
{
    if (!m_webView) return;
    /* The adapter's zoom is the CONTENT zoom (on-screen px per document px). Our fixed fit-to-screen
     * downscale already provides the baseline contentZoom (screenW/W) via the backend + header, so at
     * the fit baseline WebKit must stay at layout scale 1.0 — NOT be zoomed to 0.8, which would shrink
     * the page a second time. Map the adapter zoom relative to the fit baseline; clamp so we never zoom
     * WebKit below layout scale (the downscale handles the shrink). At 1:1 this is a pass-through, so
     * the previous behaviour is preserved. (Pinch-zoom crispness beyond fit is out of scope here.) */
    double fit = contentZoom();
    double wk = (fit > 0.0) ? zoom / fit : zoom;
    if (wk < 1.0) wk = 1.0;
    WLOG("setZoomAndScroll adapterZoom=%.3f fit=%.3f -> webkitZoom=%.3f", zoom, fit, wk);
    webkit_web_view_set_zoom_level(m_webView, wk);
}

/* WebKit told us an editable element gained/lost focus (via the IM context). Map the field purpose
 * to the Isis field type and signal the adapter, which raises/hides the webOS virtual keyboard
 * (msgEditorFocused — the legacy QtWebKit path that was never wired in the WPE port). */
void BrowserPageWPE::onEditorFocus(bool focused, int purpose)
{
    int fieldType = BATypes::FieldType_Text;
    switch (purpose) {
        case WEBKIT_INPUT_PURPOSE_PASSWORD: fieldType = BATypes::FieldType_Password; break;
        case WEBKIT_INPUT_PURPOSE_EMAIL:    fieldType = BATypes::FieldType_Email;    break;
        case WEBKIT_INPUT_PURPOSE_NUMBER:
        case WEBKIT_INPUT_PURPOSE_DIGITS:   fieldType = BATypes::FieldType_Number;   break;
        case WEBKIT_INPUT_PURPOSE_PHONE:    fieldType = BATypes::FieldType_Phone;    break;
        case WEBKIT_INPUT_PURPOSE_URL:      fieldType = BATypes::FieldType_URL;      break;
        default:                            fieldType = BATypes::FieldType_Text;     break;
    }
    WLOG("editorFocus focused=%d purpose=%d fieldType=%d", focused, purpose, fieldType);
    if (m_server) m_server->msgEditorFocused(m_proxy, focused, fieldType, 0);
}

/* After a tap, poll document.activeElement to see whether an editable element is now focused, and
 * raise/hide the VKB. Uses evaluate_javascript (proven-safe — same path as the page-height query).
 * The JS returns -1 (nothing editable) or a WebKitInputPurpose-style code (0=text … 6=password). */
void BrowserPageWPE::checkEditorFocus()
{
    if (!m_webView) return;
    webkit_web_view_evaluate_javascript(m_webView,
        "(function(){var e=document.activeElement;if(!e)return -1;var g=e.tagName;"
        "if(g==='TEXTAREA'||e.isContentEditable)return 0;if(g!=='INPUT')return -1;"
        "var t=(e.type||'text').toLowerCase();"
        "var m={password:6,email:5,number:2,range:2,tel:3,url:4};return (t in m)?m[t]:0;})()",
        -1, nullptr, nullptr, nullptr, &BrowserPageWPE::onEditorCheckResult, this);
}
void BrowserPageWPE::onEditorCheckResult(GObject* obj, GAsyncResult* res, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    GError* err = nullptr;
    JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (!v) { if (err) g_error_free(err); return; }
    int code = (int)jsc_value_to_int32(v);
    g_object_unref(v);
    self->onEditorFocus(code >= 0, code >= 0 ? code : 0);
}

/* Double-tap zoom: find the block-level element under the tap and report its rect so the adapter can
 * zoom to fit it (msgSmartZoomCalculateResponseSimple — the legacy double-tap path). Async via
 * evaluate_javascript; coords map content↔viewport through the tracked scroll (m_scrollX/Y). */
void BrowserPageWPE::smartZoomCalculate(uint32_t pointX, uint32_t pointY)
{
    if (!m_webView) return;
    m_smartZoomX = (int)pointX; m_smartZoomY = (int)pointY;
    int vx = (int)pointX - m_scrollX, vy = (int)pointY - m_scrollY;   /* content → viewport for elementFromPoint */
    char js[512];
    snprintf(js, sizeof(js),
        "(function(){var e=document.elementFromPoint(%d,%d);if(!e)return '0,0,0,0';"
        "while(e.parentElement&&getComputedStyle(e).display==='inline')e=e.parentElement;"
        "var r=e.getBoundingClientRect(),sx=window.scrollX,sy=window.scrollY;"
        "return Math.round(r.left+sx)+','+Math.round(r.top+sy)+','+Math.round(r.right+sx)+','+Math.round(r.bottom+sy);})()",
        vx, vy);
    webkit_web_view_evaluate_javascript(m_webView, js, -1, nullptr, nullptr, nullptr,
                                        &BrowserPageWPE::onSmartZoomResult, this);
}
void BrowserPageWPE::onSmartZoomResult(GObject* obj, GAsyncResult* res, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    GError* err = nullptr;
    JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (!v) { if (err) g_error_free(err); return; }
    char* s = jsc_value_to_string(v);
    int l = 0, t = 0, r = 0, b = 0;
    if (s) sscanf(s, "%d,%d,%d,%d", &l, &t, &r, &b);
    g_free(s); g_object_unref(v);
    if (self->m_server)
        self->m_server->msgSmartZoomCalculateResponseSimple(self->m_proxy, self->m_smartZoomX, self->m_smartZoomY, l, t, r, b, 0);
}

/* ---- input: dispatch on the wpe_view_backend (forwards to the WebProcess) ------------------- */
void BrowserPageWPE::dispatchPointer(int x, int y, uint32_t button, bool down)
{
    WLOG("dispatchPointer %d,%d btn=%u down=%d", x, y, button, down);
    if (!m_viewBackend) return;
    /* NO tap scaling here. The offscreen now satisfies the legacy contract (buffer holds the page
     * already scaled by contentZoom), so the adapter has ALREADY divided the on-screen tap by
     * contentZoom before sending it — the coords arrive in DOCUMENT space, which equals our layout
     * space (m_virtualWindowWidth). WebKit's viewport is laid out at that same width, so we dispatch
     * as-is. (The earlier x*virtualW/screenW rescale double-transformed taps → they landed too low.)
     * The adapter hands document-ABSOLUTE coords; WebKit wants viewport-relative — subtract scroll. */
    x -= m_scrollX; y -= m_scrollY;
    WLOG("  -> viewport %d,%d (scroll %d,%d)", x, y, m_scrollX, m_scrollY);
    struct wpe_input_pointer_event move = { wpe_input_pointer_event_type_motion, 0, x, y, 0, 0, 0 };
    wpe_view_backend_dispatch_pointer_event(m_viewBackend, &move);
    if (button) {
        struct wpe_input_pointer_event btn = { wpe_input_pointer_event_type_button, 0, x, y,
                                               button, (uint32_t)(down ? 1 : 0), 0 };
        wpe_view_backend_dispatch_pointer_event(m_viewBackend, &btn);
    }
}

void BrowserPageWPE::mouseEvent(int type, int contentX, int contentY, int detail)
{
    /* type: 0=move 1=down 2=up (Isis BATypes). detail = button. TODO: map content→view coords
     * (subtract scroll, apply zoom) once setScrollPosition is wired. */
    const uint32_t BTN_LEFT = 1;
    switch (type) {
        case 0: dispatchPointer(contentX, contentY, 0, false); break;
        case 1: dispatchPointer(contentX, contentY, detail ? (uint32_t)detail : BTN_LEFT, true); break;
        case 2: dispatchPointer(contentX, contentY, detail ? (uint32_t)detail : BTN_LEFT, false);
                checkEditorFocus(); break;   // tap-up may have focused an editable → raise/hide the VKB
    }
}

bool BrowserPageWPE::clickAt(uint32_t x, uint32_t y, uint32_t numClicks)
{
    for (uint32_t i = 0; i < (numClicks ? numClicks : 1); ++i) {
        dispatchPointer((int)x, (int)y, 1, true);
        dispatchPointer((int)x, (int)y, 1, false);
    }
    checkEditorFocus();   // tap may have focused an editable element → raise/hide the VKB
    return true;
}
bool BrowserPageWPE::holdAt(uint32_t x, uint32_t y) { dispatchPointer((int)x, (int)y, 1, true); return true; }

/* The webOS gesture recogniser delivers single-finger TAPS as gesture events (not mouse/click/
 * touch) — this was a no-op stub, so links, buttons and cookie-consent "Accept" never got a click.
 * Pinch-zoom/rotate carry scale!=1 / rotation!=0; ignore those. Scrolling comes via
 * setScrollPosition, not here. type is logged so the start/end phases can be refined if needed. */
void BrowserPageWPE::gestureEvent(int type, int contentX, int contentY, double scale, double rotation,
                                  int /*centerX*/, int /*centerY*/)
{
    WLOG("gestureEvent type=%d x=%d y=%d scale=%.3f rot=%.3f", type, contentX, contentY, scale, rotation);
    if (m_viewBackend && scale == 1.0 && rotation == 0.0) {
        dispatchPointer(contentX, contentY, 1, true);
        dispatchPointer(contentX, contentY, 1, false);
    }
}

/* The Isis adapter delivers taps as touch events (not mouseEvent/clickAt) — this was a no-op stub,
 * so taps (e.g. the cookie-consent "Accept") never reached the page. Map touches to pointer events
 * so the page gets a real click. */
void BrowserPageWPE::touchEvent(int type, int32_t touchCount, int32_t /*modifiers*/, const char* touchesJson)
{
    WLOG("touchEvent type=%d count=%d json=%s", type, touchCount, touchesJson ? touchesJson : "(null)");
    if (!m_viewBackend || !touchesJson) return;
    /* best-effort: pull the first touch point's coords out of the JSON */
    int x = -1, y = -1;
    const char* px = strstr(touchesJson, "\"x\"");  if (px && (px = strchr(px, ':'))) x = atoi(px + 1);
    const char* py = strstr(touchesJson, "\"y\"");  if (py && (py = strchr(py, ':'))) y = atoi(py + 1);
    if (x < 0 || y < 0) return;
    /* webOS touch phase (type): 0=start 1=move 2=end (logged to confirm). Drive a pointer: down on
     * start, motion on move, up on end — a tap becomes down+up = a click. */
    switch (type) {
        case 0: dispatchPointer(x, y, 1, true);  break;
        case 1: dispatchPointer(x, y, 0, false); break;
        case 2: dispatchPointer(x, y, 1, false); break;
        default: dispatchPointer(x, y, 1, true); dispatchPointer(x, y, 1, false); break;
    }
}

void BrowserPageWPE::keyDown(int32_t key, int32_t modifiers, int32_t /*chr*/)
{
    if (!m_viewBackend) return;
    /* TODO: map the Isis key code → xkb keysym (wpe/keysyms.h). Passing `key` through for now. */
    struct wpe_input_keyboard_event ev = { 0, (uint32_t)key, 0, true, (uint32_t)modifiers };
    wpe_view_backend_dispatch_keyboard_event(m_viewBackend, &ev);
}
void BrowserPageWPE::keyUp(int32_t key, int32_t modifiers, int32_t /*chr*/)
{
    if (!m_viewBackend) return;
    struct wpe_input_keyboard_event ev = { 0, (uint32_t)key, 0, false, (uint32_t)modifiers };
    wpe_view_backend_dispatch_keyboard_event(m_viewBackend, &ev);
}

void BrowserPageWPE::setFocus(bool enable)
{
    m_focused = enable;
    if (m_viewBackend) wpe_isis_view_backend_set_visible(m_viewBackend, enable);
}

/* ---- freeze / thaw (purge offscreens on backgrounding; re-attach on return) ----------------- */
bool BrowserPageWPE::freeze()
{
    m_frozen = true;
    delete m_offscreen0; m_offscreen0 = 0; m_ownOffscreen0 = false;
    delete m_offscreen1; m_offscreen1 = 0; m_ownOffscreen1 = false;
    if (m_viewBackend) wpe_isis_view_backend_set_visible(m_viewBackend, false);
    return true;
}
bool BrowserPageWPE::thaw(int key1, int key2, int size)
{
    m_frozen = false;
    bool ok = attachToBuffer(m_virtualWindowWidth, m_virtualWindowHeight, key1, key2, size);
    if (m_viewBackend) wpe_isis_view_backend_set_visible(m_viewBackend, true);
    return ok;
}

void BrowserPageWPE::setIdentifier(const char* id) {
    free(m_identifier); m_identifier = id ? strdup(id) : 0;
    WLOG("setIdentifier '%s' private=%d webViewExists=%d", id?id:"(null)",
         (id && strstr(id,"private")) ? 1 : 0, m_webView ? 1 : 0);
}
void BrowserPageWPE::setUserAgent(const char* ua)
{
    /* Store but DON'T apply the adapter's legacy UA — ensureWebView() forces a modern WebKit UA. */
    m_userAgent = ua ? ua : "";
}

/* ---- WPE WebKit signals → Isis adapter notifications --------------------------------------- *
 * These map onto the existing BrowserServerBase msg* notifications the 602 doLoad / titleChanged
 * slots used. Exact method names per BrowserServerBase.h. */
void BrowserPageWPE::onLoadChanged(WebKitWebView* v, WebKitLoadEvent ev, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    WLOG("loadChanged ev=%d (0=start 1=redir 2=commit 3=finish) uri=%s", ev, webkit_web_view_get_uri(v) ? webkit_web_view_get_uri(v) : "?");
    switch (ev) {
        case WEBKIT_LOAD_STARTED:
            self->m_server->msgLoadStarted(self->m_proxy);
            break;
        case WEBKIT_LOAD_COMMITTED:
            /* tell the adapter the content size so it sets up the display area + shows painted buffers */
            self->m_server->msgContentsSizeChanged(self->m_proxy, self->m_virtualWindowWidth, self->m_virtualWindowHeight);
            break;
        case WEBKIT_LOAD_FINISHED: {
            const char* t = webkit_web_view_get_title(v);
            const char* cu = webkit_web_view_get_uri(v);
            /* title is reliably set by load-finished — push it so history isn't "Untitled" */
            self->m_server->msgTitleAndUrlChanged(self->m_proxy, (t && *t) ? t : (cu ? cu : ""),
                                                  cu ? cu : "", self->canGoBackward(), self->canGoForward());
            self->updateContentsSize();   /* report the REAL page height for scrolling */
            self->m_server->msgDidFinishDocumentLoad(self->m_proxy);
            self->m_server->msgLoadStopped(self->m_proxy);
            break;
        }
        default: break;
    }
    (void)v;
}
void BrowserPageWPE::onLoadFailed(WebKitWebView*, WebKitLoadEvent, const char* uri, GError* err, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    WLOG("load failed %s: domain=%s code=%d msg=%s", uri,
         err ? g_quark_to_string(err->domain) : "?", err ? err->code : 0, err ? err->message : "?");
    /* self->m_server->msgReportError(self->m_proxy, uri, err?err->code:0, err?err->message:""); */
    (void)self;
}
void BrowserPageWPE::onTitleChanged(GObject*, GParamSpec*, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    const char* title = webkit_web_view_get_title(self->m_webView);
    const char* uri   = webkit_web_view_get_uri(self->m_webView);
    WLOG("titleChanged: %s", title ? title : "(null)");
    self->m_server->msgTitleAndUrlChanged(self->m_proxy, title ? title : "", uri ? uri : "",
                                          self->canGoBackward(), self->canGoForward());
}
void BrowserPageWPE::onUriChanged(GObject*, GParamSpec*, gpointer ud)        { onTitleChanged(nullptr, nullptr, ud); }
void BrowserPageWPE::onProgressChanged(GObject*, GParamSpec*, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    double p = webkit_web_view_get_estimated_load_progress(self->m_webView);
    self->m_server->msgLoadProgress(self->m_proxy, (int)(p * 100));
}
