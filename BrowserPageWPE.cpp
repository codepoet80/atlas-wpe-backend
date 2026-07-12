/*
 * BrowserPageWPE.cpp — WPE WebKit BrowserPage for the Atlas BrowserServer. See BrowserPageWPE.h.
 * Real browse path; long-tail commands stubbed with TODO. Uses the WPE WebKit C API + the custom
 * libwpe backend (wpe_atlas_view_backend_create). Input is dispatched on the wpe_view_backend (it
 * forwards to the WebProcess); rendering returns via onFrame() → Atlas offscreen → msgPainted.
 */
#include "BrowserPageWPE.h"
#include "BrowserServer.h"
#include "BrowserOffscreenQt.h"
#include "YapProxy.h"
#include "BrowserSyncReplyPipe.h"   // Feature 7: sync round-trip to the app for SSL/auth dialogs
#include "wpe-atlas-backend.h"

#include <sys/statvfs.h>            // Feature 6: /dev/shm size in the atlas:about diagnostics page
#include <sys/stat.h>               // Feature 3: stat() the downloaded favicon file in renderToFile

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <sys/time.h>
#include <glib-unix.h>   // g_unix_signal_add — SIGUSR1 kicks the autonomous scroll test

/* Log to a file (webOS syslog goes to PmLog, hard to read on-device). Timestamped (ms) so we can
 * measure load phases. */
static inline long _wlog_ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return (tv.tv_sec % 10000) * 1000 + tv.tv_usec / 1000; }
/* Off unless BPWPE_DEBUG is set (checked once) — keeps the per-frame/per-event file I/O out of
 * production while staying available on-device by setting the env var in the launch wrapper. */
static inline bool _wlog_on(void) { static int on = -1; if (on < 0) on = getenv("BPWPE_DEBUG") ? 1 : 0; return on; }
#define WLOG(fmt, ...) do { if (_wlog_on()) { FILE* _f = fopen("/tmp/bpwpe.log", "a"); \
    if (_f) { fprintf(_f, "[%08ld] " fmt "\n", _wlog_ms(), ##__VA_ARGS__); fclose(_f); } } } while (0)
/* Always-on trace for the mediad handoff — routes to syslog (→ /var/log/messages, tagged BrowserServer-atlas)
 * so it is visible without BPWPE_DEBUG, plus the WLOG file when debugging. */
#include <syslog.h>
#define MLOG(fmt, ...) do { syslog(LOG_NOTICE, "MEDIAD " fmt, ##__VA_ARGS__); WLOG(fmt, ##__VA_ARGS__); } while (0)

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
    signal(SIGTRAP, bpwpe_crash_handler);   /* JIT/-mthumb WebContext crash is SIGTRAP (RELEASE_ASSERT/CRASH) */
    signal(SIGILL,  bpwpe_crash_handler);
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
struct AtlasIMContext      { WebKitInputMethodContext      parent_instance; BrowserPageWPE* page; };
struct AtlasIMContextClass { WebKitInputMethodContextClass parent_class; };
G_DEFINE_TYPE(AtlasIMContext, atlas_im_context, WEBKIT_TYPE_INPUT_METHOD_CONTEXT)
static void atlas_im_focus_in(WebKitInputMethodContext* c) {
    AtlasIMContext* s = reinterpret_cast<AtlasIMContext*>(c);
    /* Only raise the VKB for editor focus that follows a USER interaction. A page that AUTOFOCUSES an input
     * on load (search/login sites) would otherwise grab the keyboard before any tap — which then blocks the
     * app's own address bar from getting focus/VKB ("no focus after a site loads"). m_userInteracted resets
     * on each navigation and is set on the first pointer event, so tap-to-focus stays instant. */
    if (s->page && s->page->m_userInteracted) s->page->onEditorFocus(true, (int)webkit_input_method_context_get_input_purpose(c));
}
static void atlas_im_focus_out(WebKitInputMethodContext* c) {
    AtlasIMContext* s = reinterpret_cast<AtlasIMContext*>(c);
    if (s->page) s->page->onEditorFocus(false, 0);
}
static void atlas_im_context_class_init(AtlasIMContextClass* k) {
    WebKitInputMethodContextClass* c = WEBKIT_INPUT_METHOD_CONTEXT_CLASS(k);
    c->notify_focus_in  = atlas_im_focus_in;
    c->notify_focus_out = atlas_im_focus_out;
}
static void atlas_im_context_init(AtlasIMContext*) {}

#include <set>
/* Live-page guard: a destroyed BrowserPageWPE's backend/conn can still deliver a queued
 * MSG_FRAME_READY — the WebView isn't always finalized synchronously on unref, so view_destroy
 * hasn't run, the atlas_view lingers, and v->userdata still points at the freed page. onFrame checks
 * this set so a stale frame on a freed page is dropped instead of dereferencing freed memory.
 * Everything (ctor/dtor/onFrame) runs on the glib main loop, so no locking is needed. */
static std::set<BrowserPageWPE*> g_livePages;
static bool g_mediadBusy = false;   // only ONE mediad handoff at a time (single HW VIDC decoder)
/* True if the page has already been destroyed. Async JS-result callbacks and g_timeout callbacks capture a
 * raw BrowserPageWPE*; the WebView lingers and can deliver a pending probe/timer AFTER the card is freed, so
 * every such callback must check this before touching the page. (Comparing the pointer never derefs it.) */
static inline bool bpwpe_dead(BrowserPageWPE* p) { return g_livePages.find(p) == g_livePages.end(); }

/* ===== autonomous scroll test ===== kick with `kill -USR1 <bs-pid>`. Drives setScrollPosition in a
 * timed top->bottom->top sweep (exactly the adapter's scroll stream) and logs a SCROLLTEST summary:
 *   steps     = scroll events sent
 *   renders   = pan re-renders issued (readbacks)
 *   uncovered = steps where the viewport wasn't fully inside the DELIVERED buffer (== white/partial on screen)
 *   maxStall  = worst re-render latency (ms) — the readback wall
 *   finalDeliveredY≈0 means the buffer caught back up to the top after the sweep (no stuck/white). */
struct ScrollTestState { bool active; int cy, dir, step, maxCy, steps, renders, uncovered; long maxStallMs; guint tick; };
static ScrollTestState g_st = {false,0,1,120,0,0,0,0,0,0};
static BrowserPageWPE* g_testablePage = nullptr;   // last page to attach buffers = the scroll-test target
static gboolean on_sigusr1(gpointer);              // fwd
static gboolean scrolltest_poll(gpointer);         // fwd — file trigger (app-spawned BS masks SIGUSR1)

BrowserPageWPE::BrowserPageWPE(BrowserServer* server, YapProxy* proxy)
    : m_server(server), m_proxy(proxy), m_identifier(0)
    , m_offscreen0(0), m_offscreen1(0), m_ownOffscreen0(false), m_ownOffscreen1(false)
    , m_windowWidth(1024), m_windowHeight(768)
    , m_virtualWindowWidth(0), m_virtualWindowHeight(0)
    , m_screenWidth(0), m_screenHeight(0), m_renderedY(0), m_deliveredY(0)
    , m_renderWidth(0), m_renderHeight(0)
    , m_scrollX(0), m_scrollY(0), m_lastScrollMs(0), m_lastScrollY(0), m_scrollVel(0), m_dirStreak(0), m_pendingOldY(0), m_pageHeight(0)
    , m_navByHistory(false), m_focused(true), m_frozen(false), m_private(false), m_prewarmBlank(false), m_renderPending(false), m_renderPendingMs(0), m_settleTimer(0), m_lastRenderMs(0)
    , m_viewBackend(0), m_webView(0)
{
    g_livePages.insert(this);
    WLOG("ctor %p", this);
}

BrowserPageWPE::~BrowserPageWPE()
{
    g_livePages.erase(this);   // stop onFrame touching this page the moment we start tearing down
    if (m_settleTimer) { g_source_remove(m_settleTimer); m_settleTimer = 0; }   // settle-render timer must not fire on a freed page
    if (m_videoPollTimer) { g_source_remove(m_videoPollTimer); m_videoPollTimer = 0; }   // video-rect poll must not fire on a freed page
    if (m_webView) {
        /* Force-kill the WebProcess. Do NOT rely on WebView finalization to terminate it — the WebView
         * frequently lingers (stray refs; same reason the stale-frame UAF existed), so on card close the
         * WebProcess (tens of MB each) + backend + conns leak. They pile up until webOS OOMs with
         * "Too Many Cards". Terminating explicitly reclaims the WebProcess memory immediately. */
        webkit_web_view_terminate_web_process(m_webView);
        g_object_unref(m_webView);   // also destroys the backend + view backend
    }
    /* Resolve + release a still-held login-form decision so the deferred navigation isn't leaked with the
     * page (the failsafe timer is now guarded and won't touch a freed page). */
    if (m_loginDecision) {
        webkit_policy_decision_use(m_loginDecision);
        g_object_unref(m_loginDecision);
        m_loginDecision = nullptr;
    }
    free(m_identifier);
    delete m_offscreen0;
    delete m_offscreen1;
    delete m_syncReplyPipe;   // Feature 7 SSL/auth sync pipe (leaked the object + its FIFO/fd otherwise)
}

/* ---- create the WPE view bound to the Atlas frame sink (once size is known) ----------------- */
/* The page asked to open a new window/popup (window.open, target=_blank). Many cookie-consent
 * CMPs open the consent UI this way. Atlas has one WebView per card, so load the popup target in
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
    /* getUserMedia camera/mic: the TouchPad front camera is exposed as a GStreamer capture device by
     * libgstqcamsrc.so (backed by the qcamd HAL daemon). Grant so navigator.mediaDevices.getUserMedia works. */
    bool isUserMedia = WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(req);
    bool allow = isNotif || isUserMedia;
    WLOG("PERMISSION request type=%s -> %s", type ? type : "?", allow ? "allow" : "deny");
    if (allow) webkit_permission_request_allow(req);
    else       webkit_permission_request_deny(req);
    return TRUE;
}

/* Download handoff: WebKit can't DISPLAY this response (a file — .zip/.pdf/.apk/etc), so emit
 * msgDownloadStart. The adapter forwards it to the Atlas app (BrowserApp.js downloadResource),
 * which hands the URL to com.palm.downloadmanager — the webOS system download service. We do NOT
 * download in the BS; webOS owns the download queue/notifications/storage. */
/* Default UA: modern WebKit/Safari on webOS. Drop the hp-tablet/hpwOS tokens (Google shows
 * "Update your browser"); webOS/6.0 reads as a modern webOS engine. WebKit 620/Safari 18 = WPE 2.52. */
const char* const BrowserPageWPE::kAtlasDefaultUA =
    "Mozilla/5.0 (Linux; webOS/6.0; U; en-US) AppleWebKit/620.1.16 "
    "(KHTML, like Gecko) Version/18.0 Safari/620.1.16";
/* Teams-quirk UA: same real WebKit/Safari tokens, presented as macOS so Teams' allow-list accepts it. */
const char* const BrowserPageWPE::kAtlasMacSafariUA =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/620.1.16 "
    "(KHTML, like Gecko) Version/18.0 Safari/620.1.16";

/* Choose the UA for a navigation target and apply it if it differs from what's currently set. The
 * Microsoft Teams cluster (teams host + its login redirects) sniffs UAs and rejects "Linux"; give it
 * the macOS Safari UA. Everything else gets the default webOS UA. Only touches settings on a change. */
void BrowserPageWPE::applyUserAgentQuirk(const char* url)
{
    static const char* kMsTeamsHosts[] = {
        "teams.microsoft.com", "teams.live.com",
        "login.microsoftonline.com", "login.microsoft.com", "login.live.com",
    };
    bool wantMac = false;
    if (url) {
        for (size_t i = 0; i < G_N_ELEMENTS(kMsTeamsHosts); ++i)
            if (strstr(url, kMsTeamsHosts[i])) { wantMac = true; break; }
    }
    const char* want = wantMac ? kAtlasMacSafariUA : kAtlasDefaultUA;
    WebKitSettings* s = webkit_web_view_get_settings(m_webView);
    const char* cur = s ? webkit_settings_get_user_agent(s) : nullptr;
    if (s && (!cur || strcmp(cur, want) != 0)) {
        webkit_settings_set_user_agent(s, want);
        WLOG("UA quirk: %s -> %s", wantMac ? "MS-Teams" : "default", want);
    }
}

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
    else if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationPolicyDecision* nd = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        WebKitNavigationAction* act = webkit_navigation_policy_decision_get_navigation_action(nd);
        WebKitNavigationType nt = act ? webkit_navigation_action_get_navigation_type(act)
                                      : WEBKIT_NAVIGATION_TYPE_OTHER;
        WLOG("decidePolicy NAVIGATION type=%d", (int)nt);
        /* Per-site UA quirk: Microsoft Teams' client JS sniffs the UA and hard-redirects our
         * "webOS/6.0 Safari" to /v2/unsupported-browser. We ARE WebKit 620/Safari 18, so present a
         * macOS Safari UA for the Teams domain cluster (incl. its login redirects) — Teams accepts
         * Safari-on-Mac and the code paths honestly match our engine. Applied at navigation time so
         * the main document + subresources all use it; restored for any other host. */
        {
            BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
            WebKitURIRequest* rq = act ? webkit_navigation_action_get_request(act) : nullptr;
            const char* nurl = rq ? webkit_uri_request_get_uri(rq) : nullptr;
            if (self && self->m_webView && nurl)
                self->applyUserAgentQuirk(nurl);
        }
        /* Save-login trigger: real form POSTs report FORM_SUBMITTED/FORM_RESUBMITTED, but a page that
         * calls form.submit() from JS reports OTHER — cover all three. The snapshot JS runs on the main
         * frame and returns '' unless a password field is filled, so non-login navigations send nothing. */
        bool formish = (nt == WEBKIT_NAVIGATION_TYPE_FORM_SUBMITTED ||
                        nt == WEBKIT_NAVIGATION_TYPE_FORM_RESUBMITTED ||
                        nt == WEBKIT_NAVIGATION_TYPE_OTHER);
        /* Escape hatch: `/tmp/atlas_nologincap` disables the save-login deferral entirely. The deferral holds
         * a policy decision and runs capture JS against a document that may be mid-navigation; a page that does
         * a JS-driven navigation (WebKitNavigationType OTHER, e.g. html5test.co during its test run) can crash
         * the WebProcess on the held+resumed nav. This gate lets us load such pages undeferred (and is the
         * clean toggle for measuring html5test). Only real form POSTs are affected by save-login anyway. */
        if (act && formish && access("/tmp/atlas_nologincap", F_OK) == 0) {
            WLOG("saveLogin: capture disabled via /tmp/atlas_nologincap — nav proceeds undeferred");
        } else if (act && formish) {
            BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
            /* Save-login: snapshot the submitting form's username+password. evaluate_javascript is ASYNC,
             * but the form submit navigates immediately — if we let the nav proceed the old document (with
             * the filled fields) is gone before the snapshot runs and we capture nothing. Fix: HOLD the
             * policy decision (return TRUE, ref it) and resume the navigation from the capture callback, so
             * the JS runs against the still-live submitting document. TWO SAFETY NETS so this never stalls
             * normal browsing: (1) a suppression window — after one deferral we skip the next ~1.8s of
             * OTHER navs (the post-login redirect is ALSO type=OTHER and must not be held up); (2) a failsafe
             * timer that resumes the nav even if the eval callback never fires (e.g. page torn down mid-eval).
             * The snapshot JS returns '' when no password field is filled, so non-login navs send nothing.
             * (Script message handlers would be cleaner but crash this BS build.) */
            gint64 nowms = g_get_monotonic_time() / 1000;
            if (self && self->m_webView && !self->m_loginDecision &&
                (nowms - self->m_lastLoginDeferMs) > 1800) {
                static const char* kCapJs =
                    "(function(){var ps=document.querySelectorAll('input[type=password]');"
                    "for(var i=0;i<ps.length;i++){var p=ps[i];if(!p.value)continue;"
                    "var scope=p.form?p.form:document,ins=scope.querySelectorAll('input');"
                    "var before='',any='',seen=false;"
                    "for(var j=0;j<ins.length;j++){if(ins[j]===p){seen=true;continue;}"
                    "var t=(ins[j].type||'text').toLowerCase();"
                    "if((t==='text'||t==='email'||t==='tel')&&ins[j].value){"
                    "if(!seen)before=ins[j].value;if(!any)any=ins[j].value;}}"
                    "return JSON.stringify({u:before||any,p:p.value,host:location.host});}"
                    "return '';})()";
                WLOG("saveLogin: form nav type=%d -> snapshot creds (deferring, failsafe 500ms)", (int)nt);
                self->m_lastLoginDeferMs = nowms;
                self->m_loginDecision = WEBKIT_POLICY_DECISION(g_object_ref(decision));
                webkit_web_view_evaluate_javascript(self->m_webView, kCapJs, -1, nullptr, nullptr, nullptr,
                                                    &BrowserPageWPE::onLoginCapture, self);
                g_timeout_add(500, &BrowserPageWPE::onLoginDeferTimeout, self);
                return TRUE;   /* we own the decision now; resumed in onLoginCapture or the failsafe timer */
            }
        }
    }
    return FALSE;   /* default policy for navigation / new-window / displayable responses */
}

/* Save-login: result of the form-submit snapshot → msgActionData("saveLogin", json) → app offers to save. */
void BrowserPageWPE::onLoginCapture(GObject* obj, GAsyncResult* res, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    GError* err = nullptr;
    JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (bpwpe_dead(self)) { if (v) g_object_unref(v); if (err) g_error_free(err); return; }   // page torn down mid-eval (decision freed in dtor)
    if (v) {
        char* s = jsc_value_to_string(v);
        WLOG("saveLogin: capture -> %s", (s && s[0]) ? s : "(empty)");
        if (s && s[0] == '{' && self->m_server)
            self->m_server->msgActionData(self->m_proxy, "saveLogin", s);
        if (s) g_free(s);
        g_object_unref(v);
    } else {
        WLOG("saveLogin: capture js failed: %s", err ? err->message : "?");
        if (err) g_error_free(err);
    }
    /* Resume the navigation we deferred so the snapshot could run against the live document. */
    if (self->m_loginDecision) {
        webkit_policy_decision_use(self->m_loginDecision);
        g_object_unref(self->m_loginDecision);
        self->m_loginDecision = nullptr;
    }
}

/* Page scrape (see scrolltest_poll /tmp/atlas_scrape): run arbitrary JS, write the result to a host-readable file. */
void BrowserPageWPE::runScrape(const char* js)
{
    if (m_webView && js && js[0])
        webkit_web_view_evaluate_javascript(m_webView, js, -1, nullptr, nullptr, nullptr, &BrowserPageWPE::onScrapeResult, this);
}

void BrowserPageWPE::onScrapeResult(GObject* obj, GAsyncResult* res, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    GError* err = nullptr;
    JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (bpwpe_dead(self)) { if (v) g_object_unref(v); if (err) g_error_free(err); return; }
    FILE* out = fopen("/tmp/atlas_scrape_out", "w");
    if (v) {
        char* s = jsc_value_to_string(v);
        if (out) fputs(s ? s : "", out);
        if (s) g_free(s);
        g_object_unref(v);
    } else if (out)
        fprintf(out, "JSERROR: %s", err ? err->message : "?");
    if (out) fclose(out);
    if (err) g_error_free(err);
}

/* Failsafe: if the capture callback never fires (page torn down mid-eval), resume the deferred navigation
 * anyway so browsing can never permanently stall on a login-form snapshot. One-shot. */
gboolean BrowserPageWPE::onLoginDeferTimeout(gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    if (bpwpe_dead(self)) return G_SOURCE_REMOVE;   // page freed (dtor already resolved/released the decision)
    if (self->m_loginDecision) {
        WLOG("saveLogin: failsafe timer resuming deferred navigation");
        webkit_policy_decision_use(self->m_loginDecision);
        g_object_unref(self->m_loginDecision);
        self->m_loginDecision = nullptr;
    }
    return G_SOURCE_REMOVE;
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
    /* webOS-style text selection: a warm yellow highlight with dark text (WebKit's default is iOS blue).
     * User-level !important stylesheet so it wins over the page's own ::selection. The :window-inactive
     * variant is essential: without it WebKit dims the selection to grey whenever the view isn't the active
     * window (which our offscreen WPE view usually is), so the highlight flickered yellow->grey. Forcing the
     * inactive color to the same yellow keeps it stable until the selection is cleared. */
    WebKitUserStyleSheet* ss = webkit_user_style_sheet_new(
        "::selection{background-color:#ffe45c !important;color:#1a1a1a !important;}"
        "::-webkit-selection{background-color:#ffe45c !important;color:#1a1a1a !important;}"
        "::selection:window-inactive{background-color:#ffe45c !important;color:#1a1a1a !important;}"
        "::-webkit-selection:window-inactive{background-color:#ffe45c !important;color:#1a1a1a !important;}",
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_STYLE_LEVEL_USER, nullptr, nullptr);
    webkit_user_content_manager_add_style_sheet(ucm, ss);
    webkit_user_style_sheet_unref(ss);

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
    /* The WPEWebProcess was SIGBUS-crashing mid-scroll (core: memcpy fails to commit a page in
     * IPC/StringImpl across threads) = SYSTEM memory exhaustion. Only ~180MB is free, so a 480MB
     * WebKit budget lets it grow past what the system can commit and it dies BEFORE WebKit ever
     * purges. Fix: set the budget near the real free memory so WebKit purges/GCs early enough.
     * Tunable to sweep the safe max: `echo 256 > /tmp/atlas_memlimit` (or BPWPE_MEM_LIMIT). */
    int memLimit = 480;   /* REVERTED to original — the 256/aggressive-purge (this session) is the regression
                           * suspect: more frequent pressure handling drops an in-flight IPC SharedMemory ->
                           * SIGBUS on the 1.86MB write. Original 480/0.40/0.60/0.95/2s scrolled the whole page. */
    { FILE* mf = fopen("/tmp/atlas_memlimit", "r");
      if (mf) { int m = 0; if (fscanf(mf, "%d", &m) == 1 && m >= 64 && m <= 600) memLimit = m; fclose(mf); }
      else { const char* me = getenv("BPWPE_MEM_LIMIT"); if (me) { int m = atoi(me); if (m >= 64 && m <= 600) memLimit = m; } } }
    WebKitMemoryPressureSettings* webMem = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(webMem, memLimit);        /* MB budget / web process */
    webkit_memory_pressure_settings_set_conservative_threshold(webMem, 0.40);
    webkit_memory_pressure_settings_set_strict_threshold(webMem, 0.60);
    webkit_memory_pressure_settings_set_kill_threshold(webMem, 0.95);
    webkit_memory_pressure_settings_set_poll_interval(webMem, 2.0);
    WLOG("memory budget: %d MB (original 0.40/0.60/0.95/2s)", memLimit);
    WebKitWebContext* ctx = WEBKIT_WEB_CONTEXT(g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
        "memory-pressure-settings", webMem, NULL));
    webkit_memory_pressure_settings_free(webMem);
    m_memLimit = memLimit;   /* Feature 6: surfaced in the atlas:about diagnostics page */

    /* Feature 6: the internal `atlas:` URI scheme (e.g. atlas:about) serves a diagnostics page from
     * onAtlasScheme() below — WebKit version, memory budget, /dev/shm size, build stamp, renderer. */
    webkit_web_context_register_uri_scheme(ctx, "atlas", &BrowserPageWPE::onAtlasScheme, this, nullptr);

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
    /* Feature 7: raise TLS errors so bad certs surface via load-failed-with-tls-errors (our SSL
     * confirm dialog) instead of being silently ignored. Applies to both session branches. */
    webkit_network_session_set_tls_errors_policy(session, WEBKIT_TLS_ERRORS_POLICY_FAIL);

    /* (webkit_web_context_set_web_process_count_limit removed in the 2.0 API — it was a no-op anyway.)
     * Caching re-enabled: the "Too Many Cards" OOM was actually the 2MB /dev/shm SIGBUS + WebProcess leaks
     * on card close (both fixed), NOT the cache. WEB_BROWSER model keeps a real resource/bf cache (faster
     * repeat loads + back/forward); WebKit's memory-pressure settings evict under load. DOCUMENT_VIEWER
     * fallback via /tmp/atlas_nocache if memory regresses. */
    bool s_cacheOn = (access("/tmp/atlas_nocache", F_OK) != 0);
    webkit_web_context_set_cache_model(ctx, s_cacheOn ? WEBKIT_CACHE_MODEL_WEB_BROWSER : WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);

    /* Render at the FULL device width. The adapter seeds m_virtualWindowWidth at ~960, which
     * leaves a grey bar on the 1024-wide viewport; and the backend size is fixed at create
     * (resize is a no-op), so we can't widen later. Read the real screen width from the
     * framebuffer; keep the virtual (buffer) height for scroll room. Set m_virtualWindowWidth to
     * match so onFrame's size check + the offscreen header agree with what we actually render. */
    int renderW = m_virtualWindowWidth, renderH = m_virtualWindowHeight;
    /* Rotation (option B): when the window size is known it reflects the CURRENT orientation, so use its
     * width as the layout width — this is what makes the page re-flow after a rotate (ensureWebView is
     * re-run by recreateForResize). Overrides the adapter's fixed first-load seed. */
    if (m_windowWidth > 0) renderW = m_windowWidth;
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
    /* rotation: the live window width wins over the panel config — but ONLY if it's a plausible orientation
     * width. During launch the card flickers through tiny placeholder sizes (300x150); letting a 300px width
     * size the shm SLOT permanently caps the buffer (300-wide) so every real 1024-wide frame is dropped as a
     * size mismatch -> nothing delivered -> ~95% uncovered scroll. Fall back to the panel width for those. */
    if (m_windowWidth >= 600) screenW = m_windowWidth;
    if (screenW < renderW) screenW = renderW;   /* never narrower than the adapter's layout width */
    m_screenWidth  = screenW;
    m_screenHeight = screenH;                    /* 0 if unknown → setScrollPosition uses the safe path */
    /* P1: fill the offscreen to the legacy ~4-screen budget height so scrolling pans a tall pre-rendered
     * window and the ~500ms full-buffer glReadPixels re-renders become rare (or never, for pages that fit).
     * The adapter already allocates this budget (screenW*screenH*4 * 4.0 = 12.58MB). Cap at the measured
     * GL_MAX_TEXTURE_SIZE (4096 on the Adreno 220). Was m_virtualWindowHeight (~1400 ≈ 1.8 screens). */
    if (screenH > 0) {
        /* TALL PAN BUFFER (legacy ~4-screen budget) — smooth scroll: the adapter pans a tall pre-rendered
         * window with NO readback, re-rendering only near the edges. The "white after ~1/3" that made this
         * look memory-bound was actually the 2MB /dev/shm overflowing on WebKit's IPC SharedMemory (fixed in
         * the BS wrapper: `mount -o remount,size=131072k /dev`), NOT the tile memory — so the tall buffer is
         * viable again. Paired with lock_surface (~3x readback) for fast edge re-renders. mult=1 = the
         * low-memory screen-sized fallback. Tunable: echo N > /tmp/atlas_bufscreens (or BPWPE_BUF_SCREENS). */
        /* DEFAULT = 4 (tall pan buffer): SMOOTH SCROLL fundamentally requires pre-rendering into a tall
         * buffer and PANNING within it — the webOS display path has to GPU->CPU read back every delivered
         * frame (~100ms+, and zero-copy EGLImage is dead on this Adreno driver, Phase-0-verified), so we
         * CANNOT read back per scroll frame. Panning a pre-rendered buffer avoids the per-scroll readback
         * entirely; measured tall+strip vs viewport: maxStall 177ms vs 1642ms, uncovered 60% vs 98%.
         * (Viewport mult=1 was tried to stop the continuous-render OOM crash, but it destroyed scroll and
         * damage/async can't fix scroll — only animations. The OOM crash is now CONTAINED by minicore
         * [[atlas-minicore-lockup-containment]], respawn not lockup.) The scroll-strip fast-path (P2) must
         * be ENABLED for this (do NOT set BPWPE_NO_STRIP). Tune with `echo N > /tmp/atlas_bufscreens`;
         * mult=1 = viewport (low memory, laggy scroll). FUTURE: adaptive — tall for scroll pages, shrink
         * to viewport on continuous-render detection, to keep scroll AND avoid the (contained) crash.
         * NOTE: with the tall buffer KEEP the P2 scroll-strip OFF (wrapper BPWPE_NO_STRIP=1) — recenter
         * jumps ~slack/2 (~1000px+), so the "strip" is a big slow sub-rect read + ~30MB memmove that
         * LOSES to one full-buffer EGL lock_surface readback (A/B: strip OFF 26%/730ms vs ON 53%/1432ms).
         * The strip only wins for viewport-sized rendering (small deltas). */
        int mult = 4;
        FILE* bf = fopen("/tmp/atlas_bufscreens", "r");
        if (bf) { int m = 0; if (fscanf(bf, "%d", &m) == 1 && m >= 1 && m <= 6) mult = m; fclose(bf); }
        else { const char* be = getenv("BPWPE_BUF_SCREENS"); if (be) { int m = atoi(be); if (m >= 1 && m <= 6) mult = m; } }
        if (mult <= 1) {
            renderH = screenH;                       /* screen-sized: WebKit manages/discards tiles */
        } else {
            renderH = screenH * mult;                /* tall pan-buffer (memory-heavy) */
            if (renderH > 4096) renderH = 4096;
            if (renderH < m_virtualWindowHeight) renderH = m_virtualWindowHeight;
        }
        m_virtualWindowHeight = renderH;
        WLOG("buffer height: %d screen(s) -> renderH=%d%s", mult, renderH, mult <= 1 ? " (screen-sized/WAM)" : "");
    }
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

    m_viewBackend = wpe_atlas_view_backend_create(renderW, renderH, scaledW, scaledH,
                                                 &BrowserPageWPE::onFrame, this);
    WebKitWebViewBackend* vb = webkit_web_view_backend_new(m_viewBackend, nullptr, nullptr);
    m_webView = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,    // 2.0: context + network session
                                             "backend", vb, "web-context", ctx,
                                             "network-session", session, NULL));
    g_object_ref_sink(m_webView);
    g_object_unref(ctx);       // the WebView holds its own refs now
    g_object_unref(session);

    /* Wire the native input-method focus signal → INSTANT webOS keyboard on field focus. The AtlasIMContext
     * (notify_focus_in/out → onEditorFocus → msgEditorFocused) was defined above but never attached, so
     * editor focus relied entirely on the slow checkEditorFocus JS poll — which crawls behind a heavy page's
     * own JS on this CPU, delaying the keyboard by many seconds (tweakers login). Attaching the context makes
     * focus detection native + immediate. */
    {
        AtlasIMContext* imctx = reinterpret_cast<AtlasIMContext*>(g_object_new(atlas_im_context_get_type(), NULL));
        imctx->page = this;
        webkit_web_view_set_input_method_context(m_webView, WEBKIT_INPUT_METHOD_CONTEXT(imctx));
        g_object_unref(imctx);   /* the WebView holds its own ref */
    }

    /* This page has a live WebView -> it's the scroll-test target (NOT a startPage/tab that only
     * attached buffers with screenH=0). Install the trigger once (poll works despite masked SIGUSR1). */
    g_testablePage = this;
    static bool s_testInstalled = false;
    if (!s_testInstalled) {
        s_testInstalled = true;
        g_unix_signal_add(SIGUSR1, on_sigusr1, nullptr);
        g_timeout_add(700, scrolltest_poll, nullptr);
    }

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
    /* Media (HTML5 <video>): allow playback (incl. muted autoplay) without a prior user gesture, so pages
     * with autoplay work and tap-to-play isn't swallowed. No-op when the engine is built media-off. */
    webkit_settings_set_media_playback_requires_user_gesture(s, FALSE);
    webkit_settings_set_enable_media(s, TRUE);
    /* MediaStream / getUserMedia (WPE defaults this OFF): exposes navigator.mediaDevices so pages can
     * capture the TouchPad camera (surfaced as a GStreamer Video/Source device by libgstqcamsrc.so). */
    webkit_settings_set_enable_media_stream(s, TRUE);
    applyAutoplayFlag();   /* enforce the "autoplay with sound" flag state before this page's media loads */
    /* WebKit DOM fullscreen ABORTS (SIGABRT) the moment it engages in our headless/no-window embedding —
     * proven inherent to WebKit's async fullscreen continuation, NOT our enter-fullscreen handler (deferring
     * all handler work out of the signal still crashed). So keep it OFF by default (requestFullscreen is
     * rejected cleanly, fullscreen is a no-op) for stability; `touch /tmp/atlas_fs` re-enables it for debugging
     * the crash. Proper fullscreen needs the system-media handoff (bypasses WebKit fullscreen) or a WebKit fix. */
    webkit_settings_set_enable_fullscreen(s, access("/tmp/atlas_fs", F_OK) == 0 ? TRUE : FALSE);
    webkit_settings_set_enable_developer_extras(s, FALSE);
    /* Route page console.log/warn/error to BS stdout (/tmp/bs-atlas.log) when /tmp/atlas_console exists.
     * A headless readout for on-device web-compat debugging (why site X rejects us / fails to load) with
     * no inspector; off by default so production pays nothing. */
    webkit_settings_set_enable_write_console_messages_to_stdout(s, access("/tmp/atlas_console", F_OK) == 0 ? TRUE : FALSE);
    webkit_settings_set_enable_page_cache(s, (access("/tmp/atlas_nocache", F_OK) != 0) ? TRUE : FALSE);   /* bf-cache = instant back/forward; /dev/shm + leak fixes made the OOM moot. Disable via /tmp/atlas_nocache */
    /* Runtime double-gating: these ship as build flags in the engine but WPE defaults them OFF, so the
     * embedder must flip them on. EME uses WebKit's built-in ClearKey CDM (no external DRM needed).
     * requestIdleCallback + LinkPrefetch are widely-used modern-web APIs defaulted off on WPE — enable via
     * the WebKitFeature list (no dedicated setter). Match by substring to be robust to the identifier suffix. */
    webkit_settings_set_enable_encrypted_media(s, TRUE);
    {
        WebKitFeatureList* feats = webkit_settings_get_all_features();
        if (feats) {
            gsize n = webkit_feature_list_get_length(feats);
            int en = 0;
            for (gsize i = 0; i < n; i++) {
                WebKitFeature* f = webkit_feature_list_get(feats, i);
                const char* id = webkit_feature_get_identifier(f);
                /* Forms input types (date/datetime-local/month/time/week/color): mature prefs, default ON for
                 * GTK/Cocoa but OFF for WPE. Enabling makes input.type return the real type (html5test scores
                 * it) even without a native picker UI. Batched with requestIdleCallback + LinkPrefetch. */
                /* ExposeSpeakers[WithoutMicrophone]: without these, MediaDevices.enumerateDevices()
                 * returns NO audiooutput entries (audio *destination* is empty in WebRTC device tests)
                 * even though the ALSA sink devices exist -- see MediaDevices.cpp checkSpeakerAccess()
                 * gating on settings().exposeSpeakersEnabled(). Internal prefs, default OFF off-Cocoa;
                 * enable via the feature list (no dedicated setter). WithoutMicrophone lets the speaker
                 * list populate even before mic permission is granted. */
                if (id && (strstr(id, "RequestIdleCallback") || strstr(id, "LinkPrefetch")
                         || strstr(id, "ExposeSpeakers")
                         || strstr(id, "InputTypeColor") || strstr(id, "InputTypeDate")
                         || strstr(id, "InputTypeMonth") || strstr(id, "InputTypeTime")
                         || strstr(id, "InputTypeWeek"))) {
                    webkit_settings_set_feature_enabled(s, f, TRUE);
                    WLOG("feature-enable: %s", id);
                    en++;
                }
            }
            WLOG("feature toggle: %d enabled of %zu total features", en, (size_t)n);
            webkit_feature_list_unref(feats);
        }
    }
    /* ALWAYS use a modern WebKit/Safari UA — the adapter otherwise sends the legacy
     * "HP TouchPad webOS 3.0.5" UA, making sites think we're the old engine.
     * WebKit 620 / Safari 18 = WPE 2.52 (was 612/15.0 = WPE 2.34-era). */
    /* Drop the hp-tablet/hpwOS tokens — Google (search/suggest) blocks them with
     * "Update your browser". webOS/6.0 reads as a modern webOS engine. */
    webkit_settings_set_user_agent(s, kAtlasDefaultUA);
    WLOG("UA set to: %s", webkit_settings_get_user_agent(s) ? webkit_settings_get_user_agent(s) : "?");

    g_signal_connect(m_webView, "load-changed",  G_CALLBACK(onLoadChanged), this);
    g_signal_connect(m_webView, "load-failed",   G_CALLBACK(onLoadFailed), this);
    g_signal_connect(m_webView, "notify::title", G_CALLBACK(onTitleChanged), this);
    g_signal_connect(m_webView, "notify::uri",   G_CALLBACK(onUriChanged), this);
    g_signal_connect(m_webView, "notify::estimated-load-progress", G_CALLBACK(onProgressChanged), this);
    g_signal_connect(m_webView, "create",             G_CALLBACK(bpwpe_on_create), this);
    g_signal_connect(m_webView, "permission-request", G_CALLBACK(bpwpe_on_permission), this);
    g_signal_connect(m_webView, "decide-policy",      G_CALLBACK(onDecidePolicy), this);  // unsupported-mime → download
    /* Feature 7: HTTP auth + SSL cert dialogs — route through the app UI via the sync reply pipe. */
    g_signal_connect(m_webView, "authenticate",                 G_CALLBACK(onAuthenticate), this);
    g_signal_connect(m_webView, "load-failed-with-tls-errors",  G_CALLBACK(onTlsErrors), this);
    /* Diagnostics: log WHY the WebProcess died (crash vs memory-limit vs killed-by-API). Heavy SPAs
     * (WhatsApp Web etc.) on the 1GB TouchPad can drive the WebProcess into memory exhaustion —
     * EXCEEDED_MEMORY_LIMIT here distinguishes that from a real crash/assert (CRASHED). */
    g_signal_connect(m_webView, "web-process-terminated", G_CALLBACK(onWebProcessTerminated), this);
    /* Fullscreen: HANDLE it ourselves (return TRUE). The WPE default fullscreen path resizes the
     * view — a no-op on our fixed-size backend — and then blocks waiting for a resize/repaint that
     * never arrives, hanging the page. Returning TRUE takes ownership (no hang), and WebKit still
     * enters DOM fullscreen so the element gets :-webkit-full-screen.
     *
     * BUT our "viewport" is the TALL pan-buffer (1024x3072), not the screen, so the UA :fullscreen
     * rule (height:100% -> 3072) put the video's letterbox strip mid-buffer, off the visible card
     * (the "fullscreen does nothing" bug). Tier-1 fix (CSS): inject a style that caps the fullscreen
     * element to the VISIBLE screen height (m_screenHeight) as a fixed overlay at buffer top, and
     * align the pan so the adapter shows buffer top (m_renderedY = m_scrollY, same identity-blit trick
     * the rotation path uses) so the overlay lands in the visible card. Nudge a repaint via scrollTo.
     * leave-fullscreen removes the style. (Tier-2 later: screen-sized surface resize for higher fps.) */
    g_signal_connect(m_webView, "enter-fullscreen",
        G_CALLBACK(+[](WebKitWebView*, gpointer ud) -> gboolean {
            BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
            /* Preferred path (opt-in /tmp/atlas_mediad): hand the video to the system media server for a
             * true HW MDP overlay (fills the card, HW-decoded ~30fps) — see mediadBegin(). Falls back to the
             * Tier-2 surface-resize WIP otherwise. */
            if (self && access("/tmp/atlas_mediad", F_OK) == 0) {
                self->m_fsPending = true;
                g_timeout_add(300, +[](gpointer p) -> gboolean {
                    BrowserPageWPE* s = static_cast<BrowserPageWPE*>(p);
                    if (!s->m_fsPending || !s->m_webView) return G_SOURCE_REMOVE;
                    s->m_fsPending = false; s->mediadBegin();
                    return G_SOURCE_REMOVE;
                }, self);
                WLOG("enter-fullscreen (mediad handoff queued)"); return TRUE;
            }
            /* Tier-2: shrink the WPE surface to the visible screen so the fullscreen video fills the card
             * (author CSS can't beat the UA :fullscreen !important rule; only the viewport size can). Defer
             * out of the fullscreen state machine (a synchronous WebKit call here crashed before) — run after
             * the DOM transition settles. */
            if (self) { self->m_fsPending = true;
                g_timeout_add(500, +[](gpointer p) -> gboolean {
                    BrowserPageWPE* s = static_cast<BrowserPageWPE*>(p);
                    if (!s->m_fsPending || !s->m_webView) return G_SOURCE_REMOVE;
                    s->m_fsPending = false;
                    s->enterFsResize();
                    return G_SOURCE_REMOVE;
                }, self);
            }
            WLOG("enter-fullscreen (Tier-2 surface resize queued)"); return TRUE; }), this);
    g_signal_connect(m_webView, "leave-fullscreen",
        G_CALLBACK(+[](WebKitWebView*, gpointer ud) -> gboolean {
            BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
            if (self) { self->m_fsPending = false;   /* cancel any not-yet-run resize */
                if (self->m_mediadActive) self->mediadEnd();
                else self->exitFsResize(); }
            WLOG("leave-fullscreen (restore)"); return TRUE; }), this);

    applyContentBlocker(webkit_web_view_get_user_content_manager(m_webView));

    /* Editor-focus → VKB is driven by checkEditorFocus() after each tap (see clickAt/mouseEvent): it
     * polls document.activeElement via evaluate_javascript — the same proven-safe path as the page-
     * height query. The script-message-received channel was tried but its signal value crashed the BS
     * (jsc_value_* on a non-JSCValue at the UIProcess), so it is intentionally not used. */

    /* Partial-readback for video: poll the playing <video>'s rect every 300ms (cheap; the rect only
     * changes on play/pause/scroll/fullscreen). While a video plays the backend reads back ONLY that
     * rect instead of the full 1024x3072 buffer. Disable via /tmp/atlas_no_vrect (backend-side gate). */
    if (!m_videoPollTimer)
        m_videoPollTimer = g_timeout_add(300, (GSourceFunc)&BrowserPageWPE::pollVideoRect, this);
}

/* ---- Feature 6: atlas: internal scheme (atlas:about diagnostics page) ------------------------- */
void BrowserPageWPE::onAtlasScheme(WebKitURISchemeRequest* req, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    const char* uri = webkit_uri_scheme_request_get_uri(req);   // e.g. "atlas:about" (opaque, no //)
    /* atlas:about is opaque — get_path is empty; parse the part after the first ':'. */
    const char* colon = uri ? strchr(uri, ':') : nullptr;
    std::string path = colon ? std::string(colon + 1) : std::string();
    /* strip any leading slashes just in case a "atlas:/about" form arrives */
    while (!path.empty() && path[0] == '/') path.erase(0, 1);

    gchar* html = nullptr;
    if (path == "about") {
        /* Small helpers: slurp a /proc file, pull a "Key: value" field, format bytes as MB. */
        auto slurp = [](const char* p) -> std::string {
            std::string out; FILE* f = fopen(p, "r"); if (!f) return out;
            char b[512]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
            fclose(f); return out;
        };
        auto field = [](const std::string& s, const char* key) -> std::string {
            size_t p = s.find(key); if (p == std::string::npos) return "";
            p = s.find(':', p); if (p == std::string::npos) return "";
            ++p; while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
            size_t e = s.find('\n', p);
            std::string v = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
            while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
            return v;
        };
        auto mb = [](unsigned long long bytes) -> std::string {
            char b[48]; snprintf(b, sizeof b, "%.1f MB", (double)bytes / (1024.0 * 1024.0)); return b;
        };

        /* System memory (2.6.35 has no MemAvailable → approximate as free+cached+buffers). */
        std::string mi = slurp("/proc/meminfo");
        long memTotalKB = atol(field(mi, "MemTotal").c_str());
        long memFreeKB  = atol(field(mi, "MemFree").c_str());
        long cachedKB   = atol(field(mi, "Cached").c_str());
        long buffersKB  = atol(field(mi, "Buffers").c_str());
        long memAvailKB = memFreeKB + cachedKB + buffersKB;

        /* CPU / load / uptime. */
        std::string ci = slurp("/proc/cpuinfo");
        std::string cpu = field(ci, "Processor"); if (cpu.empty()) cpu = field(ci, "model name");
        int cores = 0; for (size_t p = 0; (p = ci.find("processor", p)) != std::string::npos; p += 9) ++cores;
        std::string la = slurp("/proc/loadavg");   /* keep the 3 load figures, drop the rest */
        { size_t sp = la.find(' '); if (sp != std::string::npos) sp = la.find(' ', sp + 1);
          if (sp != std::string::npos) sp = la.find(' ', sp + 1);
          if (sp != std::string::npos) la.resize(sp); }
        double up = atof(slurp("/proc/uptime").c_str());
        int upH = (int)(up / 3600), upM = (int)((up - upH * 3600) / 60);

        /* /dev/shm (WebKit IPC SharedMemory budget) + persistent storage free. */
        unsigned long long shmTot = 0, shmFree = 0, medTot = 0, medFree = 0;
        struct statvfs st;
        if (statvfs("/dev/shm", &st) == 0) {
            shmTot  = (unsigned long long)st.f_blocks * st.f_bsize;
            shmFree = (unsigned long long)st.f_bavail * st.f_bsize;
        }
        struct statvfs ms;
        if (statvfs("/media/internal", &ms) == 0) {
            medTot  = (unsigned long long)ms.f_blocks * ms.f_frsize;
            medFree = (unsigned long long)ms.f_bavail * ms.f_frsize;
        }

        /* Cookie store size (fopen/ftell — no extra includes). */
        long long cookieSz = -1;
        { FILE* cf = fopen("/media/internal/wpe-252/cookies.db", "rb");
          if (cf) { fseek(cf, 0, SEEK_END); cookieSz = ftell(cf); fclose(cf); } }
        char cookieBuf[48];
        if (cookieSz >= 0) snprintf(cookieBuf, sizeof cookieBuf, "%.1f KB", cookieSz / 1024.0);
        else               snprintf(cookieBuf, sizeof cookieBuf, "(none)");

        const char* ua = (self && self->m_webView)
            ? webkit_settings_get_user_agent(webkit_web_view_get_settings(self->m_webView)) : "(n/a)";
        const char* tls  = getenv("GIO_USE_TLS");
        const char* cert = getenv("SSL_CERT_FILE");
        bool cacheOn = (access("/tmp/atlas_nocache", F_OK) != 0);

        std::string h;
        h += "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
             /* Light, webOS/Enyo-like: Prelude UI font, light-grey page, white cards, dark text. */
             "<style>body{background:#e5e5e5;color:#333;font-family:Prelude,sans-serif;margin:0;padding:20px}"
             "h1{color:#2b2b2b;font-size:22px;font-weight:bold;margin:0 0 2px}"
             "h2{color:#777;font-size:12px;text-transform:uppercase;letter-spacing:.06em;margin:22px 0 6px;"
             "padding-bottom:4px;border-bottom:1px solid #cfcfcf}"
             ".sub{color:#999;font-size:12px;margin:0 0 8px}"
             "table{border-collapse:separate;border-spacing:0;width:100%;background:#fff;"
             "border:1px solid #cfcfcf;-webkit-border-radius:6px;border-radius:6px;overflow:hidden}"
             "td{padding:8px 12px;border-bottom:1px solid #ededed;font-size:14px;vertical-align:top}"
             "tr:last-child td{border-bottom:none}"
             "td.k{color:#888;width:42%}code{color:#1a6bb5;font-family:monospace;word-break:break-all}</style>"
             "<title>Atlas Diagnostics</title></head><body>";
        h += "<h1>Atlas Browser</h1><p class='sub'>WPE WebKit engine diagnostics</p>";

        char row[2048];
        #define ROW(k, fmt, ...) do { snprintf(row, sizeof row, \
            "<tr><td class='k'>" k "</td><td><code>" fmt "</code></td></tr>", ##__VA_ARGS__); h += row; } while (0)

        h += "<h2>Engine</h2><table>";
        ROW("WebKit version", "%d.%d.%d", webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
        ROW("Port", "%s", "WPE 2.52 &middot; offscreen atlas backend");
        ROW("Cache model", "%s", cacheOn ? "Web browser (resource + bf cache)" : "Document viewer (no cache)");
        ROW("Page cache (bf)", "%s", cacheOn ? "enabled" : "disabled");
        ROW("TLS backend", "GIO_USE_TLS=%s", tls ? tls : "(unset!)");
        ROW("CA bundle", "%s", cert ? cert : "(unset!)");
        ROW("User agent", "%s", ua ? ua : "(n/a)");
        ROW("Build", "%s", __DATE__ " " __TIME__);
        h += "</table>";

        h += "<h2>Display</h2><table>";
        ROW("Renderer", "%s", "Qualcomm Adreno 220");
        ROW("Screen", "%d &times; %d", self ? self->m_screenWidth : 0, self ? self->m_screenHeight : 0);
        ROW("Render buffer", "%d &times; %d", self ? self->m_virtualWindowWidth : 0, self ? self->m_virtualWindowHeight : 0);
        h += "</table>";

        h += "<h2>Memory</h2><table>";
        ROW("WebProcess budget", "%d MB", self ? self->m_memLimit : 0);
        ROW("System RAM total", "%ld MB", memTotalKB / 1024);
        ROW("System RAM free", "%ld MB (avail ~%ld MB)", memFreeKB / 1024, memAvailKB / 1024);
        ROW("Cached / buffers", "%ld MB / %ld MB", cachedKB / 1024, buffersKB / 1024);
        ROW("/dev/shm (IPC)", "%s free of %s", mb(shmFree).c_str(), mb(shmTot).c_str());
        h += "</table>";

        h += "<h2>System</h2><table>";
        ROW("CPU", "%s", cpu.empty() ? "(unknown)" : cpu.c_str());
        ROW("Cores", "%d", cores);
        ROW("Load average", "%s", la.empty() ? "(n/a)" : la.c_str());
        ROW("Uptime", "%dh %dm", upH, upM);
        ROW("Storage (/media/internal)", "%s free of %s", mb(medFree).c_str(), mb(medTot).c_str());
        h += "</table>";

        h += "<h2>Session</h2><table>";
        ROW("Private browsing", "%s", (self && self->m_private) ? "yes (ephemeral)" : "no (persistent)");
        ROW("Cookie store", "%s", cookieBuf);
        ROW("Identifier", "%s", (self && self->m_identifier) ? self->m_identifier : "(none)");
        h += "</table>";
        #undef ROW

        h += "</body></html>";
        html = g_strdup(h.c_str());
    } else {
        html = g_strdup_printf(
            "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>body{background:#101418;color:#e6e6e6;font-family:sans-serif;padding:24px}</style></head>"
            "<body><h2>Atlas internal page</h2><p>Unknown page: <code>%s</code></p>"
            "<p>Try <a style='color:#4ea1ff' href='atlas:about'>atlas:about</a>.</p></body></html>",
            path.empty() ? "(none)" : path.c_str());
    }

    gsize len = html ? strlen(html) : 0;
    gchar* buf = g_strdup(html ? html : "");
    if (html) g_free(html);
    GInputStream* s = g_memory_input_stream_new_from_data(buf, (gssize)len, g_free);
    webkit_uri_scheme_request_finish(req, s, (gint64)len, "text/html");
    g_object_unref(s);
}

/* ---- Feature 3: bookmark favicon — the app calls saveViewToFile → renderToFile; we download the site's
 * favicon (via our modern-TLS engine) to a LOCAL file the app chrome can display. The app chrome runs in
 * LunaSysMgr's old WebKit (system OpenSSL 0.9.8) and CANNOT fetch modern-TLS https favicons itself, so the
 * engine does it. WPE 2.52 here has NO favicon-database API, so we can't read the page's declared
 * <link rel=icon> — we use the conventional origin + "/favicon.ico" (covers the large majority of sites).
 *
 * CRITICAL: this is best-effort and runs ASYNCHRONOUSLY — it ALWAYS returns 0 immediately. A non-zero
 * return makes the adapter throw a JS exception that aborts addBookmark BEFORE the DB write (breaks
 * bookmarking entirely). And a blocking (nested-loop) download would stall every bookmark. So we just kick
 * off the download and let it land in the background; the icon appears next time the list renders. ------- */
/* The old LunaSysMgr WebKit (which renders the app chrome) can decode PNG but NOT ICO, so we must fetch a
 * PNG icon, not favicon.ico. Try the de-facto PNG icon paths first; favicon.ico is last (works only for the
 * minority of sites that actually serve a PNG there — for ICO sites it just fails and we end up with no icon,
 * which is no worse than before). Chained: on each download failure we advance to the next candidate. */
static const char* const ATLAS_FAVICON_CANDIDATES[] = {
    "/apple-touch-icon.png",
    "/apple-touch-icon-precomposed.png",
    "/favicon.ico",
    nullptr
};
struct AtlasFaviconDl {
    WebKitWebView* view;
    std::string origin;   /* scheme://host[:port] */
    std::string dest;
    int idx;
    gulong hDst, hFin, hFail;
};

static void atlas_favicon_try_next(AtlasFaviconDl* s);   /* fwd */

static void atlas_favicon_disconnect(WebKitDownload* dl, AtlasFaviconDl* s) {
    if (s->hDst)  { g_signal_handler_disconnect(dl, s->hDst);  s->hDst = 0; }
    if (s->hFin)  { g_signal_handler_disconnect(dl, s->hFin);  s->hFin = 0; }
    if (s->hFail) { g_signal_handler_disconnect(dl, s->hFail); s->hFail = 0; }
}
static gboolean atlas_favicon_decide_dest(WebKitDownload* dl, gchar* /*suggested*/, gpointer ud) {
    AtlasFaviconDl* s = static_cast<AtlasFaviconDl*>(ud);
    webkit_download_set_destination(dl, s->dest.c_str());   /* plain path in 2.40+ API */
    return TRUE;
}
static void atlas_favicon_dl_finished(WebKitDownload* dl, gpointer ud) {
    AtlasFaviconDl* s = static_cast<AtlasFaviconDl*>(ud);
    WLOG("favicon saved -> %s (via %s)", s->dest.c_str(), ATLAS_FAVICON_CANDIDATES[s->idx]);
    atlas_favicon_disconnect(dl, s);
    g_object_unref(dl);
    g_object_unref(s->view);
    delete s;
}
static void atlas_favicon_dl_failed(WebKitDownload* dl, GError*, gpointer ud) {
    AtlasFaviconDl* s = static_cast<AtlasFaviconDl*>(ud);
    atlas_favicon_disconnect(dl, s);
    g_object_unref(dl);
    s->idx++;
    atlas_favicon_try_next(s);   /* advance to the next candidate path, or clean up when exhausted */
}
static void atlas_favicon_try_next(AtlasFaviconDl* s) {
    if (!ATLAS_FAVICON_CANDIDATES[s->idx]) { g_object_unref(s->view); delete s; return; }   /* exhausted — leave no file */
    std::string uri = s->origin + ATLAS_FAVICON_CANDIDATES[s->idx];
    WebKitDownload* dl = webkit_web_view_download_uri(s->view, uri.c_str());
    if (!dl) { g_object_unref(s->view); delete s; return; }
    webkit_download_set_destination(dl, s->dest.c_str());
    s->hDst  = g_signal_connect(dl, "decide-destination", G_CALLBACK(atlas_favicon_decide_dest), s);
    s->hFin  = g_signal_connect(dl, "finished", G_CALLBACK(atlas_favicon_dl_finished), s);
    s->hFail = g_signal_connect(dl, "failed",   G_CALLBACK(atlas_favicon_dl_failed),   s);
}

/* Favicons MUST live inside the app's own bundle: LunaSysMgr's isAllowedFileAccess policy blocks the app
 * from reading /var/luna/data/browser/icons (that's the stock Palm browser's privileged dir). The app can
 * only read files under its own install dir, so we write there and the app references them via a RELATIVE
 * "faviconcache/..." path. The host authority is sanitized to [A-Za-z0-9.-] (else '_'); the APP computes the
 * SAME leaf name. Empty for non-http(s) URIs. */
#define ATLAS_FAVICON_DIR "/media/cryptofs/apps/usr/palm/applications/org.webosports.app.atlas/faviconcache"
static std::string atlas_favicon_dest_for(const char* pageUri) {
    if (!pageUri || strncmp(pageUri, "http", 4) != 0) return std::string();
    const char* p = strstr(pageUri, "://");
    if (!p) return std::string();
    p += 3;
    const char* slash = strchr(p, '/');
    std::string authority = slash ? std::string(p, slash - p) : std::string(p);
    if (authority.empty()) return std::string();
    std::string safe;
    safe.reserve(authority.size());
    for (size_t i = 0; i < authority.size(); ++i) {
        char c = authority[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
        safe += ok ? c : '_';
    }
    return std::string(ATLAS_FAVICON_DIR "/fav_") + safe + ".png";
}

/* Kick off the chained favicon download (PNG candidates first). Best-effort, never blocks. */
static void atlas_start_favicon_download(WebKitWebView* view, const char* pageUri, const std::string& dest) {
    if (!view || !pageUri) return;
    const char* p = strstr(pageUri, "://");
    if (!p) return;
    const char* slash = strchr(p + 3, '/');
    std::string origin = slash ? std::string(pageUri, slash - pageUri) : std::string(pageUri);
    /* Remove any stale icon (e.g. a previously-saved ICO) so a candidate can write fresh; if all candidates
     * fail we correctly end up with no icon rather than a broken leftover. */
    unlink(dest.c_str());
    AtlasFaviconDl* s = new AtlasFaviconDl();
    /* Hold a ref for the download's lifetime: the page (and its dtor's unref) may go away mid-download, and
     * atlas_favicon_try_next would otherwise call download_uri on a freed WebView. The object stays valid even
     * after the web process is terminated; every terminal cleanup path unrefs it. */
    s->view = WEBKIT_WEB_VIEW(g_object_ref(view));
    s->origin = origin;
    s->dest = dest;
    s->idx = 0;
    s->hDst = s->hFin = s->hFail = 0;
    atlas_favicon_try_next(s);
}

/* The saveViewToFile sync command doesn't reach here reliably in the WPE port, so favicon fetching is driven
 * from onLoadChanged(LOAD_FINISHED) instead (see atlas_start_favicon_download). This stays a no-op that NEVER
 * fails — a non-zero return makes the adapter throw and abort addBookmark. */
int BrowserPageWPE::renderToFile(const char*, int32_t, int32_t, int32_t, int32_t)
{
    return 0;
}

/* ---- Feature 7: HTTP auth dialog — round-trips to the app UI for username/password ---------- */
gboolean BrowserPageWPE::onAuthenticate(WebKitWebView*, WebKitAuthenticationRequest* req, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    if (!self || !self->m_proxy) return FALSE;
    if (!self->m_syncReplyPipe)
        self->m_syncReplyPipe = new BrowserSyncReplyPipe(reinterpret_cast<BrowserPage*>(self));

    const char* host = webkit_authentication_request_get_host(req);
    self->m_server->msgDialogUserPassword(self->m_proxy, self->m_syncReplyPipe->pipePath(),
                                          host ? host : "");

    GPtrArray* reply = nullptr;
    if (!self->m_syncReplyPipe->getReply(&reply, self->m_proxy->commandSocketFd()) || !reply) {
        webkit_authentication_request_cancel(req);
        return TRUE;
    }

    bool ok = reply->len >= 1 && ::atoi(static_cast<const char*>(g_ptr_array_index(reply, 0))) != 0;
    if (ok && reply->len >= 3) {
        const char* user = static_cast<const char*>(g_ptr_array_index(reply, 1));
        const char* pass = static_cast<const char*>(g_ptr_array_index(reply, 2));
        WebKitCredential* c = webkit_credential_new(user ? user : "", pass ? pass : "",
                                                    WEBKIT_CREDENTIAL_PERSISTENCE_FOR_SESSION);
        webkit_authentication_request_authenticate(req, c);
        webkit_credential_free(c);
    } else {
        webkit_authentication_request_cancel(req);
    }

    for (guint i = 0; i < reply->len; i++) ::free(g_ptr_array_index(reply, i));
    g_ptr_array_free(reply, TRUE);
    return TRUE;
}

/* ---- Feature 7: SSL cert confirm dialog — writes the cert PEM to a temp file for the app ----- */
gboolean BrowserPageWPE::onTlsErrors(WebKitWebView* v, const char* uri, GTlsCertificate* cert,
                                     GTlsCertificateFlags errs, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    if (!self || !self->m_proxy) return FALSE;
    if (!self->m_syncReplyPipe)
        self->m_syncReplyPipe = new BrowserSyncReplyPipe(reinterpret_cast<BrowserPage*>(self));

    /* host from the failing URI */
    std::string host;
    if (uri) {
        const char* p = strstr(uri, "://");
        p = p ? p + 3 : uri;
        const char* e = p;
        while (*e && *e != '/' && *e != ':') e++;
        host.assign(p, e - p);
    }

    /* write the cert PEM to a temp file the app can read/display */
    char certPath[] = "/tmp/atlas-cert-XXXXXX";
    int fd = mkstemp(certPath);
    gchar* pem = nullptr;
    if (cert) g_object_get(cert, "certificate-pem", &pem, NULL);
    if (fd >= 0) {
        if (pem) { ssize_t n = write(fd, pem, strlen(pem)); (void)n; }
        close(fd);
    }

    self->m_server->msgDialogSSLConfirm(self->m_proxy, self->m_syncReplyPipe->pipePath(),
                                        host.c_str(), (int)errs, certPath);

    GPtrArray* reply = nullptr;
    gboolean gotReply = self->m_syncReplyPipe->getReply(&reply, self->m_proxy->commandSocketFd()) && reply;

    int decision = 0;   // 0 = reject (default)
    if (gotReply && reply->len >= 1)
        decision = ::atoi(static_cast<const char*>(g_ptr_array_index(reply, 0)));

    if (decision != 0 && cert) {
        /* accept: pin the cert for this host and retry the load */
        webkit_network_session_allow_tls_certificate_for_host(
            webkit_web_view_get_network_session(v), cert, host.c_str());
        webkit_web_view_load_uri(v, uri);
    }

    if (gotReply && reply) {
        for (guint i = 0; i < reply->len; i++) ::free(g_ptr_array_index(reply, i));
        g_ptr_array_free(reply, TRUE);
    }
    if (pem) g_free(pem);
    unlink(certPath);
    return TRUE;
}

/* ---- Atlas buffer attach (mirrors BrowserPage::attachToBuffer) ------------------------------ */
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
    self->m_renderPending = false;   /* this re-render's frame arrived -> allow the next pan re-render */
    if (g_st.active && self->m_renderPendingMs) {   /* scroll-test: record the re-render latency (the readback wall) */
        long st = _wlog_ms() - self->m_renderPendingMs; if (st < 0) st += 10000000;
        if (st > g_st.maxStallMs) g_st.maxStallMs = st;
    }
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
        hdr->contentZoom = ((self->m_virtualWindowWidth > 0)
                           ? (double)w / (double)self->m_virtualWindowWidth : 1.0) * self->m_webkitZoom;
        hdr->renderedX = 0; hdr->renderedY = self->m_renderedY;   /* pan model: buffer's content-top */
        hdr->renderedWidth = (int)w; hdr->renderedHeight = (int)h;
    }

    if (getenv("BPWPE_DUMPFRAME")) {   /* dump the offscreen the adapter will show → diagnose the 3× repeat */
        FILE* fp = fopen("/tmp/atlas_frame.bgra", "wb");
        if (fp) { fprintf(fp, "ATLAS %u %u %u\n", w, h, dstStride);
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

    self->m_deliveredY = self->m_renderedY;   /* the adapter now shows the buffer at this top (test coverage) */

    /* P2 catch-up: the scroll may have moved outside the buffer while this frame was in flight (those
     * setScrollPosition calls got gated by m_renderPending). Now that we've delivered and cleared the
     * flag, follow the latest scroll so the buffer doesn't freeze behind the scroll (the "renderedY
     * stuck at N -> white everywhere else" bug). No-op when the scroll is already inside the buffer. */
    self->recenterForScroll(self->m_scrollX, self->m_scrollY);
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
     * setIdentifier to the BS). The app opens a private card with "atlas-private:" + the real target,
     * BUT the app prepends http:// to unknown schemes, so it can arrive as "atlas-private:X" OR
     * "http://atlas-private:X". Strip an optional leading http:// before matching. Detect it BEFORE
     * ensureWebView so the session is created ephemeral, then strip the marker and load the target. */
    {
        std::string c = (u.compare(0, 7, "http://") == 0) ? u.substr(7) : u;
        if (c.compare(0, 14, "atlas-private:") == 0) {
            m_private = true;
            u = c.substr(14);
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
        && u.compare(0, 5, "file:") != 0 && u.compare(0, 7, "mailto:") != 0
        && u.compare(0, 6, "atlas:") != 0)   /* Feature 6: keep "atlas:about" verbatim for the custom scheme */
        u = "http://" + u;
    webkit_web_view_load_uri(m_webView, u.c_str());
}
void BrowserPageWPE::setHTML(const char* url, const char* body) { ensureWebView(); webkit_web_view_load_html(m_webView, body, url); }
void BrowserPageWPE::pageBackward()             { WLOG("pageBackward (engine go_back)"); if (m_webView) { m_navByHistory = true; webkit_web_view_go_back(m_webView); } }
void BrowserPageWPE::pageForward()              { WLOG("pageForward (engine go_forward)"); if (m_webView) { m_navByHistory = true; webkit_web_view_go_forward(m_webView); } }
void BrowserPageWPE::pageReload()               { if (m_webView) webkit_web_view_reload(m_webView); }
void BrowserPageWPE::pageStop()                 { if (m_webView) webkit_web_view_stop_loading(m_webView); }
bool BrowserPageWPE::canGoBackward() const      { return m_webView && webkit_web_view_can_go_back(m_webView); }
bool BrowserPageWPE::canGoForward() const       { return m_webView && webkit_web_view_can_go_forward(m_webView); }
void BrowserPageWPE::clearHistory()             { /* TODO: WebKit2/WPE has no back-forward-list clear API; clear via the WebsiteDataManager (webkit_website_data_manager_clear, WEBKIT_WEBSITE_DATA_*) or recreate the view. */ }

/* ---- sizing -------------------------------------------------------------------------------- */
void BrowserPageWPE::setWindowSize(uint32_t w, uint32_t h)
{
    WLOG("setWindowSize %ux%u (was win=%dx%d virt=%dx%d webView=%p)", w, h, m_windowWidth, m_windowHeight, m_virtualWindowWidth, m_virtualWindowHeight, (void*)m_webView);
    int oldW = m_windowWidth;
    m_windowWidth = (int)w; m_windowHeight = (int)h;
    /* The VISIBLE viewport height is the window height (portrait ~942/1024, landscape ~686, minus the VKB) —
     * NOT the framebuffer height set at view creation. Track it so the scroll clamp / visible-region math use
     * the real height (fixes portrait staying at the landscape 768). */
    if ((int)h > 0) m_screenHeight = (int)h;
    /* Rotation: a WIDTH change means the device rotated (a VKB show/hide only changes height). Re-flow IN
     * PLACE via the view-backend resize (dispatch_set_size); update the reported geometry so onFrame accepts
     * the new width and contentZoom stays 1.0, and re-derive the scroll/pan state + re-query the page height
     * so panning after the rotate doesn't stall. */
    /* Resize only to a PLAUSIBLE orientation width (>=600). During launch the card flickers through tiny
     * placeholder sizes (300x150, 960x1400) — reacting to those, and area-preserving from their bogus areas,
     * COLLAPSED the scroll buffer (300x3072 area / 1024 = layout 1024x900, slack ~214 -> ~95% uncovered). */
    if (m_webView && m_viewBackend && oldW > 0 && (int)w != oldW && m_renderHeight > 0 && (int)w >= 600) {
        int newLayoutW = (int)w;
        /* Rotation preserves the rendered pixel AREA (= the shm slot allocated for the launch orientation):
         * the render height scales inversely with the new width. Guard the area: if the current one is
         * implausibly small (a placeholder slipped through earlier), use the TouchPad tall-buffer slot
         * (1024x3072) so we never area-preserve from garbage. */
        long area = (long)m_renderWidth * m_renderHeight;
        if (area < 2000000L) area = 3145728L;        /* 1024x3072 slot */
        int newLayoutH = (int)(area / newLayoutW);
        if (newLayoutH < 2048) newLayoutH = 2048;    /* never collapse the scroll buffer (safety clamp) */
        m_virtualWindowWidth = newLayoutW;   // layout / document coordinate space
        m_renderWidth  = newLayoutW;         // onFrame validates against this (1:1, no fit-to-screen scaling)
        m_renderHeight = newLayoutH;
        m_screenWidth  = newLayoutW;
        WLOG("rotation resize -> layout %dx%d (area-preserving, visible h=%d)", newLayoutW, newLayoutH, m_screenHeight);
        wpe_atlas_view_backend_resize(m_viewBackend, (uint32_t)newLayoutW, (uint32_t)newLayoutH);
        /* the re-flow changes the page height at the new width — re-query it so the adapter's scroll clamp
         * is right, and reset the pan bookkeeping so the next setScrollPosition isn't gated as "in flight". */
        m_renderPending = false;
        /* Force an immediate repaint at the new orientation. The backend resize re-lays-out WebKit but does
         * not reliably schedule a composite, so without this the reflow only appears after the next scroll.
         * Re-assert the current scroll (in document space) to trigger a paint; the backend's force_full then
         * re-seeds the pan master at the new size. renderedY := scroll so the adapter blit is the identity. */
        m_renderedY = m_scrollY;
        { char js[96]; snprintf(js, sizeof(js), "window.scrollTo(%d,%d)", m_scrollX, m_scrollY);
          webkit_web_view_evaluate_javascript(m_webView, js, -1, nullptr, nullptr, nullptr, nullptr, nullptr); }
        updateContentsSize();
        return;
    }
    if (m_viewBackend && !m_webView) wpe_atlas_view_backend_resize(m_viewBackend, w, h);
}

/* Tier-2 fullscreen: shrink the WPE surface to the VISIBLE screen (1024x686) so WebKit's UA :fullscreen rule
 * (video height:100% !important — unbeatable by author CSS) fills the visible card instead of the 3072-tall
 * pan buffer (which put the letterboxed video off-screen mid-buffer). Bonus: readback drops ~4x -> higher fps.
 * No scrolling happens in fullscreen, so collapsing the tall buffer is safe; exitFsResize() restores it. */
void BrowserPageWPE::enterFsResize()
{
    /* WIP / opt-in (touch /tmp/atlas_fsresize): resizing the layout to 686 makes the fullscreen video FILL
     * the card (innerHeight 686 -> UA :fullscreen height:100% = visible screen), BUT the render FBO does NOT
     * follow — dispatch_set_size(686) resizes WebKit's LAYOUT yet target_resize() never fires in fullscreen
     * (the video top-layer means the compositor doesn't re-render the base viewport at the new size), so the
     * backend keeps delivering 3072-tall frames that the adapter DROPS (frozen fullscreen). OFF by default so
     * the shipped state is just the (crash-fixed) clean enter. TODO: force the compositor to re-render at the
     * new size (forceRepaint / a real size-change nudge) so target_resize fires -> frames 1024x686 -> fills
     * + ~4x fps; OR pan the adapter to the video's letterbox rows in the 3072 buffer. */
    if (access("/tmp/atlas_fsresize", F_OK) != 0) { WLOG("enterFsResize: gated off (touch /tmp/atlas_fsresize to enable WIP)"); return; }
    if (m_fsActive || !m_webView || !m_viewBackend) return;
    int sw = m_windowWidth  >= 600 ? m_windowWidth  : 1024;
    int sh = m_screenHeight >= 500 ? m_screenHeight : 686;
    m_fsRestoreH = m_renderHeight > 0 ? m_renderHeight : 3072;   // save the tall buffer to restore on exit
    m_fsRestoreScrollY = m_scrollY;
    WLOG("enterFsResize -> %dx%d (was render %dx%d, scrollY=%d)", sw, sh, m_renderWidth, m_renderHeight, m_scrollY);
    wpe_atlas_view_backend_resize(m_viewBackend, (uint32_t)sw, (uint32_t)sh);
    m_virtualWindowWidth = sw; m_virtualWindowHeight = sh;
    m_renderWidth = sw; m_renderHeight = sh; m_screenWidth = sw;
    m_renderedY = 0; m_scrollY = 0;
    m_fsActive = true; m_renderPending = false;
    webkit_web_view_evaluate_javascript(m_webView, "window.scrollTo(0,0)", -1, nullptr, nullptr, nullptr, nullptr, nullptr);
    updateContentsSize();
}
void BrowserPageWPE::exitFsResize()
{
    if (!m_fsActive || !m_webView || !m_viewBackend) return;
    int rw = 1024;                                        // full device width
    int rh = m_fsRestoreH > 0 ? m_fsRestoreH : 3072;     // the tall pan buffer
    WLOG("exitFsResize -> restore %dx%d scrollY=%d", rw, rh, m_fsRestoreScrollY);
    wpe_atlas_view_backend_resize(m_viewBackend, (uint32_t)rw, (uint32_t)rh);
    m_virtualWindowWidth = rw; m_virtualWindowHeight = rh;
    m_renderWidth = rw; m_renderHeight = rh; m_screenWidth = rw;
    m_renderedY = m_fsRestoreScrollY; m_scrollY = m_fsRestoreScrollY;
    m_fsActive = false; m_renderPending = false;
    char js[96]; snprintf(js, sizeof(js), "window.scrollTo(%d,%d)", m_scrollX, m_fsRestoreScrollY);
    webkit_web_view_evaluate_javascript(m_webView, js, -1, nullptr, nullptr, nullptr, nullptr, nullptr);
    updateContentsSize();
}
/* ================= mediad system-video handoff (fullscreen HW MDP overlay) =================
 * mediad HW-decodes on the VIDC and owns the MDP/V4L2 overlay a plain app can't get. We open a
 * MEDIA_PLAYER session as the Atlas app (LSCallFromApplication so mediad binds the overlay to the
 * app's card), load the video URL, set the fullscreen bounds/fit, and play; the in-browser <video>
 * is paused meanwhile. On leave-fullscreen we unload, drop the subscription (frees the session), and
 * resume the <video> at its saved time. Direct http/file URLs only — blob:/MSE have no URL to hand off. */

/* enter-fullscreen: read the fullscreen <video>'s currentSrc + currentTime (and pause it) via JS, then
 * onFsVideoInfo opens the session. */
void BrowserPageWPE::mediadBegin()
{
    MLOG("mediadBegin: active=%d busy=%d webView=%p", m_mediadActive, g_mediadBusy, (void*)m_webView);
    if (m_mediadActive || g_mediadBusy || !m_webView) return;
    g_mediadBusy = true;   /* claim the single decoder synchronously so a sibling page won't also start */
    static const char* kJs =
        "(function(){var e=document.webkitFullscreenElement||document.fullscreenElement,v=null,i,vs,x,r,a,ba=0;"
        "if(e&&e.tagName=='VIDEO')v=e;else{vs=document.getElementsByTagName('video');"
        "for(i=0;i<vs.length;i++){x=vs[i];if(!x.currentSrc)continue;r=x.getBoundingClientRect();"
        "a=r.width*r.height;if(a>=ba){ba=a;v=x;}}}"
        "if(!v||!v.currentSrc)return '';var u=v.currentSrc,t=v.currentTime||0;"
        /* Fully RELEASE our in-browser HW decoder so mediad can acquire the single VIDC instance:
         * pause + drop the src + load() tears down the media element's gst pipeline. Restored on exit. */
        "try{v.pause();v.removeAttribute('src');v.load();}catch(e){}"
        "return u+'|'+t;})()";
    webkit_web_view_evaluate_javascript(m_webView, kJs, -1, nullptr, nullptr, nullptr,
                                        &BrowserPageWPE::onFsVideoInfo, this);
}

/* Shrink our WebProcess GL FBO + lock-pbuffer (the pmem_smipool holders) to a small size so mediad's VIDC
 * decoder output buffers + MDP overlay can allocate from the SMI pool. We don't render the page while mediad
 * owns the screen, so the tiny surface is fine; restored on exit. Nudge a repaint so the WebProcess actually
 * recreates (and frees) the big FBO. Tunable target via /tmp/atlas_mediad_shrink "WxH" (default 256x256). */
void BrowserPageWPE::mediadFreeSurface()
{
    if (!m_viewBackend || !m_webView) return;
    if (m_mediadSavedW == 0) { m_mediadSavedW = m_renderWidth > 0 ? m_renderWidth : 1024;
                               m_mediadSavedH = m_renderHeight > 0 ? m_renderHeight : 3072; }
    int sw = 256, sh = 256;
    FILE* f = fopen("/tmp/atlas_mediad_shrink", "r");
    if (f) { if (fscanf(f, "%dx%d", &sw, &sh) != 2) { sw = 256; sh = 256; } fclose(f); }
    if (sw < 16) sw = 16; if (sh < 16) sh = 16;
    MLOG("mediad: freeing render surface %dx%d -> %dx%d (release pmem_smipool)", m_mediadSavedW, m_mediadSavedH, sw, sh);
    wpe_atlas_view_backend_resize(m_viewBackend, (uint32_t)sw, (uint32_t)sh);
    m_virtualWindowWidth = sw; m_virtualWindowHeight = sh;
    m_renderWidth = sw; m_renderHeight = sh; m_screenWidth = sw;
    m_renderedY = 0; m_scrollY = 0; m_renderPending = false;
    /* force the compositor to re-render at the new (small) size so the old big FBO is destroyed -> pmem freed */
    webkit_web_view_evaluate_javascript(m_webView, "window.scrollTo(0,1);window.scrollTo(0,0);", -1, nullptr, nullptr, nullptr, nullptr, nullptr);
    updateContentsSize();
}

void BrowserPageWPE::mediadRestoreSurface()
{
    if (!m_viewBackend || m_mediadSavedW == 0) return;
    int rw = m_mediadSavedW, rh = m_mediadSavedH;
    MLOG("mediad: restoring render surface -> %dx%d", rw, rh);
    wpe_atlas_view_backend_resize(m_viewBackend, (uint32_t)rw, (uint32_t)rh);
    m_virtualWindowWidth = rw; m_virtualWindowHeight = rh;
    m_renderWidth = rw; m_renderHeight = rh; m_screenWidth = rw;
    m_renderPending = false;
    m_mediadSavedW = m_mediadSavedH = 0;
    if (m_webView) webkit_web_view_evaluate_javascript(m_webView, "window.scrollTo(0,0)", -1, nullptr, nullptr, nullptr, nullptr, nullptr);
    updateContentsSize();
}

void BrowserPageWPE::onFsVideoInfo(GObject* obj, GAsyncResult* res, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    GError* err = nullptr;
    JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (bpwpe_dead(self)) { if (v) g_object_unref(v); if (err) g_error_free(err); return; }
    if (!v) { if (err) { MLOG("mediad: video-info JS failed: %s", err->message); g_error_free(err); } return; }
    char* s = jsc_value_to_string(v);
    g_object_unref(v);
    if (!s || !s[0]) { if (s) g_free(s); g_mediadBusy = false; MLOG("mediad: no fullscreen video src (blob/MSE or none) — abort handoff"); return; }
    /* split "src|time" on the LAST '|' (URLs can't contain a bare '|' but be safe) */
    char* bar = strrchr(s, '|');
    double t = 0; std::string src;
    if (bar) { *bar = '\0'; t = atof(bar + 1); src = s; }
    else src = s;
    g_free(s);
    if (src.compare(0, 5, "blob:") == 0 || src.compare(0, 11, "mediasource") == 0) {
        g_mediadBusy = false; MLOG("mediad: src is %s — cannot hand off (no direct URL)", src.c_str()); return; }
    self->m_mediadSrc = src;
    self->m_mediadResumeTime = t;
    self->m_mediadActive = true;   /* claim the handoff now so a second trigger no-ops */
    /* Free our GL FBO's pmem_smipool so mediad's decoder+overlay can allocate from the SMI pool. */
    self->mediadFreeSurface();
    /* Defer the mediad session-open so our in-browser gst pipeline finishes releasing the single HW VIDC
     * decoder AND the WebProcess frees the shrunken FBO's pmem before mediad tries to acquire them. */
    MLOG("mediad: video+surface released; opening session in 900ms for src=%s t=%.1f", src.c_str(), t);
    g_timeout_add(900, &BrowserPageWPE::mediadOpenSession, self);
}

/* the appId to forge via LSCallFromApplication — default our app; overridable by writing an appId into
 * /tmp/atlas_mediad_appid (empty file = use default; presence also enables the forge path). */
static const char* mediad_appid()
{
    static char buf[128];
    FILE* f = fopen("/tmp/atlas_mediad_appid", "r");
    if (f) { size_t n = fread(buf, 1, sizeof(buf)-1, f); fclose(f); buf[n]=0;
        char* nl = strpbrk(buf, "\r\n "); if (nl) *nl = 0;
        if (buf[0]) return buf; }
    return "org.webosports.app.atlas";
}

gboolean BrowserPageWPE::mediadOpenSession(gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    if (bpwpe_dead(self) || !self->m_mediadActive) return G_SOURCE_REMOVE;
    /* Open the session on the NAMED handle; per-session control calls go on the ANON handle (mediadCall) —
     * mimics luna-send's two separate connections (session-owner vs controller). */
    LSHandle* h = BrowserServer::instance() ? BrowserServer::instance()->getServiceHandle() : nullptr;
    if (!h) { MLOG("mediad: no LSHandle — abort"); self->m_mediadActive = false; g_mediadBusy = false; return G_SOURCE_REMOVE; }
    LSError e; LSErrorInit(&e);
    /* appId forge (LSCallFromApplication) needs a privileged role; opt in via /tmp/atlas_mediad_appid.
     * Default: plain LSCall — the session opens either way; mediad may still punch the HW overlay at the
     * absolute setVideoBounds screen coords without the app-card association. */
    bool ok;
    if (access("/tmp/atlas_mediad_appid", F_OK) == 0)
        ok = LSCallFromApplication(h, "luna://com.palm.mediad/service/playback", "{}",
                                   mediad_appid(),
                                   &BrowserPageWPE::onMediadPlayback, self, &self->m_mediadSubToken, &e);
    else
        ok = LSCall(h, "luna://com.palm.mediad/service/playback", "{}",
                    &BrowserPageWPE::onMediadPlayback, self, &self->m_mediadSubToken, &e);
    if (!ok) { MLOG("mediad: playback LSCall failed: %s", e.message ? e.message : "?"); LSErrorFree(&e); self->m_mediadActive = false; g_mediadBusy = false; }
    return G_SOURCE_REMOVE;
}

/* /service/playback reply: {"returnValue":true,"location":"palm://com.palm.mediad.MediaPlayer_N/"} */
bool BrowserPageWPE::onMediadPlayback(LSHandle* /*sh*/, LSMessage* msg, void* ctx)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ctx);
    const char* p = LSMessageGetPayload(msg);
    if (bpwpe_dead(self) || !p) return true;
    /* extract the per-session service name between "palm://" and the trailing "/" */
    const char* loc = strstr(p, "palm://");
    if (!loc) { MLOG("mediad: playback reply had no location: %s", p); return true; }
    loc += 7;
    const char* end = strchr(loc, '/'); if (!end) end = loc + strlen(loc);
    std::string svc(loc, end - loc);
    if (svc.empty()) { MLOG("mediad: empty location"); return true; }
    if (!self->m_mediadLoc.empty()) return true;   /* already handled the first reply */
    self->m_mediadLoc = svc;
    MLOG("mediad: session service = %s", svc.c_str());
    /* Fire load NOW (held subscription); defer bounds/fit/play until load has had time to build the pipeline
     * (mediad load is async — commands sent before it settles are dropped). */
    char pl[1024];
    snprintf(pl, sizeof(pl), "{\"source\":\"%s\",\"audioClass\":\"kAudioStreamMedia\"}", self->m_mediadSrc.c_str());
    self->mediadCall("load", pl);
    /* diag: /tmp/atlas_mediad_loadonly fires ONLY load (no bounds/fit/play) to check if load alone decodes */
    if (access("/tmp/atlas_mediad_loadonly", F_OK) == 0) { MLOG("mediad: load-only mode (skipping bounds/fit/play)"); return true; }
    g_timeout_add(1000, &BrowserPageWPE::mediadFirePlay, self);
    return true;
}

gboolean BrowserPageWPE::mediadFirePlay(gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    if (bpwpe_dead(self) || !self->m_mediadActive || self->m_mediadLoc.empty()) return G_SOURCE_REMOVE;
    /* now that load has built the pipeline: place the overlay, fill it, seek, and play */
    char pl[256];
    int vx=0, vy=0, vw = self->m_windowWidth >= 320 ? self->m_windowWidth : 1024, vh = 768;
    FILE* f = fopen("/tmp/atlas_vbounds", "r");
    if (f) { if (fscanf(f, "%d,%d,%d,%d", &vx,&vy,&vw,&vh) != 4) { vx=0;vy=0; } fclose(f); }
    snprintf(pl, sizeof(pl), "{\"videoBounds\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}}", vx,vy,vw,vh);
    self->mediadCall("setVideoBounds", pl);
    self->mediadCall("setFitMode", "{\"fitMode\":\"VIDEO_FILL\"}");
    if (self->m_mediadResumeTime > 1.0) {
        snprintf(pl, sizeof(pl), "{\"position\":%d}", (int)(self->m_mediadResumeTime * 1000));
        self->mediadCall("seek", pl);
    }
    self->mediadCall("play", "{}");
    return G_SOURCE_REMOVE;
}

/* fire a per-session control method (one-reply) as the Atlas app */
void BrowserPageWPE::mediadCall(const char* method, const char* payload)
{
    if (m_mediadLoc.empty()) return;
    LSHandle* h = BrowserServer::instance() ? BrowserServer::instance()->getMediaHandle() : nullptr;
    if (!h) return;
    char uri[256]; snprintf(uri, sizeof(uri), "luna://%s/%s", m_mediadLoc.c_str(), method);
    MLOG("mediad -> %s %s", uri, payload);
    LSError e; LSErrorInit(&e);
    /* HELD subscription (LSCall, not OneReply): mediad ties the media pipeline to the call's subscription —
     * a OneReply auto-cancel tears the pipeline down before load progresses. Cancelled in mediadEnd. */
    LSMessageToken tok = 0;
    bool ok;
    if (access("/tmp/atlas_mediad_appid", F_OK) == 0)
        ok = LSCallFromApplication(h, uri, payload, mediad_appid(),
                                   &BrowserPageWPE::onMediadReply, this, &tok, &e);
    else
        ok = LSCall(h, uri, payload, &BrowserPageWPE::onMediadReply, this, &tok, &e);
    if (!ok) { MLOG("mediad: %s LSCall failed: %s", method, e.message ? e.message : "?"); LSErrorFree(&e); }
    else if (tok) m_mediadCallTokens.push_back(tok);
}

bool BrowserPageWPE::onMediadReply(LSHandle* /*sh*/, LSMessage* msg, void* ctx)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ctx);
    const char* p = LSMessageGetPayload(msg);
    if (!bpwpe_dead(self)) MLOG("mediad reply: %s", p ? p : "(null)");
    return true;
}

/* leave-fullscreen: unload, drop the subscription (frees the session), resume the in-browser <video> */
void BrowserPageWPE::mediadEnd()
{
    if (!m_mediadActive) return;
    MLOG("mediad: ending handoff (resume t=%.1f)", m_mediadResumeTime);
    if (!m_mediadLoc.empty()) mediadCall("unload", "{}");
    BrowserServer* bs = BrowserServer::instance();
    LSHandle* ctrl = bs ? bs->getMediaHandle()   : nullptr;  // control calls went out here
    LSHandle* sess = bs ? bs->getServiceHandle() : nullptr;  // /service/playback subscription went out here
    LSError e; LSErrorInit(&e);
    /* cancel the held control-call subscriptions (anon handle), then the session subscription (named handle) */
    for (LSMessageToken t : m_mediadCallTokens) if (ctrl && t) LSCallCancel(ctrl, t, &e);
    m_mediadCallTokens.clear();
    if (sess && m_mediadSubToken) {
        if (!LSCallCancel(sess, m_mediadSubToken, &e)) { MLOG("mediad: cancel failed: %s", e.message?e.message:"?"); LSErrorFree(&e); }
    }
    m_mediadSubToken = 0; m_mediadLoc.clear(); m_mediadActive = false; g_mediadBusy = false;
    mediadRestoreSurface();   /* bring our render surface back to full size */
    /* restore the in-browser video (we dropped its src to free the decoder) at the saved time */
    if (m_webView && !m_mediadSrc.empty()) {
        char js[512];
        snprintf(js, sizeof(js),
            "(function(){var vs=document.getElementsByTagName('video'),i,v;for(i=0;i<vs.length;i++){v=vs[i];"
            "if(!v.currentSrc){try{v.src='%s';v.load();v.currentTime=%.1f;v.play();}catch(e){}break;}}})()",
            m_mediadSrc.c_str(), m_mediadResumeTime);
        webkit_web_view_evaluate_javascript(m_webView, js, -1, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
    m_mediadSrc.clear();
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
    /* The adapter sends scroll in its CONTENT/zoomed space (screen px * mZoomLevel per document px). Convert
     * to document space by dividing by the fit baseline AND the pinch zoom (m_uiZoom). At no zoom this is the
     * fit baseline as before. */
    double cz = contentZoom() * m_uiZoom;
    if (cz > 0.0 && cz != 1.0) { cx = (int)(cx / cz); cy = (int)(cy / cz); }
    m_scrollX = cx; m_scrollY = cy;
    /* Predictive pan: scroll velocity (px/sec, signed) so recenterForScroll can re-render EARLIER and
     * biased in the scroll direction — the ~1.4s composite then lands before we reach the buffer edge. */
    { long nowms = _wlog_ms(); long dt = nowms - m_lastScrollMs; if (dt < 0) dt += 10000000;
      int dy = cy - m_lastScrollY;
      /* Require dt>=8ms: sub-frame gaps (adapter batching multiple positions in the same tick) otherwise
       * divide by ~1ms and yield absurd velocities (vel=430000) that poison the guard/lead math -> jumping.
       * Clamp to a human-plausible ±8000 px/s regardless. */
      m_scrollVel = (m_lastScrollMs && dt >= 8 && dt < 400) ? (int)((long)dy * 1000 / dt) : 0;
      if (m_scrollVel >  8000) m_scrollVel =  8000;
      if (m_scrollVel < -8000) m_scrollVel = -8000;
      /* Direction streak: consecutive same-direction moves (signed). The predictive lead only kicks in for
       * SUSTAINED scrolling (|streak|>=3); on a direction flip it resets to ±1, so oscillation gets no lead
       * -> the buffer stops swinging ~1500px on every up/down flip. */
      if (m_lastScrollMs && dy != 0) { int d = dy > 0 ? 1 : -1;
          if ((d > 0 && m_dirStreak > 0) || (d < 0 && m_dirStreak < 0)) m_dirStreak += d; else m_dirStreak = d; }
      m_lastScrollMs = nowms; m_lastScrollY = cy; }
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
    /* P1 full-page-once: the whole (scaled) page fits the tall buffer -> it's all rendered at renderedY=0,
     * so the adapter pans the ENTIRE page with ZERO re-render (legacy-smooth end to end). */
    if (m_pageHeight > 0 && m_pageHeight <= m_renderHeight) {
        if (m_renderedY != 0) {
            m_renderedY = 0;
            webkit_web_view_evaluate_javascript(m_webView, "window.scrollTo(0,0)", -1, nullptr, nullptr, nullptr, nullptr, nullptr);
        }
        return;
    }
    /* Windowed pan: the re-center / catch-up decision lives in one place, shared with onFrame. */
    WLOG("setScrollPosition %d,%d renderedY=%d", cx, cy, m_renderedY);
    recenterForScroll(cx, cy);
}

/* Windowed pan re-center + catch-up. cx,cy are DOCUMENT-space scroll. Within the tall buffer -> false
 * (adapter pans, no readback). Near/past the edge -> kick a strip-hinted re-render centred on it.
 * ALSO called from onFrame: the scroll that SETTLED while a render was in flight was gated by
 * m_renderPending, so when that frame lands we must re-check the latest scroll and follow it —
 * otherwise the buffer freezes wherever the last render left it and everything else shows white. */
/* Sticky-fling latch (BPWPE_STICKY). File-scope is fine: only one page scrolls at a time (same assumption as
 * the scroll-test/g_st machinery). Set on a fast fling start, cleared when motion settles. */
static bool g_flinging = false;
bool BrowserPageWPE::recenterForScroll(int cx, int cy, bool force)
{
    if (!m_webView || m_screenHeight <= 0) return false;
    if (m_pageHeight <= 0 || m_pageHeight <= m_renderHeight) return false;   /* full-page-once owns this */
    int screenH = m_screenHeight;
    int slack   = m_renderHeight - screenH; if (slack < 0) slack = 0;
    int absVel  = m_scrollVel < 0 ? -m_scrollVel : m_scrollVel;
    /* Predictive pan: the ~1.4s composite means a re-render must START well before the viewport reaches
     * the buffer edge, or it lands late and the content "jumps"/goes white. When scrolling fast, widen the
     * guard (trigger earlier) AND lead the buffer in the scroll direction so we pan INTO fresh content. */
    int guard   = screenH / 3;
    if (absVel > 1000) guard = (slack < screenH) ? slack / 2 : screenH;   /* fast: re-render earlier */
    bool inBuffer = (slack > 2 * guard) && (cy >= m_renderedY + guard) && (cy <= m_renderedY + slack - guard);
    if (inBuffer) { g_flinging = false; return false; }    /* adapter pans — no re-render, no readback (fling has settled inside the buffer -> drop the sticky latch) */
    /* SETTLE-RENDER (gated /tmp/atlas_settle): during a FAST flick, chasing the scroll just thrashes the
     * WebProcess (each re-render then stalls ~1-2.5s) and still can't keep up. Instead DEFER — let the
     * adapter pan the buffer it has (blank past the edge) — and (re)arm a one-shot timer that fires ONE
     * sharp re-render ~160ms after scrolling stops, on an un-thrashed WebProcess. force=true (the timer)
     * bypasses this to actually render the settled position. */
    static int s_settle = -1;
    if (s_settle < 0) s_settle = (access("/tmp/atlas_nosettle", F_OK) == 0) ? 0 : 1;   /* default ON; disable via /tmp/atlas_nosettle */
    /* DEFER velocity threshold (px/s). Tunable via BPWPE_SETTLE_VEL. Measured a real fast flick on-device:
     * the momentum COAST runs ~1500-2400 px/s — BELOW the old hardcoded 2500 — so the coast phase never
     * deferred and instead CHAINED 6-8 sequential ~1200ms renders walking down the page (~7s to "settle").
     * A flick outruns the ~2386px buffer slack once V*renderLatency > slack, i.e. ~V>2000 even before
     * memory-pressure stall; 1500 comfortably catches the whole coast so the flick DEFERS to ONE settle
     * render instead of a grind. Deliberate reading-scroll stays well under this, so it still pans live. */
    static int s_settleVel = -1;
    if (s_settleVel < 0) { const char* e = getenv("BPWPE_SETTLE_VEL"); s_settleVel = e ? atoi(e) : 1500; if (s_settleVel <= 0) s_settleVel = 1500; }
    /* STICKY SNAP-TO-DESTINATION (env BPWPE_STICKY, default OFF). A HARD fling — detected by its fast START
     * (absVel > s_flingVel, default 2500; that's reliably a fling, never deliberate scroll) — LATCHES
     * g_flinging and then DEFERS the ENTIRE coast regardless of the noisy/under-counted coast velocity, until
     * motion stops. Result: the fling snaps to its landing in ONE settle render instead of the buffer chasing
     * the coast in ~6 laggy steps; the coast shows blank/held past the buffer edge. The latch is cleared when
     * motion settles (onSettleTimer, or the inBuffer early-return). Only a clear fling START latches it, so
     * normal/deliberate scroll (which never starts >2500px/s) is completely unaffected. */
    static int s_sticky = -1;
    if (s_sticky < 0) s_sticky = getenv("BPWPE_STICKY") ? 1 : 0;
    static int s_flingVel = -1;
    if (s_flingVel < 0) { const char* e = getenv("BPWPE_FLING_VEL"); s_flingVel = e ? atoi(e) : 2500; if (s_flingVel < 800) s_flingVel = 2500; }
    if (s_sticky && !force && absVel > s_flingVel) g_flinging = true;
    if (s_settle && !force && (absVel > s_settleVel || (s_sticky && g_flinging))) {
        /* Max-defer safeguard: only defer if we re-rendered recently. During CONTINUOUS fast motion (e.g.
         * finger oscillating up/down) every event would otherwise re-arm the 160ms timer so it NEVER fires
         * -> the buffer freezes and the content jumps to a stale position. If it's been >450ms since the
         * last issued re-render, DON'T defer — force a re-render now (force=true -> centered, no lead swing)
         * to keep tracking. A genuine single flick (motion then stop) still defers + settles via the timer. */
        long sinceRender = _wlog_ms() - m_lastRenderMs; if (sinceRender < 0) sinceRender += 10000000;
        /* This threshold MUST exceed the actual render time, or it defeats itself. Measured a fling-to-bottom
         * on-device: each recenter render takes 2000-3000ms under memory pressure, but this was 1800ms — so
         * sinceRender crossed 1800 before the render even finished, the force path fired on EVERY event, and
         * the fling CHAINED ~11 overlapping renders (stale=3) instead of deferring. Raised to 3000 (env
         * BPWPE_RENDER_MS) so a slow render completes first and the fling defers to the settle timer. */
        static int s_renderMs = -1;
        if (s_renderMs < 0) { const char* e = getenv("BPWPE_RENDER_MS"); s_renderMs = e ? atoi(e) : 3000; if (s_renderMs < 800) s_renderMs = 3000; }
        if (sinceRender < s_renderMs) {
            if (m_settleTimer) g_source_remove(m_settleTimer);
            m_settleTimer = g_timeout_add(140, (GSourceFunc)&BrowserPageWPE::onSettleTimer, this);
            return false;                                  /* defer: settle timer renders when the flick stops */
        }
        /* max-defer: a very long continuous coast force-renders here to avoid a frozen buffer. If we're in a
         * sticky fling, KEEP the latch and re-arm the settle timer so we still snap to the landing when motion
         * finally stops (the cancel below is guarded by !g_flinging so it won't drop it). */
        if (s_sticky && g_flinging) {
            if (m_settleTimer) g_source_remove(m_settleTimer);
            m_settleTimer = g_timeout_add(140, (GSourceFunc)&BrowserPageWPE::onSettleTimer, this);
        }
        force = true;   /* deferred too long during continuous motion -> render now, CENTERED, keep tracking */
    }
    if (m_settleTimer && !g_flinging) { g_source_remove(m_settleTimer); m_settleTimer = 0; }   /* re-rendering now -> drop the pending settle (but keep it alive through a sticky fling) */
    if (m_renderPending) {
        long age = _wlog_ms() - m_renderPendingMs; if (age < 0) age += 10000000;
        /* Same calibration as the settle threshold: a slow render (2-3s under memory pressure) must NOT be
         * declared stale/lost prematurely, or we issue an OVERLAPPING re-render (the stale=3 in the log) and
         * chain the fling. onFrame clears m_renderPending the instant the frame arrives, so this only gates
         * genuinely-lost frames — safe to wait the full render budget. */
        static int s_renderMs2 = -1;
        if (s_renderMs2 < 0) { const char* e = getenv("BPWPE_RENDER_MS"); s_renderMs2 = e ? atoi(e) : 3000; if (s_renderMs2 < 800) s_renderMs2 = 3000; }
        if (age < s_renderMs2) return false;               /* a re-render is plausibly still in flight */
        WLOG("renderPending STALE %ldms -> force re-render", age);   /* frame lost -> never deadlock */
    }
    int lead = 0;
    bool sustained = (m_dirStreak >= 3 || m_dirStreak <= -3);   /* only lead when scrolling one way consistently */
    if (!force && sustained && absVel > 700) {
        /* Lead SCALES with speed instead of a fixed ~965px slug: a big fixed lead makes every moderate-scroll
         * re-center visibly JUMP ~965px ahead, while moderate scroll (which keeps up fine) needs little lead.
         * Full lead only kicks in for genuine fast flicks (where the buffer would otherwise be outrun). */
        int Lmax = slack / 2 - screenH / 3; if (Lmax < 0) Lmax = 0;
        /* Lead per (screenH) of velocity — smaller divisor = MORE lead = buffer biased further ahead.
         * Default 4000 (conservative). NOTE (2026-07-12): tried /2000 (~2x lead) to reduce sustained-flick
         * catchup — it BACKFIRED for real usage: biasing the buffer one way REDUCES the runway for the
         * OPPOSITE direction, and real browsing flicks reverse constantly (down-to-read, up-to-go-back), so
         * more lead made reversals hit a recenter SOONER = worse. Keep the lead small; env BPWPE_LEAD_DIV
         * tunes it if ever needed (higher=less lead=more symmetric runway, better for reversal-heavy use). */
        static int s_leadDiv = -1;
        if (s_leadDiv < 0) { const char* e = getenv("BPWPE_LEAD_DIV"); s_leadDiv = e ? atoi(e) : 4000; if (s_leadDiv < 400) s_leadDiv = 4000; }
        int L = (absVel * (screenH > 0 ? screenH : 686)) / s_leadDiv;
        if (L > Lmax) L = Lmax;
        lead = (m_scrollVel > 0) ? L : -L;
    }
    int newTop = cy - slack / 2 + lead; if (newTop < 0) newTop = 0;   /* bias the buffer toward the scroll direction */
    int maxTop = m_pageHeight - m_renderHeight;
    if (maxTop > 0 && newTop > maxTop) newTop = maxTop;
    if (newTop == m_renderedY) return false;               /* already centred here (clamp) — nothing to do */
    /* P2 strip readback: at 1:1, read back ONLY the newly-exposed strip + memmove the overlap. delta
     * from the OLD renderedY; skip the hint if it exceeds the buffer (no overlap -> full readback). */
    int delta = newTop - m_renderedY;   /* strip delta (plain path); confirm path uses m_pendingOldY in the cb */
    static int s_confirm = -1;
    if (s_confirm < 0) s_confirm = (access("/tmp/atlas_noconfirm", F_OK) == 0) ? 0 : 1;   /* default ON; disable via /tmp/atlas_noconfirm */
    m_renderPending = true; m_renderPendingMs = _wlog_ms(); m_lastRenderMs = m_renderPendingMs;
    char js[128];
    if (s_confirm) {
        /* CONFIRM mode: the eval RETURNS the ACTUAL clamp-corrected scroll; onScrollConfirm labels
         * m_renderedY with it + sends the strip hint (actual delta), so the buffer label matches where the
         * frame truly rendered — fixes the async-scrollTo mislabel jump. Same eval COUNT as the plain path
         * (no extra probe), so it shouldn't regress catch-up. m_renderedY optimistic here; corrected in cb. */
        m_pendingOldY = m_renderedY;
        m_renderedY = newTop;
        snprintf(js, sizeof(js), "window.scrollTo(%d,%d);Math.round(window.scrollY)", cx, newTop);
        webkit_web_view_evaluate_javascript(m_webView, js, -1, nullptr, nullptr, nullptr, &BrowserPageWPE::onScrollConfirm, this);
    } else {
        if (contentZoom() == 1.0 && (delta > 0 ? delta : -delta) < m_renderHeight) {
            int y0, sh;
            if (delta > 0) { y0 = m_renderHeight - delta; sh = delta; }   /* scrolled down: strip at bottom */
            else           { y0 = 0;                       sh = -delta; } /* scrolled up:   strip at top    */
            wpe_atlas_view_backend_scroll_hint(m_viewBackend, y0, sh, delta);
        }
        m_renderedY = newTop;
        snprintf(js, sizeof(js), "window.scrollTo(%d,%d)", cx, newTop);
        webkit_web_view_evaluate_javascript(m_webView, js, -1, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
    WLOG("recenter cx=%d cy=%d vel=%d lead=%d -> renderedY=%d delta=%d cf=%d", cx, cy, m_scrollVel, lead, newTop, delta, s_confirm);
    if (g_st.active) g_st.renders++;
    return true;
}
/* Settle timer: fired ~160ms after the last scroll while a fast flick was deferring re-render. The flick
 * has stopped, so force a single sharp re-render to the settled position (force=true bypasses the defer
 * and centers the buffer on cy — no lead). One-shot (returns G_SOURCE_REMOVE=0). */
int BrowserPageWPE::onSettleTimer(void* ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    if (g_livePages.find(self) == g_livePages.end()) return 0;
    self->m_settleTimer = 0;
    g_flinging = false;   /* motion stopped -> release the sticky-fling latch; the forced render below lands it */
    WLOG("settle: flick stopped -> render settled cy=%d", self->m_scrollY);
    self->recenterForScroll(self->m_scrollX, self->m_scrollY, /*force=*/true);
    return 0;   /* G_SOURCE_REMOVE — one-shot */
}
/* CONFIRM mode callback: 'actual' = window.scrollY AFTER window.scrollTo (clamp-corrected true scroll).
 * Label the buffer with it (not the requested newTop) so the adapter pan cancels -> no mislabel jump. The
 * strip hint uses the ACTUAL delta (from the pre-recenter top); if it lands after the backend readback the
 * ctrl-seq mismatch just yields a full readback (still correct). */
void BrowserPageWPE::onScrollConfirm(GObject* obj, GAsyncResult* res, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    if (g_livePages.find(self) == g_livePages.end()) return;
    GError* err = nullptr;
    JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (!v) { if (err) g_error_free(err); return; }
    int actual = (int)jsc_value_to_double(v);
    g_object_unref(v);
    if (actual < 0) actual = 0;
    int delta = actual - self->m_pendingOldY;
    if (self->contentZoom() == 1.0 && delta != 0 && (delta > 0 ? delta : -delta) < self->m_renderHeight) {
        int y0, sh;
        if (delta > 0) { y0 = self->m_renderHeight - delta; sh = delta; }
        else           { y0 = 0;                            sh = -delta; }
        wpe_atlas_view_backend_scroll_hint(self->m_viewBackend, y0, sh, delta);
    }
    self->m_renderedY = actual;
}

/* Partial-readback driver: periodically ask the DOM for the largest PLAYING <video>'s rect (viewport
 * CSS px). The backend then reads back ONLY that rect each frame instead of the full 1024x3072 buffer —
 * the difference between ~1 fps and playable video. Empty result → no video → active=0 (full readback).
 * getBoundingClientRect returns the full viewport in fullscreen, so the same poll covers fullscreen too.
 * evaluate_javascript-returns is the proven-safe ferry (the script-message channel crashes the BS). */
gboolean BrowserPageWPE::pollVideoRect(gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    if (bpwpe_dead(self)) return G_SOURCE_REMOVE;
    if (!self->m_webView || !self->m_viewBackend) return G_SOURCE_CONTINUE;
    static const char* kJs =
        "(function(){var vs=document.getElementsByTagName('video'),b=null,ba=0,i,v,r,a;"
        "for(i=0;i<vs.length;i++){v=vs[i];if(v.paused||v.ended||!v.videoWidth)continue;"
        "r=v.getBoundingClientRect();if(r.width<=0||r.height<=0)continue;"
        "if(r.bottom<=0||r.right<=0||r.top>=innerHeight||r.left>=innerWidth)continue;"
        "a=r.width*r.height;if(a>ba){ba=a;b=r;}}"
        "if(!b)return '';return Math.round(b.left)+','+Math.round(b.top)+','+Math.round(b.width)+','+Math.round(b.height);})()";
    webkit_web_view_evaluate_javascript(self->m_webView, kJs, -1, nullptr, nullptr, nullptr,
                                        &BrowserPageWPE::onVideoRect, self);
    return G_SOURCE_CONTINUE;
}

/* Result of pollVideoRect: "" → no playing video (active=0). Else "l,t,w,h" in CSS px → scale by
 * cz=contentZoom*uiZoom to FBO/device px (same space setScrollPosition uses), clamp to the buffer,
 * publish to the backend's damage-rect control block. */
void BrowserPageWPE::onVideoRect(GObject* obj, GAsyncResult* res, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    GError* err = nullptr;
    JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (bpwpe_dead(self)) { if (v) g_object_unref(v); if (err) g_error_free(err); return; }
    if (!v) { if (err) g_error_free(err); return; }
    char* s = jsc_value_to_string(v);
    g_object_unref(v);
    int l=0, t=0, w=0, h=0;
    bool active = (s && s[0] && sscanf(s, "%d,%d,%d,%d", &l, &t, &w, &h) == 4 && w > 0 && h > 0);
    if (s) g_free(s);
    if (!active) { wpe_atlas_view_backend_video_rect(self->m_viewBackend, 0,0,0,0, 0); return; }
    double cz = self->contentZoom() * self->m_uiZoom; if (cz <= 0.0) cz = 1.0;
    int x = (int)(l * cz), y = (int)(t * cz), rw = (int)(w * cz), rh = (int)(h * cz);
    /* clamp to the FBO/buffer (m_renderWidth x m_renderHeight) */
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (self->m_renderWidth  > 0 && x + rw > self->m_renderWidth)  rw = self->m_renderWidth  - x;
    if (self->m_renderHeight > 0 && y + rh > self->m_renderHeight) rh = self->m_renderHeight - y;
    if (rw <= 0 || rh <= 0) { wpe_atlas_view_backend_video_rect(self->m_viewBackend, 0,0,0,0, 0); return; }
    wpe_atlas_view_backend_video_rect(self->m_viewBackend, x, y, rw, rh, 1);
}

/* async JS: report the real page height so the adapter knows the scroll range */
void BrowserPageWPE::onContentHeight(GObject* obj, GAsyncResult* res, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    GError* err = nullptr;
    JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (bpwpe_dead(self)) { if (v) g_object_unref(v); if (err) g_error_free(err); return; }
    if (!v) { if (err) g_error_free(err); return; }
    int h = (int)jsc_value_to_double(v);
    g_object_unref(v);   /* 2.0: _finish returns an owned JSCValue (replaces WebKitJavascriptResult) */
    WLOG("content height = %d (virt %d)", h, self->m_virtualWindowHeight);
    /* Ignore the exact-viewport-height artifact: document.scrollHeight transiently returns the WebView
     * viewport height (== m_virtualWindowHeight) during reflow / subframe loads. Reporting it clobbers the
     * real page height and clamps the adapter's scroll to one buffer. Real heights are never exactly it. */
    if (h > 0 && h != self->m_virtualWindowHeight) {
        self->m_pageHeight = h;   /* P1: full-page-fits check in setScrollPosition */
        self->m_server->msgContentsSizeChanged(self->m_proxy, self->m_virtualWindowWidth, h);
    }
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
    if (bpwpe_dead(self)) { if (v) g_object_unref(v); if (err) g_error_free(err); return; }
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

/* ===================== autonomous scroll test (SIGUSR1) ===================== */
static gboolean scrolltest_tick(gpointer)
{
    if (!g_testablePage || g_livePages.find(g_testablePage) == g_livePages.end()) { g_st.active = false; return G_SOURCE_REMOVE; }
    return g_testablePage->scrollTestStep() ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}
static gboolean on_sigusr1(gpointer)
{
    if (g_testablePage && g_livePages.find(g_testablePage) != g_livePages.end() && !g_st.active)
        g_testablePage->startScrollTest();
    else
        WLOG("SIGUSR1: no testable page (or a test is already running)");
    return G_SOURCE_CONTINUE;   /* keep the handler for repeated runs */
}
/* File trigger: the host `touch`es /tmp/atlas_scrolltest; polled on the glib loop (which is clearly
 * dispatching — frames render), so it works even though the app-spawned BS has SIGUSR1 masked. */
static gboolean scrolltest_poll(gpointer)
{
    if (access("/tmp/atlas_scrolltest", F_OK) == 0) {
        unlink("/tmp/atlas_scrolltest");
        if (g_testablePage && g_livePages.find(g_testablePage) != g_livePages.end() && !g_st.active)
            g_testablePage->startScrollTest();
        else
            WLOG("scrolltest trigger: no testable page (or a test is already running)");
    }
    /* Rotation test: `touch /tmp/atlas_rotatetest` toggles the window between landscape (1024x686) and
     * portrait (768x942) — driving setWindowSize exactly like a real rotate, so the reflow/resize path
     * can be exercised + framebuffer-checked autonomously. */
    if (access("/tmp/atlas_rotatetest", F_OK) == 0) {
        unlink("/tmp/atlas_rotatetest");
        if (g_testablePage && g_livePages.find(g_testablePage) != g_livePages.end())
            g_testablePage->toggleRotationTest();
    }
    /* Fullscreen test: `touch /tmp/atlas_fstest` injects a synthetic click on the page body. That click is a
     * real user gesture from WebKit's view (dispatched via dispatchPointer), so Element.requestFullscreen()
     * is accepted (transient activation) — lets us trigger fullscreen for crash-testing WITHOUT a physical
     * tap. The test page (vidfs2.html) has a full-page body click handler that requests fullscreen. */
    if (access("/tmp/atlas_fstest", F_OK) == 0) {
        unlink("/tmp/atlas_fstest");
        // Click ALL live pages (the app launches a shell + the content page); the video page's body handler
        // fires requestFullscreen. Clicking the shell is harmless.
        int n = 0;
        for (BrowserPageWPE* p : g_livePages) { p->clickAt(300, 500, 1); n++; }
        WLOG("fstest: injected click at 300,500 on %d live page(s)", n);
    }
    /* Page scrape: `/tmp/atlas_scrape` holds a JS expression; run it in the live content page and write the
     * (stringified) result to /tmp/atlas_scrape_out. General tool for scraping loaded pages (html5test,
     * codec-probe results, etc.) from the host without a physical device. */
    if (access("/tmp/atlas_scrape", F_OK) == 0) {
        static char js[65536];
        FILE* jf = fopen("/tmp/atlas_scrape", "r");
        size_t jn = jf ? fread(js, 1, sizeof(js) - 1, jf) : 0; js[jn] = 0;
        if (jf) fclose(jf);
        unlink("/tmp/atlas_scrape");
        BrowserPageWPE* target = nullptr;
        for (BrowserPageWPE* p : g_livePages) target = p;   /* last live page = the content page */
        if (target && js[0]) target->runScrape(js);
        else WLOG("scrape: no live content page");
    }
    /* Direct mediad-handoff test: `touch /tmp/atlas_mediadgo` calls mediadBegin() on each live page WITHOUT
     * needing a fullscreen gesture (requestFullscreen needs transient activation the synthetic tap can't give).
     * Decouples testing the media handoff from the fullscreen-activation problem. */
    if (access("/tmp/atlas_mediadgo", F_OK) == 0) {
        unlink("/tmp/atlas_mediadgo");
        int n = 0;
        for (BrowserPageWPE* p : g_livePages) { p->mediadBegin(); n++; }
        MLOG("mediadgo: triggered mediadBegin on %d live page(s)", n);
    }
    return G_SOURCE_CONTINUE;
}
/* Simulate a device rotation by toggling the window size portrait<->landscape (drives setWindowSize). */
void BrowserPageWPE::toggleRotationTest()
{
    bool wide = m_windowWidth >= 1000;
    if (wide) setWindowSize(768, 942);
    else      setWindowSize(1024, 686);
    WLOG("rotatetest -> %s", wide ? "portrait 768x942" : "landscape 1024x686");
}
/* The scroll test must sweep the WHOLE page (past the buffer edge -> exercises the recenter/re-render
 * path), not just within the buffer. m_pageHeight can be 0/stale at trigger time (the async content-
 * height query may not have run for this load), which made the old test fall back to the buffer height
 * (m_virtualWindowHeight) and pan only in-buffer — the trivially-easy case. Re-measure the real DOM
 * height NOW, then begin the sweep in the callback so maxCy spans the full page. */
void BrowserPageWPE::startScrollTest()
{
    if (!m_webView) { WLOG("scrolltest: no webview"); return; }
    webkit_web_view_evaluate_javascript(m_webView,
        "Math.max(document.documentElement.scrollHeight, document.body?document.body.scrollHeight:0)",
        -1, nullptr, nullptr, nullptr, &BrowserPageWPE::onScrollTestHeight, this);
}
void BrowserPageWPE::onScrollTestHeight(GObject* obj, GAsyncResult* res, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    GError* err = nullptr;
    JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (bpwpe_dead(self)) { if (v) g_object_unref(v); if (err) g_error_free(err); return; }
    if (v) { int h = (int)jsc_value_to_double(v); if (h > 0) self->m_pageHeight = h; g_object_unref(v); }
    if (err) g_error_free(err);
    WLOG("scrolltest: measured real pageHeight=%d (virt %d, renderH %d) before sweep",
         self->m_pageHeight, self->m_virtualWindowHeight, self->m_renderHeight);
    self->beginScrollTest();
}
void BrowserPageWPE::beginScrollTest()
{
    int pageH = m_pageHeight > 0 ? m_pageHeight : m_virtualWindowHeight;
    int maxCy = pageH - (m_screenHeight > 0 ? m_screenHeight : 768);
    if (maxCy < 1) { WLOG("SCROLLTEST abort: page too short (pageH=%d screenH=%d)", pageH, m_screenHeight); return; }
    const char* es = getenv("BPWPE_TEST_STEP"); const char* em = getenv("BPWPE_TEST_MS");
    g_st.step = es ? atoi(es) : 120; if (g_st.step < 1) g_st.step = 120;
    int ms    = em ? atoi(em) : 50;  if (ms < 5) ms = 50;
    /* /tmp/atlas_test.cfg ("<stepPx> <intervalMs>") overrides env, so the host can vary scroll speed
     * per run (slow drag vs fast flick) without relaunching the BS. */
    FILE* cf = fopen("/tmp/atlas_test.cfg", "r");
    if (cf) { int s = 0, m = 0; if (fscanf(cf, "%d %d", &s, &m) == 2) { if (s > 0) g_st.step = s; if (m >= 5) ms = m; } fclose(cf); }
    g_st.active = true; g_st.cy = 0; g_st.dir = 1; g_st.maxCy = maxCy;
    g_st.steps = g_st.renders = g_st.uncovered = 0; g_st.maxStallMs = 0;
    WLOG("SCROLLTEST START maxCy=%d step=%d ms=%d pageH=%d screenH=%d renderH=%d deliveredY=%d",
         maxCy, g_st.step, ms, pageH, m_screenHeight, m_renderHeight, m_deliveredY);
    /* Unconditional result file (WLOG needs BPWPE_DEBUG, which the app-spawned BS lacks). */
    FILE* rf = fopen("/tmp/atlas_scrolltest.result", "w");
    if (rf) { fprintf(rf, "START maxCy=%d step=%d ms=%d pageH=%d screenH=%d renderH=%d bgra?\n",
                      maxCy, g_st.step, ms, pageH, m_screenHeight, m_renderHeight); fclose(rf); }
    g_st.tick = g_timeout_add(ms, scrolltest_tick, nullptr);
}
bool BrowserPageWPE::scrollTestStep()
{
    if (!g_st.active) return false;
    setScrollPosition(0, g_st.cy, 0, 0);         /* drive one scroll exactly like the adapter */
    g_st.steps++;
    /* coverage: is the viewport [cy, cy+screenH] fully inside the buffer the adapter is SHOWING
     * ([m_deliveredY, +m_renderHeight])? If not, the user sees white/partial at this instant. */
    if (m_screenHeight > 0 &&
        !(g_st.cy >= m_deliveredY && g_st.cy + m_screenHeight <= m_deliveredY + m_renderHeight))
        g_st.uncovered++;
    g_st.cy += g_st.dir * g_st.step;
    if (g_st.cy >= g_st.maxCy) { g_st.cy = g_st.maxCy; g_st.dir = -1; }
    else if (g_st.cy <= 0 && g_st.dir < 0) { g_st.cy = 0; setScrollPosition(0, 0, 0, 0); reportScrollTest(); g_st.active = false; return false; }
    return true;
}
void BrowserPageWPE::reportScrollTest()
{
    int uncoveredPct = g_st.steps ? (g_st.uncovered * 100 / g_st.steps) : 0;
    WLOG("SCROLLTEST DONE steps=%d renders=%d uncovered=%d(%d%%) maxStall=%ldms finalRenderedY=%d finalDeliveredY=%d maxCy=%d",
         g_st.steps, g_st.renders, g_st.uncovered, uncoveredPct, g_st.maxStallMs, m_renderedY, m_deliveredY, g_st.maxCy);
    FILE* rf = fopen("/tmp/atlas_scrolltest.result", "a");
    if (rf) { fprintf(rf, "DONE steps=%d renders=%d uncovered=%d(%d%%) maxStall=%ldms finalRenderedY=%d finalDeliveredY=%d maxCy=%d\n",
                      g_st.steps, g_st.renders, g_st.uncovered, uncoveredPct, g_st.maxStallMs, m_renderedY, m_deliveredY, g_st.maxCy); fclose(rf); }
}
void BrowserPageWPE::setZoomAndScroll(double zoom, int sx, int sy)
{
    if (!m_webView) return;
    /* Pinch zoom is done by the ADAPTER scaling the fit-baseline buffer (fast, live, anchored to the pinch
     * centre). The engine must NOT also apply a webkit zoom — that would double the zoom AND force a slow
     * full re-render. We just record the UI zoom (so the zoomed scroll the adapter sends converts to the
     * right document position) and apply the anchored scroll so the region we keep rendered matches the
     * zoomed viewport. At zoom==1 this is a plain scroll, so normal browsing is unchanged. */
    m_uiZoom = (zoom > 0.0) ? zoom : 1.0;
    /* Crisp re-render: zoom WebKit so the buffer is genuinely RENDERED at the target scale (not just the
     * adapter's nearest-neighbor upscale of the fit buffer, which is pixelated). fit = our fixed downscale
     * baseline; wk maps the content zoom onto WebKit's layout scale. onFrame then reports contentZoom =
     * fit*wk (the buffer's TRUE scale), so the adapter's scaled blit divides back to 1:1 = SHARP. During the
     * pinch the engine isn't re-rendered (only on release/settle), so the adapter scales the old buffer live;
     * on release this snaps it crisp. Clamp wk>=1 so we never render below the fit baseline. */
    double fit = contentZoom();
    double wk = (fit > 0.0) ? (m_uiZoom / fit) : m_uiZoom;
    if (wk < 1.0) wk = 1.0;
    m_webkitZoom = wk;
    WLOG("setZoomAndScroll uiZoom=%.3f fit=%.3f -> webkitZoom=%.3f scroll=%d,%d", m_uiZoom, fit, wk, sx, sy);
    webkit_web_view_set_zoom_level(m_webView, wk);
    setScrollPosition(sx, sy, 0, 0);
}

/* WebKit told us an editable element gained/lost focus (via the IM context). Map the field purpose
 * to the Atlas field type and signal the adapter, which raises/hides the webOS virtual keyboard
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
    if (bpwpe_dead(self)) { if (v) g_object_unref(v); if (err) g_error_free(err); return; }
    if (!v) { if (err) g_error_free(err); return; }
    int code = (int)jsc_value_to_int32(v);
    g_object_unref(v);
    self->onEditorFocus(code >= 0, code >= 0 ? code : 0);
}

/* Autofill: fill the page's login form in ONE JS pass so the user never has to tap each field (the slow
 * part on heavy pages). payload = "\x02" username "\x02" password (routed via insertStringAtCursor). We set
 * the password field + its form's first text/email field, dispatching input+change so the site's JS reacts.
 * No-op if the page has no password field (so triggering on any site is safe). */
static std::string atlas_js_escape(const char* s) {
    std::string o;
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            case '<':  o += "\\x3c"; break;   /* avoid </script> style breakouts, just in case */
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\x%02x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}
void BrowserPageWPE::fillLoginForm(const char* payload)
{
    if (!m_webView || !payload || payload[0] != '\x02') return;
    const char* u = payload + 1;
    const char* sep = strchr(u, '\x02');
    if (!sep) return;
    std::string user(u, sep - u);
    std::string pass(sep + 1);
    std::string js =
        "(function(u,p){"
        "var pw=document.querySelector('input[type=password]');"
        "if(!pw)return;"
        "var set=function(el,v){try{el.focus();}catch(e){}el.value=v;"
        "el.dispatchEvent(new Event('input',{bubbles:true}));"
        "el.dispatchEvent(new Event('change',{bubbles:true}));};"
        "var scope=pw.form||document;var list=scope.querySelectorAll('input');var us=null;"
        "for(var i=0;i<list.length;i++){if(list[i]===pw)break;"
        "var t=(list[i].type||'text').toLowerCase();"
        "if(t==='text'||t==='email'||t===''){us=list[i];}}"
        "if(us)set(us,u);"
        "set(pw,p);"
        "})(\"" + atlas_js_escape(user.c_str()) + "\",\"" + atlas_js_escape(pass.c_str()) + "\")";
    WLOG("fillLoginForm: user=%zu pass=%zu chars", user.size(), pass.size());
    webkit_web_view_evaluate_javascript(m_webView, js.c_str(), -1, nullptr, nullptr, nullptr, nullptr, nullptr);
}

/* Double-tap zoom: find the block-level element under the tap and report its rect so the adapter can
 * zoom to fit it (msgSmartZoomCalculateResponseSimple — the legacy double-tap path). Async via
 * evaluate_javascript; coords map content↔viewport through the tracked scroll (m_scrollX/Y). */
void BrowserPageWPE::smartZoomCalculate(uint32_t pointX, uint32_t pointY)
{
    if (!m_webView) return;
    m_smartZoomX = (int)pointX; m_smartZoomY = (int)pointY;
    /* content → viewport for elementFromPoint: subtract WebKit's ACTUAL scroll (m_renderedY in the pan
     * model), NOT the adapter pan (m_scrollY) — same fix as dispatchPointer. Using m_scrollY hit-tested the
     * wrong spot whenever the page was panned within the buffer, so double-tap zoom fired only intermittently. */
    int vx = (int)pointX - m_scrollX, vy = (int)pointY - m_renderedY;
    char js[640];
    snprintf(js, sizeof(js),
        "(function(){var e=document.elementFromPoint(%d,%d);"
        "if(!e||e.tagName==='HTML'||e.tagName==='BODY')return '0,0,0,0';"
        /* walk up out of inline boxes to the containing block */
        "while(e.parentElement&&getComputedStyle(e).display==='inline')e=e.parentElement;"
        /* climb tiny/degenerate wrappers to a meaningfully-sized block, but stop at BODY/HTML */
        "while(e.parentElement&&e.parentElement.tagName!=='BODY'&&e.parentElement.tagName!=='HTML'){"
        "var pr=e.parentElement.getBoundingClientRect();if(pr.width>=8&&pr.height>=8&&pr.width<=e.getBoundingClientRect().width*1.05)break;"
        "if(e.getBoundingClientRect().width>=8&&e.getBoundingClientRect().height>=8)break;e=e.parentElement;}"
        "if(e.tagName==='HTML'||e.tagName==='BODY')return '0,0,0,0';"
        "var r=e.getBoundingClientRect();if(r.width<8||r.height<8)return '0,0,0,0';"
        "var sx=window.scrollX,sy=window.scrollY;"
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
    if (bpwpe_dead(self)) { if (v) g_object_unref(v); if (err) g_error_free(err); return; }
    if (!v) { if (err) g_error_free(err); return; }
    char* s = jsc_value_to_string(v);
    int l = 0, t = 0, r = 0, b = 0;
    if (s) sscanf(s, "%d,%d,%d,%d", &l, &t, &r, &b);
    g_free(s); g_object_unref(v);
    if (self->m_server)
        self->m_server->msgSmartZoomCalculateResponseSimple(self->m_proxy, self->m_smartZoomX, self->m_smartZoomY, l, t, r, b, 0);
}

/* ---- Feature 9: context-menu hit-test + save-image (async; WPE has no sync hit-test) ---------------- */
void BrowserPageWPE::hitTestAsync(int32_t queryNum, int32_t cx, int32_t cy)
{
    m_hitTestQuery = queryNum;
    if (!m_webView) { if (m_server) m_server->msgHitTestResponse(m_proxy, queryNum, "{\"isNull\":true,\"x\":0,\"y\":0}"); return; }
    int vx = cx - m_scrollX, vy = cy - m_renderedY;   /* document -> viewport (WebKit's real scroll = m_renderedY) */
    WLOG("hitTest q=%d doc(%d,%d) -> view(%d,%d) scrollX=%d renderedY=%d", queryNum, cx, cy, vx, vy, m_scrollX, m_renderedY);
    char js[2400];
    snprintf(js, sizeof js,
        "(function(x,y){"
        /* stacked elements at the point so we see images/links UNDER overlays and link wrappers */
        "var st=(document.elementsFromPoint?document.elementsFromPoint(x,y):[document.elementFromPoint(x,y)]).filter(Boolean);"
        "if(!st.length)return JSON.stringify({isNull:true,x:x,y:y});var e=st[0];"
        /* link: nearest ancestor A[href] of anything in the stack */
        "var lk=null;for(var i=0;i<st.length&&!lk;i++){var p=st[i];while(p){if(p.tagName==='A'&&p.getAttribute('href')){lk=p;break;}p=p.parentElement;}}"
        /* image: an IMG in the stack (or a descendant), else the first CSS background-image up the tree */
        "var im=null,bg='';for(var j=0;j<st.length&&!im;j++){if(st[j].tagName==='IMG'){im=st[j];break;}}"
        "if(!im){for(var k=0;k<st.length&&!bg;k++){var d=st[k];while(d&&d.nodeType===1){var cs=getComputedStyle(d);var bi=cs&&cs.backgroundImage;if(bi&&bi.indexOf('url(')>-1){var m=bi.match(/url\\((\"|')?([^\"')]+)/);if(m){bg=m[2];break;}}d=d.parentElement;}}}"
        "var ed=false,c=e;while(c){var t=c.tagName;if(t==='INPUT'||t==='TEXTAREA'||c.isContentEditable){ed=true;break;}c=c.parentElement;}"
        "var r={isNull:false,isLink:!!lk,isImage:!!(im||bg),element:(e.tagName||'').toUpperCase(),editable:ed,x:%d,y:%d};"
        "if(lk){r.linkUrl=lk.href;r.linkText=(lk.textContent||'').replace(/\\s+/g,' ').trim().slice(0,200);r.linkTitle=lk.getAttribute('title')||'';}"
        "if(im){r.imageUrl=im.currentSrc||im.src;r.altText=im.getAttribute('alt')||'';}else if(bg){r.imageUrl=bg;r.altText='';}"
        "var bt=(im||e),b=bt.getBoundingClientRect();r.bounds={left:Math.round(b.left),top:Math.round(b.top),right:Math.round(b.right),bottom:Math.round(b.bottom)};"
        "return JSON.stringify(r);})(%d,%d)",
        cx, cy, vx, vy);
    webkit_web_view_evaluate_javascript(m_webView, js, -1, nullptr, nullptr, nullptr, &BrowserPageWPE::onHitTestResult, this);
}
void BrowserPageWPE::onHitTestResult(GObject* obj, GAsyncResult* res, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    GError* err = nullptr;
    JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (bpwpe_dead(self)) { if (v) g_object_unref(v); if (err) g_error_free(err); return; }
    std::string json = "{\"isNull\":true,\"x\":0,\"y\":0}";
    if (v) { char* s = jsc_value_to_string(v); if (s) { json = s; g_free(s); } g_object_unref(v); }
    else if (err) { WLOG("hitTest JS error: %s", err->message ? err->message : "?"); g_error_free(err); }
    WLOG("hitTest q=%d result: %s", self->m_hitTestQuery, json.c_str());
    if (self->m_server) self->m_server->msgHitTestResponse(self->m_proxy, self->m_hitTestQuery, json.c_str());
}

/* Save-image: 2-step async — JS to get the <img> src at the point, then download it (modern TLS) into the
 * requested dir, then respond. State lives in a heap struct freed on finish/fail (like the favicon dl). */
struct AtlasImgDl { BrowserPageWPE* page; int query; std::string dest; gulong hFin, hFail; };
static void atlas_img_dl_done(WebKitDownload* dl, AtlasImgDl* s, bool ok);
static void atlas_img_dl_finished(WebKitDownload* dl, gpointer ud) { atlas_img_dl_done(dl, static_cast<AtlasImgDl*>(ud), true); }
static void atlas_img_dl_failed(WebKitDownload* dl, GError* e, gpointer ud) {
    WLOG("saveImg FAILED: %s", (e && e->message) ? e->message : "?");
    atlas_img_dl_done(dl, static_cast<AtlasImgDl*>(ud), false);
}
static gboolean atlas_img_decide_dest(WebKitDownload* dl, gchar*, gpointer ud) {
    webkit_download_set_destination(dl, static_cast<AtlasImgDl*>(ud)->dest.c_str()); return TRUE;
}

void BrowserPageWPE::saveImageAtPointAsync(int32_t queryNum, int32_t x, int32_t y, const char* dir)
{
    m_saveImgQuery = queryNum;
    m_saveImgDir = (dir && *dir) ? dir : "/tmp";
    if (!m_webView) { if (m_server) m_server->msgSaveImageAtPointResponse(m_proxy, queryNum, false, ""); return; }
    int vx = x - m_scrollX, vy = y - m_renderedY;
    char js[900];
    snprintf(js, sizeof js,
        "(function(x,y){"
        "var st=(document.elementsFromPoint?document.elementsFromPoint(x,y):[document.elementFromPoint(x,y)]).filter(Boolean);"
        "for(var j=0;j<st.length;j++){var s=st[j];if(s.tagName==='IMG')return s.currentSrc||s.src;var q=s.querySelector&&s.querySelector('img');if(q)return q.currentSrc||q.src;}"
        "for(var k=0;k<st.length;k++){var d=st[k];while(d&&d.nodeType===1){var bi=getComputedStyle(d).backgroundImage;if(bi&&bi.indexOf('url(')>-1){var m=bi.match(/url\\((\"|')?([^\"')]+)/);if(m)return m[2];}d=d.parentElement;}}"
        "return '';})(%d,%d)", vx, vy);
    webkit_web_view_evaluate_javascript(m_webView, js, -1, nullptr, nullptr, nullptr, &BrowserPageWPE::onSaveImgSrcResult, this);
}
void BrowserPageWPE::onSaveImgSrcResult(GObject* obj, GAsyncResult* res, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    GError* err = nullptr;
    JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (bpwpe_dead(self)) { if (v) g_object_unref(v); if (err) g_error_free(err); return; }
    std::string src;
    if (v) { char* s = jsc_value_to_string(v); if (s) { src = s; g_free(s); } g_object_unref(v); }
    else if (err) g_error_free(err);
    if (src.empty() || !self->m_webView) {
        if (self->m_server) self->m_server->msgSaveImageAtPointResponse(self->m_proxy, self->m_saveImgQuery, false, "");
        return;
    }
    /* dest filename from the URL basename (or a fixed name for data: URIs) */
    std::string leaf;
    if (src.compare(0, 5, "data:") == 0) leaf = "atlas-image.png";
    else {
        size_t qm = src.find('?'); std::string clean = (qm == std::string::npos) ? src : src.substr(0, qm);
        size_t sl = clean.rfind('/'); leaf = (sl == std::string::npos) ? clean : clean.substr(sl + 1);
        if (leaf.empty()) leaf = "atlas-image";
    }
    mkdir(self->m_saveImgDir.c_str(), 0755);
    std::string dest = self->m_saveImgDir + "/" + leaf;
    /* A duplicate request (menu tap double-fires) would start a 2nd download onto the SAME file;
     * the two collide and one fails — and the failure can respond first. Drop the duplicate. */
    if (self->m_saveImgInFlight == dest) {
        WLOG("saveImg q=%d DROP duplicate (already downloading %s)", self->m_saveImgQuery, dest.c_str());
        return;
    }
    self->m_saveImgInFlight = dest;
    unlink(dest.c_str());   /* WebKitDownload can "failed" if the destination already exists — clear it first */
    WLOG("saveImg q=%d src=%s dest=%s", self->m_saveImgQuery, src.c_str(), dest.c_str());
    WebKitDownload* dl = webkit_web_view_download_uri(self->m_webView, src.c_str());
    if (!dl) { WLOG("saveImg: download_uri returned null"); if (self->m_server) self->m_server->msgSaveImageAtPointResponse(self->m_proxy, self->m_saveImgQuery, false, ""); return; }
    AtlasImgDl* s = new AtlasImgDl();
    s->page = self; s->query = self->m_saveImgQuery; s->dest = dest;
    /* WPE 2.40+: decide-destination sets a PATH (not a URI) and returns TRUE. Don't pre-set. */
    g_signal_connect(dl, "decide-destination", G_CALLBACK(atlas_img_decide_dest), s);
    s->hFin  = g_signal_connect(dl, "finished", G_CALLBACK(atlas_img_dl_finished), s);
    s->hFail = g_signal_connect(dl, "failed",   G_CALLBACK(atlas_img_dl_failed),   s);
}
void BrowserPageWPE::respondSaveImage(int32_t query, bool ok, const char* path)
{
    m_saveImgInFlight.clear();   // this download finished (ok or fail) — allow a future save of the same file
    if (m_server && m_proxy) m_server->msgSaveImageAtPointResponse(m_proxy, query, ok, ok ? path : "");
}
static void atlas_img_dl_done(WebKitDownload* dl, AtlasImgDl* s, bool ok)
{
    WLOG("saveImg done q=%d ok=%d dest=%s", s->query, ok, s->dest.c_str());
    if (s->page && !bpwpe_dead(s->page)) s->page->respondSaveImage(s->query, ok, s->dest.c_str());   // page may have closed mid-download
    g_signal_handler_disconnect(dl, s->hFin);
    g_signal_handler_disconnect(dl, s->hFail);
    g_object_unref(dl);
    delete s;
}

/* ---- input: dispatch on the wpe_view_backend (forwards to the WebProcess) ------------------- */
void BrowserPageWPE::dispatchPointer(int x, int y, uint32_t button, bool down)
{
    WLOG("dispatchPointer %d,%d btn=%u down=%d", x, y, button, down);
    m_userInteracted = true;   // a real tap on the page — subsequent editor focus may raise the VKB
    if (!m_viewBackend) return;
    /* NO tap scaling here. The offscreen now satisfies the legacy contract (buffer holds the page
     * already scaled by contentZoom), so the adapter has ALREADY divided the on-screen tap by
     * contentZoom before sending it — the coords arrive in DOCUMENT space, which equals our layout
     * space (m_virtualWindowWidth). WebKit's viewport is laid out at that same width, so we dispatch
     * as-is. (The earlier x*virtualW/screenW rescale double-transformed taps → they landed too low.)
     * The adapter hands document-ABSOLUTE coords; WebKit wants viewport-relative — subtract scroll. */
    /* Convert document-absolute -> viewport by subtracting WebKit's ACTUAL scroll. In the pan model that is
     * m_renderedY (the top of the region WebKit is scrolled to via window.scrollTo), NOT the adapter's visual
     * pan position m_scrollY. They diverge when a page fits the tall pan buffer: WebKit's scroll stays 0 while
     * the adapter pans down, so subtracting m_scrollY put clicks m_scrollY px too high — e.g. a one-screen
     * cookie-consent page where "Accept"/"Reject" never got hit. (X has no pan buffer, so m_scrollX is right.) */
    x -= m_scrollX; y -= m_renderedY;
    WLOG("  -> viewport %d,%d (scrollX %d renderedY %d panY %d)", x, y, m_scrollX, m_renderedY, m_scrollY);
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
    /* type: 0=move 1=down 2=up (Atlas BATypes). detail = button. contentX/Y are passed straight to
     * dispatchPointer — correct only while the page is unscrolled/unzoomed. Scroll tracking (m_scrollX/Y)
     * is wired now, so TODO: apply the content→view map here (subtract m_scrollX/Y, apply zoom) for taps
     * on scrolled/zoomed pages. */
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
    // The long-press RELEASE arrives here as a click at the SAME point ~a second after selectWordAt. Its
    // mousedown would collapse the fresh selection (and leave stale highlight). Swallow that one click so
    // the selection survives; a genuine tap elsewhere/later still falls through and dismisses.
    gint64 nowMs = g_get_monotonic_time() / 1000;
    if ((nowMs - m_lastSelMs) < 1800 &&
        (int)x - m_lastSelDocX < 40 && m_lastSelDocX - (int)x < 40 &&
        (int)y - m_lastSelDocY < 40 && m_lastSelDocY - (int)y < 40) {
        WLOG("clickAt swallowed (post-select release) at %u,%u", x, y);
        m_lastSelMs = 0;   // one-shot: only the immediate release is swallowed
        return true;
    }
    for (uint32_t i = 0; i < (numClicks ? numClicks : 1); ++i) {
        dispatchPointer((int)x, (int)y, 1, true);
        dispatchPointer((int)x, (int)y, 1, false);
    }
    checkEditorFocus();   // tap may have focused an editable element → raise/hide the VKB
    // A genuine tap dismisses any active text selection + its overlay markers.
    g_timeout_add(130, &BrowserPageWPE::onSelClearTimer, this);
    return true;
}
bool BrowserPageWPE::holdAt(uint32_t x, uint32_t y) {
    dispatchPointer((int)x, (int)y, 1, true);
    // Arm the release-swallow: a long-press (word-select OR the editable menu) is followed by a click at
    // this point on finger-up; record it so clickAt swallows that click instead of dismissing the popover.
    m_lastSelDocX = (int)x; m_lastSelDocY = (int)y; m_lastSelMs = g_get_monotonic_time() / 1000;
    return true;
}

/* ---- tap-to-select a word of web content; WebKit paints the selection highlight natively --------- */
void BrowserPageWPE::selectWordAt(int docX, int docY)
{
    if (!m_webView) return;
    // Force the view active/focused so WebKit paints the selection with the ACTIVE colour (our yellow) —
    // otherwise WindowIsActive is false at render time (even when document.hasFocus() is true) and the
    // highlight comes out grey. Re-asserted on every selection since the init-time state doesn't stick.
    if (m_viewBackend) wpe_view_backend_add_activity_state(m_viewBackend,
        wpe_view_activity_state_visible | wpe_view_activity_state_in_window | wpe_view_activity_state_focused);
    m_lastSelDocX = docX; m_lastSelDocY = docY; m_lastSelMs = g_get_monotonic_time() / 1000;   // so clickAt can swallow the release
    WLOG("selectWordAt in=(%d,%d) scrollX=%d scrollY=%d renderedY=%d deliveredY=%d", docX, docY, m_scrollX, m_scrollY, m_renderedY, m_deliveredY);
    /* Three coordinate frames exist in the tall-buffer pan model and they DIVERGE:
     *   docX/docY   = true-document (the app already added the adapter pan to the screen tap)
     *   m_renderedY = WebKit's real scroll (window.scrollY) — where the tall buffer is painted from
     *   m_scrollY   = the adapter's VISIBLE pan — where the user is actually looking
     * Hit-test: convert document -> WebKit viewport by subtracting m_renderedY (NOT m_scrollY — same reason
     * dispatchPointer uses m_renderedY). getClientRects() then returns rects in that WebKit-viewport space.
     * Output: convert WebKit-viewport -> VISIBLE viewport (what the adapter shows at m_scrollY) by adding
     * panY = m_renderedY - m_scrollY on Y. X has no pan buffer so a.left is already visible-relative. The app
     * then only adds the WebView's on-screen offset. This lands markers on the word at ANY scroll; the old
     * "subtract scroll both ways" and the "no scroll math" versions were both wrong off the first screen. */
    int vx = docX - m_scrollX, vy = docY - m_renderedY;
    int panY = m_renderedY - m_scrollY;
    char js[2200];
    snprintf(js, sizeof js,
        "(function(x,y,PY){"
        /* form input/textarea: getSelection() is empty for these, so select the word around the caret with
         * setSelectionRange and report the FIELD's rect (no per-char rects) with ed:1 (the app skips markers). */
        "var ae=document.activeElement;"
        /* Only treat this as an input selection when the PRESS is actually inside the focused field —
         * otherwise (input focused earlier, user long-presses page text elsewhere) fall through to the
         * page path so normal-page selection still lands at the finger. */
        "var aer=(ae&&(ae.tagName=='INPUT'||ae.tagName=='TEXTAREA'))?ae.getBoundingClientRect():null;"
        "if(aer&&x>=aer.left&&x<=aer.right&&y>=aer.top&&y<=aer.bottom){"
        "ae.focus();var v=ae.value||'',p=ae.selectionStart;if(p==null)p=v.length;"
        "var ss=p,ee=p;while(ss>0&&/\\S/.test(v.charAt(ss-1)))ss--;while(ee<v.length&&/\\S/.test(v.charAt(ee)))ee++;"
        "if(ss>=ee)return JSON.stringify({len:0});"
        "ae.setSelectionRange(ss,ee);var q=aer,h=Math.min(q.height,26);"
        "return JSON.stringify({sx:Math.round(q.left),sy:Math.round(q.top+PY),sh:Math.round(h),ex:Math.round(q.right),ey:Math.round(q.top+h+PY),len:ee-ss,ed:1});}"
        /* page / contenteditable text */
        "var s=window.getSelection();if(!s)return '';"
        "try{window.focus();}catch(e){}"
        "s.removeAllRanges();"
        "var r=(document.caretRangeFromPoint?document.caretRangeFromPoint(x,y):null);"
        "if(r){s.addRange(r);try{s.modify('move','backward','word');s.modify('extend','forward','word');}catch(e){}"
        "if(!s.toString().trim().length)s.removeAllRanges();}"
        "if(!s.rangeCount||!s.toString().length){var el=document.elementFromPoint(x,y);if(el){var rr=document.createRange();rr.selectNodeContents(el);try{s.addRange(rr);}catch(e){}}}"
        "if(!s.rangeCount||!s.toString().length)return '';"
        "var rc=s.getRangeAt(0).getClientRects();if(!rc.length)return '';"
        "var a=rc[0],b=rc[rc.length-1];"
        "return JSON.stringify({sx:Math.round(a.left),sy:Math.round(a.top+PY),sh:Math.round(a.height),ex:Math.round(b.right),ey:Math.round(b.bottom+PY),len:s.toString().length,ed:(ae&&ae.isContentEditable?1:0),hf:(document.hasFocus?document.hasFocus():-1)});"
        "})(%d,%d,%d)",
        vx, vy, panY);
    webkit_web_view_evaluate_javascript(m_webView, js, -1, nullptr, nullptr, nullptr, &BrowserPageWPE::onSelectResult, this);
}
/* Drag a selection marker: keep the un-dragged end anchored and move the other end to the point. Same
 * coord conversions as selectWordAt (document -> WebKit viewport in, WebKit -> visible viewport out). */
void BrowserPageWPE::extendSelectionTo(int whichEnd, int docX, int docY)
{
    if (!m_webView) return;
    if (m_viewBackend) wpe_view_backend_add_activity_state(m_viewBackend,
        wpe_view_activity_state_visible | wpe_view_activity_state_in_window | wpe_view_activity_state_focused);
    int vx = docX - m_scrollX, vy = docY - m_renderedY;
    int panY = m_renderedY - m_scrollY;
    WLOG("extendSelectionTo which=%d in=(%d,%d) scrollY=%d renderedY=%d", whichEnd, docX, docY, m_scrollY, m_renderedY);
    char js[1200];
    snprintf(js, sizeof js,
        "(function(x,y,W,PY){var s=window.getSelection();if(!s||!s.rangeCount)return '';"
        "try{window.focus();}catch(e){}"
        "var r=(document.caretRangeFromPoint?document.caretRangeFromPoint(x,y):null);if(!r)return '';"
        "var rg=s.getRangeAt(0),aN,aO;"
        /* W==1: dragging the END -> anchor at current start; else dragging the START -> anchor at current end */
        "if(W==1){aN=rg.startContainer;aO=rg.startOffset;}else{aN=rg.endContainer;aO=rg.endOffset;}"
        "try{s.setBaseAndExtent(aN,aO,r.startContainer,r.startOffset);}catch(e){return '';}"
        "if(!s.rangeCount||!s.toString().length)return '';"
        "var rc=s.getRangeAt(0).getClientRects();if(!rc.length)return '';"
        "var a=rc[0],b=rc[rc.length-1];"
        "return JSON.stringify({sx:Math.round(a.left),sy:Math.round(a.top+PY),sh:Math.round(a.height),ex:Math.round(b.right),ey:Math.round(b.bottom+PY),len:s.toString().length});"
        "})(%d,%d,%d,%d)",
        vx, vy, whichEnd, panY);
    webkit_web_view_evaluate_javascript(m_webView, js, -1, nullptr, nullptr, nullptr, &BrowserPageWPE::onSelectResult, this);
}
/* Select the whole page body and report its rects (so "Select All" shows markers + keeps the highlight). */
void BrowserPageWPE::selectAll()
{
    if (!m_webView) return;
    int panY = m_renderedY - m_scrollY;
    char js[1700];
    snprintf(js, sizeof js,
        "(function(PY){"
        /* form input/textarea: select the whole value, report the field rect with ed:1. The focused element
         * is the reliable signal here (paste into it works, so it's focused when the popover is up); a page
         * 'Select All' runs after a page-text selection, by which point window.focus() has blurred any input. */
        "var ae=document.activeElement;"
        "if(ae&&(ae.tagName=='INPUT'||ae.tagName=='TEXTAREA')){"
        "ae.focus();var v=ae.value||'';if(!v.length)return JSON.stringify({len:0});ae.setSelectionRange(0,v.length);"
        "var q=ae.getBoundingClientRect(),h=Math.min(q.height,26);"
        "return JSON.stringify({sx:Math.round(q.left),sy:Math.round(q.top+PY),sh:Math.round(h),ex:Math.round(q.right),ey:Math.round(q.top+h+PY),len:v.length,ed:1});}"
        "var s=window.getSelection();if(!s)return JSON.stringify({len:0});"
        "try{window.focus();s.removeAllRanges();var r=document.createRange();r.selectNodeContents(document.body||document.documentElement);s.addRange(r);}catch(e){return JSON.stringify({len:0});}"
        "if(!s.rangeCount||!s.toString().length)return JSON.stringify({len:0});"
        "var rc=s.getRangeAt(0).getClientRects();if(!rc.length)return JSON.stringify({len:0});"
        "var a=rc[0],b=rc[rc.length-1];"
        "return JSON.stringify({sx:Math.round(a.left),sy:Math.round(a.top+PY),sh:Math.round(a.height),ex:Math.round(b.right),ey:Math.round(b.bottom+PY),len:s.toString().length,ed:(ae&&ae.isContentEditable?1:0)});"
        "})(%d)", panY);
    webkit_web_view_evaluate_javascript(m_webView, js, -1, nullptr, nullptr, nullptr, &BrowserPageWPE::onSelectResult, this);
}
/* Report the CURRENT selection's rects (or {len:0} if none) on "selectionBounds". Called shortly after a
 * tap: the tap's mousedown natively collapses the selection, so this reports {len:0} and the app hides the
 * markers/popover — which is how "tap outside to dismiss" works without the (disabled) single-tap event. */
void BrowserPageWPE::reportCurrentSelection()
{
    if (!m_webView) return;
    int panY = m_renderedY - m_scrollY;
    char js[760];
    snprintf(js, sizeof js,
        "(function(PY){var s=window.getSelection();"
        "if(!s||!s.rangeCount||!s.toString().length)return JSON.stringify({len:0});"
        "var rc=s.getRangeAt(0).getClientRects();if(!rc.length)return JSON.stringify({len:0});"
        "var a=rc[0],b=rc[rc.length-1];"
        "return JSON.stringify({sx:Math.round(a.left),sy:Math.round(a.top+PY),sh:Math.round(a.height),ex:Math.round(b.right),ey:Math.round(b.bottom+PY),len:s.toString().length});"
        "})(%d)", panY);
    webkit_web_view_evaluate_javascript(m_webView, js, -1, nullptr, nullptr, nullptr, &BrowserPageWPE::onSelectResult, this);
}
gboolean BrowserPageWPE::onSelReportTimer(gpointer ud)
{
    if (bpwpe_dead(static_cast<BrowserPageWPE*>(ud))) return G_SOURCE_REMOVE;   // card closed before the timer fired
    static_cast<BrowserPageWPE*>(ud)->reportCurrentSelection();
    return G_SOURCE_REMOVE;
}
/* A quick tap outside the selection collapses it natively but leaves the app's overlay markers behind
 * (there is no single-tap event to the app). So on a quick tap we explicitly clear the selection and
 * report {len:0}, which dismisses the markers/popover. */
void BrowserPageWPE::clearSelectionAndReport()
{
    if (!m_webView) return;
    // Do NOT removeAllRanges when an editable field is focused — that wipes the input's caret and breaks
    // typing/paste. Only clear a document (non-editable) selection. Either way report {len:0} to hide markers.
    const char* js =
        "(function(){var ae=document.activeElement,"
        "ed=ae&&(ae.isContentEditable||ae.tagName=='INPUT'||ae.tagName=='TEXTAREA');"
        "if(!ed){var s=window.getSelection();if(s)s.removeAllRanges();}"
        "return JSON.stringify({len:0});})()";
    webkit_web_view_evaluate_javascript(m_webView, js, -1, nullptr, nullptr, nullptr, &BrowserPageWPE::onSelectResult, this);
}
gboolean BrowserPageWPE::onSelClearTimer(gpointer ud)
{
    if (bpwpe_dead(static_cast<BrowserPageWPE*>(ud))) return G_SOURCE_REMOVE;   // card closed within ~130ms of the tap
    static_cast<BrowserPageWPE*>(ud)->clearSelectionAndReport();
    return G_SOURCE_REMOVE;
}
/* Selection-geometry result -> app via the "selectionBounds" channel (JSON start/end rects). */
void BrowserPageWPE::onSelectResult(GObject* obj, GAsyncResult* res, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    GError* err = nullptr;
    JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (bpwpe_dead(self)) { if (v) g_object_unref(v); if (err) g_error_free(err); return; }
    if (!v) { if (err) g_error_free(err); return; }
    char* s = jsc_value_to_string(v);
    WLOG("selectWordAt result: %s", (s && s[0]) ? s : "(empty)");
    if (s && s[0] && self->m_server) self->m_server->msgActionData(self->m_proxy, "selectionBounds", s);
    if (s) g_free(s);
    g_object_unref(v);
}
void BrowserPageWPE::onCopyText(GObject* obj, GAsyncResult* res, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    GError* err = nullptr;
    JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (bpwpe_dead(self)) { if (v) g_object_unref(v); if (err) g_error_free(err); return; }
    if (!v) { if (err) g_error_free(err); return; }
    char* s = jsc_value_to_string(v);
    if (s && s[0] && self->m_server) {
        /* The YAP message buffer is 16KB (kMaxMsgLen). A huge copy (e.g. Select All on a big page) blew past
         * it — YapPacket's g_return_if_fail dropped the write, corrupting the stream and crashing the client
         * (LunaSysMgr). Cap well under 16KB, backing off to a UTF-8 boundary so we don't split a sequence. */
        size_t len = strlen(s);
        const size_t CAP = 500000;   /* under the new dynamic YAP 512KB cap */
        if (len > CAP) {
            size_t cut = CAP;
            while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;   /* don't cut mid-UTF-8 */
            s[cut] = '\0';
            WLOG("copiedText truncated %u -> %u (YAP 512KB cap)", (unsigned)len, (unsigned)cut);
        }
        self->m_server->msgActionData(self->m_proxy, "copiedText", s);
    }
    if (s) g_free(s);
    g_object_unref(v);
}

/* ---- web-content drag: button-held pointer motion (in-page sliders/canvas/DnD under touch) ------- */
void BrowserPageWPE::dragStart(int contentX, int contentY)
{
    m_dragX = contentX; m_dragY = contentY;
    dispatchPointer(contentX, contentY, 1, true);   // press at the drag origin
}
void BrowserPageWPE::dragProcess(int deltaX, int deltaY)
{
    if (!m_viewBackend) return;
    m_dragX += deltaX; m_dragY += deltaY;
    int x = m_dragX - m_scrollX, y = m_dragY - m_renderedY;
    /* motion WITH button 1 held so WebKit treats it as a drag, not a hover */
    struct wpe_input_pointer_event move = { wpe_input_pointer_event_type_motion, 0, x, y, 1, 1, 0 };
    wpe_view_backend_dispatch_pointer_event(m_viewBackend, &move);
}
void BrowserPageWPE::dragEnd(int contentX, int contentY)
{
    if (contentX || contentY) { m_dragX = contentX; m_dragY = contentY; }
    dispatchPointer(m_dragX, m_dragY, 1, false);   // release
}

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
        // a single-tap gesture = a quick tap -> dismiss any active selection (markers + popover)
        g_timeout_add(130, &BrowserPageWPE::onSelClearTimer, this);
    }
}

/* The Atlas adapter delivers taps as touch events (not mouseEvent/clickAt) — this was a no-op stub,
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
        case 0: m_touchDownUs = g_get_monotonic_time(); dispatchPointer(x, y, 1, true);  break;
        case 1: dispatchPointer(x, y, 0, false); break;
        case 2: {
            dispatchPointer(x, y, 1, false);
            gint64 durMs = m_touchDownUs ? (g_get_monotonic_time() - m_touchDownUs) / 1000 : 999;
            // quick tap = dismiss any selection; long-press (the selecting gesture) or a drag = just resync
            if (durMs >= 0 && durMs < 350) g_timeout_add(130, &BrowserPageWPE::onSelClearTimer, this);
            else                           g_timeout_add(130, &BrowserPageWPE::onSelReportTimer, this);
            break;
        }
        default: dispatchPointer(x, y, 1, true); dispatchPointer(x, y, 1, false); g_timeout_add(130, &BrowserPageWPE::onSelClearTimer, this); break;
    }
}

/* webOS/Palm delivers ASCII-based raw key codes (ESC=27, etc.). WPE's keyboard event wants an XKB KEYSYM.
 * Printable Latin-1 (0x20–0x7e) already IS its keysym, so those pass through (why typing letters worked);
 * but the control keys need remapping — notably BackSpace (0x08 → 0xff08), or WebKit never deletes. */
static uint32_t atlas_key_to_keysym(int32_t key)
{
    switch (key) {
        case 8:   return 0xff08;   /* BackSpace */
        case 9:   return 0xff09;   /* Tab       */
        case 10:
        case 13:  return 0xff0d;   /* Return    */
        case 27:  return 0xff1b;   /* Escape    */
        case 127: return 0xffff;   /* Delete    */
        default:  return (uint32_t)key;   /* printable ASCII == its own keysym */
    }
}
void BrowserPageWPE::keyDown(int32_t key, int32_t modifiers, int32_t /*chr*/)
{
    if (!m_viewBackend) return;
    uint32_t ks = atlas_key_to_keysym(key);
    WLOG("keyDown raw=%d(0x%x) -> keysym=0x%x mod=0x%x", key, key, ks, modifiers);
    struct wpe_input_keyboard_event ev = { 0, ks, (uint32_t)key, true, (uint32_t)modifiers };
    wpe_view_backend_dispatch_keyboard_event(m_viewBackend, &ev);
}
void BrowserPageWPE::keyUp(int32_t key, int32_t modifiers, int32_t /*chr*/)
{
    if (!m_viewBackend) return;
    uint32_t ks = atlas_key_to_keysym(key);
    struct wpe_input_keyboard_event ev = { 0, ks, (uint32_t)key, false, (uint32_t)modifiers };
    wpe_view_backend_dispatch_keyboard_event(m_viewBackend, &ev);
}

void BrowserPageWPE::setFocus(bool enable)
{
    m_focused = enable;
    if (m_viewBackend) wpe_atlas_view_backend_set_visible(m_viewBackend, enable);
}

/* ---- freeze / thaw (purge offscreens on backgrounding; re-attach on return) ----------------- */
bool BrowserPageWPE::freeze()
{
    m_frozen = true;
    delete m_offscreen0; m_offscreen0 = 0; m_ownOffscreen0 = false;
    delete m_offscreen1; m_offscreen1 = 0; m_ownOffscreen1 = false;
    if (m_viewBackend) wpe_atlas_view_backend_set_visible(m_viewBackend, false);
    return true;
}
bool BrowserPageWPE::thaw(int key1, int key2, int size)
{
    m_frozen = false;
    bool ok = attachToBuffer(m_virtualWindowWidth, m_virtualWindowHeight, key1, key2, size);
    if (m_viewBackend) wpe_atlas_view_backend_set_visible(m_viewBackend, true);
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

/* ---- WPE WebKit signals → Atlas adapter notifications --------------------------------------- *
 * These map onto the existing BrowserServerBase msg* notifications the 602 doLoad / titleChanged
 * slots used. Exact method names per BrowserServerBase.h. */
/* Fill the held offscreen with opaque white + reset pan state, and flush it so the adapter shows a blank
 * page immediately. Called on navigation commit: otherwise the buffer keeps the OLD page's pixels until
 * the new page paints (and on a blank page like about:blank WebKit may never composite a fresh frame, so
 * the last page lingers indefinitely). White = 0xFFFFFFFF (ARGB32-premul opaque white). */
void BrowserPageWPE::clearToWhite()
{
    int buf = m_ownOffscreen0 ? 0 : (m_ownOffscreen1 ? 1 : -1);
    if (buf < 0) return;                                   /* both held by adapter — can't flush now */
    BrowserOffscreenQt* off = (buf == 0) ? m_offscreen0 : m_offscreen1;
    if (!off || m_renderWidth <= 0 || m_renderHeight <= 0) return;
    m_renderedY = 0; m_deliveredY = 0; m_pageHeight = 0;   /* new page starts at top; pan off until new height known */
    /* A fresh page always starts scrolled to the top. If the PREVIOUS page was scrolled (e.g. the user
     * panned a login form up to see a field behind the VKB), the adapter still holds that scroll offset and
     * would blit the new (often short) page at that offset — reading past the buffer -> WHITE page. Reset
     * the engine scroll state AND tell the adapter to scroll to 0 so its blit is identity (same fix the
     * history-restore branch applies). */
    m_scrollX = 0; m_scrollY = 0; m_lastScrollY = 0; m_dirStreak = 0; m_scrollVel = 0;
    if (m_server) m_server->msgScrolledTo(m_proxy, 0, 0);
    unsigned char* dst = off->rasterBuffer();
    if (dst) memset(dst, 0xFF, (size_t)m_renderWidth * m_renderHeight * 4);
    BrowserOffscreenInfo* hdr = off->header();
    if (hdr) {
        hdr->bufferWidth = m_renderWidth; hdr->bufferHeight = m_renderHeight;
        hdr->contentZoom = ((m_virtualWindowWidth > 0) ? (double)m_renderWidth / (double)m_virtualWindowWidth : 1.0) * m_webkitZoom;
        hdr->renderedX = 0; hdr->renderedY = 0;
        hdr->renderedWidth = m_renderWidth; hdr->renderedHeight = m_renderHeight;
    }
    m_renderPending = false;
    flushBuffer(buf);
    WLOG("clearToWhite: blanked buffer on navigation (%dx%d)", m_renderWidth, m_renderHeight);
}
/* ---- page-load timing instrumentation: 1.2s after LOAD_FINISHED, dump the Navigation Timing +
 * resource summary to bpwpe.log ("PERF ...") so we can see where the wall-clock goes. ------------- */
void BrowserPageWPE::onPerfTiming(GObject* obj, GAsyncResult* res, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    GError* err = nullptr;
    JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
    if (!v) { if (err) g_error_free(err); return; }
    char* s = jsc_value_to_string(v);
    if (s) { if (g_livePages.find(self) != g_livePages.end()) WLOG("PERF %s", s); g_free(s); }
    g_object_unref(v);
}
gboolean BrowserPageWPE::perfTimingTimeout(gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    if (g_livePages.find(self) == g_livePages.end() || !self->m_webView) return G_SOURCE_REMOVE;
    const char* js =
        "(function(){try{var t=performance.timing,n=t.navigationStart;"
        "var res=performance.getEntriesByType?performance.getEntriesByType('resource'):[];"
        "var loc=location.hostname.split('.').slice(-2).join('.');"
        "var sz=0,byType={},tp={},tpN=0,tpScript=0,tpKB=0;"
        "for(var i=0;i<res.length;i++){var r=res[i];sz+=(r.transferSize||0);"
        "var it=r.initiatorType||'other';byType[it]=(byType[it]||0)+1;"
        "var host=(r.name||'').split('/')[2]||'';var dom=host.split('.').slice(-2).join('.');"
        "if(dom&&dom!==loc){tpN++;tp[dom]=(tp[dom]||0)+1;tpKB+=(r.transferSize||0);if(it==='script')tpScript++;}}"
        "var tpArr=Object.keys(tp).map(function(d){return tp[d]+':'+d;}).sort(function(a,b){return parseInt(b)-parseInt(a);}).slice(0,10);"
        "return JSON.stringify({ttfb:t.responseStart-n,domInt:t.domInteractive-n,dcl:t.domContentLoadedEventEnd-n,"
        "domComplete:t.domComplete-n,load:t.loadEventEnd-n,nres:res.length,kb:Math.round(sz/1024),byType:byType,"
        "tpN:tpN,tpScript:tpScript,tpKB:Math.round(tpKB/1024),tp:tpArr});"
        "}catch(e){return 'perf-err:'+e;}})()";
    webkit_web_view_evaluate_javascript(self->m_webView, js, -1, nullptr, nullptr, nullptr,
                                        &BrowserPageWPE::onPerfTiming, self);
    return G_SOURCE_REMOVE;
}
void BrowserPageWPE::onLoadChanged(WebKitWebView* v, WebKitLoadEvent ev, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    WLOG("loadChanged ev=%d (0=start 1=redir 2=commit 3=finish) uri=%s", ev, webkit_web_view_get_uri(v) ? webkit_web_view_get_uri(v) : "?");
    switch (ev) {
        case WEBKIT_LOAD_STARTED:
            self->m_userInteracted = false;   // new page: ignore autofocus VKB grabs until the user taps
            self->m_server->msgLoadStarted(self->m_proxy);
            break;
        case WEBKIT_LOAD_COMMITTED:
            if (self->m_navByHistory) {
                /* bf-cache back/forward: the restore is INSTANT and composites its own frame right away.
                 * Do NOT blank-flush (that consumes an offscreen buffer the fast restore frame then can't
                 * get -> onFrame DROP both-held -> blank page). Reset the pan state for the restored page,
                 * and CRITICALLY tell the adapter to scroll to 0 (msgScrolledTo) — otherwise the adapter keeps
                 * the PREVIOUS page's scroll and blits oy = local + oldScroll - renderedY(0) -> reads outside
                 * the buffer -> white/wrong. With mScrollPos=0 the blit is identity -> restored page shows from top. */
                self->m_navByHistory = false;
                self->m_renderedY = 0; self->m_deliveredY = 0; self->m_pageHeight = 0; self->m_renderPending = false;
                self->m_scrollX = 0; self->m_scrollY = 0; self->m_lastScrollY = 0; self->m_dirStreak = 0; self->m_scrollVel = 0;
                if (self->m_settleTimer) { g_source_remove(self->m_settleTimer); self->m_settleTimer = 0; }
                self->m_server->msgScrolledTo(self->m_proxy, 0, 0);   /* reset adapter mScrollPos -> identity blit */
                /* Offscreen WPE bf-cache restore does NOT auto-composite a frame (the layer tree is cached),
                 * so the buffer keeps the OLD page -> "repaint not from cache". Force a composite with a scroll
                 * nudge that ends at the top (matching the adapter reset); onFrame then delivers the restored page. */
                webkit_web_view_evaluate_javascript(v, "window.scrollTo(0,4);window.scrollTo(0,0);", -1,
                                                    nullptr, nullptr, nullptr, nullptr, nullptr);
                WLOG("HISTORY restore commit -> reset pan + msgScrolledTo(0,0) + repaint nudge");
            } else {
                /* blank the old page NOW — the new document has committed; don't show the last page while it paints */
                self->clearToWhite();
            }
            /* tell the adapter the content size so it sets up the display area + shows painted buffers */
            self->m_server->msgContentsSizeChanged(self->m_proxy, self->m_virtualWindowWidth, self->m_virtualWindowHeight);
            /* Feature 3: fetch the favicon EARLY — at commit (final URL known), not at finish. On slow sites
             * the page can take many seconds to finish; the favicon only needs the host, so downloading it
             * here (in parallel with the page) makes it ready well before the user can bookmark. */
            {
                const char* cu = webkit_web_view_get_uri(v);
                if (cu) {
                    std::string favDest = atlas_favicon_dest_for(cu);
                    if (!favDest.empty()) {
                        mkdir(ATLAS_FAVICON_DIR, 0755);   /* under the app bundle; parent exists; ignore EEXIST */
                        atlas_start_favicon_download(v, cu, favDest);
                        WLOG("favicon prefetch (commit) -> %s", favDest.c_str());
                    }
                }
            }
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
            g_timeout_add(1200, &BrowserPageWPE::perfTimingTimeout, self);   /* dump Navigation Timing -> "PERF ..." */
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
void BrowserPageWPE::onWebProcessTerminated(WebKitWebView* v, WebKitWebProcessTerminationReason reason, gpointer ud)
{
    BrowserPageWPE* self = static_cast<BrowserPageWPE*>(ud);
    const char* r = reason == WEBKIT_WEB_PROCESS_CRASHED               ? "CRASHED"
                  : reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "EXCEEDED_MEMORY_LIMIT"
                  : reason == WEBKIT_WEB_PROCESS_TERMINATED_BY_API     ? "TERMINATED_BY_API"
                  : "UNKNOWN";
    const char* uri = webkit_web_view_get_uri(v);
    WLOG("*** WEBPROCESS TERMINATED reason=%s uri=%s ***", r, uri ? uri : "?");
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
