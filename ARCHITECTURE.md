# WPE-Atlas backend — custom libwpe backend for webOS / HP TouchPad

Bridges WPE WebKit's multiprocess EGL rendering into the Atlas BrowserServer's shared-buffer +
Yap display path, replacing the Qt `QGraphicsView` render the 602 engine used. There is **no
Wayland compositor** on webOS, so wpebackend-fdo cannot be used — this backend renders WebKit
offscreen and hands ARGB frames to the existing Atlas offscreen buffers.

## Process model

```
 ┌─ UIProcess = Atlas BrowserServer ──────────────┐      ┌─ WebProcess (WPEWebProcess) ─────────┐
 │  WPEWebView                                    │      │                                       │
 │   ├ wpe_view_backend  (this backend)           │      │  wpe_renderer_backend_egl (this)      │
 │   │   • get_renderer_host_fd  ───socketpair────┼──fd──┤   • get_native_display → Adreno EGL   │
 │   │   • dispatch_set_size / input events       │      │  wpe_renderer_backend_egl_target      │
 │   │   • on frame: → BrowserPage offscreen      │      │   • EGL ctx + offscreen surface/FBO   │
 │   ├ wpe_renderer_host (this) → create_client ──┼──fd──┤   • frame_rendered: glReadPixels →    │
 │   │                                            │      │       shm frame buf → notify UIProc   │
 │   └ frame_handler callback                     │      │   • dispatch_frame_complete           │
 │        → memcpy into m_offscreenN rasterBuffer │      └───────────────────────────────────────┘
 │        → BrowserPage::flushBuffer(N)           │
 │        → m_server->msgPainted(proxy, key)  ────┼─Yap→ BrowserAdapter blits to screen
 └────────────────────────────────────────────────┘
```

The backend `.so` is loaded by `libwpe` in **both** processes (`WPE_BACKEND_LIBRARY`); its
`_wpe_loader_interface.load_object(name)` returns the right interface struct per process:
- UIProcess asks for `_wpe_renderer_host_interface` (and, for standalone use,
  `_wpe_view_backend_interface`). **But the BrowserServer instead supplies the view backend
  in-process** via `wpe_view_backend_create_with_backend_interface()` so the view backend can
  reach the owning `BrowserPage` (its offscreen buffers + `flushBuffer`).
- WebProcess asks for `_wpe_renderer_backend_egl_interface` and
  `_wpe_renderer_backend_egl_target_interface`.

## Frame transport (WebProcess → UIProcess)

Per view, a `socketpair(AF_UNIX, SOCK_SEQPACKET)`:
- view backend keeps fd_ui (`get_renderer_host_fd` returns the peer fd_web, WebKit hands it to
  the WebProcess as the target's `create(int)` arg).
- One shared frame buffer: an **memfd / shm** of `width*height*4`, its fd passed once over the
  socket (SCM_RIGHTS). Both processes `mmap` it. (memfd_create is Linux 3.17 — NOT on kernel
  2.6.35; use `shm_open` + `ftruncate` or a SysV `shmget`, which the Atlas side already uses.)
- Each rendered frame: WebProcess `glReadPixels` into the shared buffer, then sends a 1-byte
  "frame ready" datagram. No 3 MB/frame socket copy.

Double buffering mirrors Atlas: two shared frame slots, ping-pong, so the WebProcess can render
slot N+1 while the UIProcess blits slot N.

## Pixel format

GL readback is `GL_RGBA`/`GL_UNSIGNED_BYTE` (or `GL_BGRA_EXT` if the Adreno exposes it). Atlas
wants **ARGB32 premultiplied** = BGRA byte order on little-endian ARM, premultiplied alpha.
WebKit composits premultiplied, so alpha is fine; a channel swizzle (RGBA→BGRA) is needed unless
`GL_BGRA_EXT` readback is available (it usually is on Adreno). Also flip vertically (GL origin is
bottom-left; the offscreen/QImage origin is top-left).

## Atlas integration seam

The backend stays Atlas-agnostic: the view backend invokes a `wpe_atlas_frame_handler` callback
(set by the BrowserServer) with `(argb, width, height, stride)`. The BrowserServer's handler does
the Atlas-specific work — pick the current offscreen, `memcpy` (or row-copy if strides differ) into
`m_offscreenN->rasterBuffer()`, then `BrowserPage::flushBuffer(N)`. This keeps the backend
testable standalone (dump frames to PNG) and confines the Yap/shm/double-buffer protocol to the
BrowserServer, exactly where the 602 port already implements it.

## Input

Atlas adapter events arrive over Yap (BrowserAdapter → BrowserServer). The BrowserServer translates
them into libwpe events and calls `wpe_view_backend_dispatch_{pointer,axis,touch,keyboard}_event`
on the view backend, which forwards them to the WebProcess. Coordinate space is the virtual page
(same as the 602 setVisibleSize path).

## Open question (agent is mapping it)

Whether WebKit's WPE WebProcess **forces** `eglCreateWindowSurface(native_window)` or can run
fully offscreen (`EGL_KHR_surfaceless_context` + FBO, or a pbuffer). This decides the target's
`initialize`/`get_native_window` impl. The Adreno 220 EGL on webOS has no app-visible window, so
surfaceless/FBO is strongly preferred; if WebKit insists on a window surface we provide a 1x1
pbuffer-config window or the device fbdev native window and still read back via an FBO.

## The "GPU wedge" (atlas-wpe-env#3) — root cause, fixed 2026-08-23

The hang tracked as [atlas-wpe-env#3](https://github.com/Herrie82/atlas-wpe-env/issues/3) — after
GPU-heavy work (site menus, rotation, a few minutes of navigation) no page ever loads again, the
BrowserServer main thread sits in `futex_wait_queue_me` / `__skb_recv_datagram`, the WPEWebProcess is
idle — was never a GPU hang. It was three bugs on the UIProcess (BrowserServer) side — two in this file's
**frame channel**, one in BrowserPageWPE.cpp:

1. **Stale close of the fd handed to WebKit.** `view_get_renderer_host_fd()` returns one end of a
   `socketpair`. WebKit wraps the return value `UnixFileDescriptor::Adopt` (`PageClientImpl::
   hostFileDescriptor()`), ships it to the WebProcess and **closes it** — on another thread, within
   milliseconds. The backend kept the number and closed it again when the first frame arrived. By then
   the number had been reused (strace on-device: the very next `socketpair` got it back 80 ms later),
   so that close hit a live fd — another channel's `sock_ui`, or a WebKit IPC socket. The channel's
   `g_unix_fd_add` source then polled a foreign socket: a blocking `recv()` racing the IPC thread parks
   the main loop in `__skb_recv_datagram`; a stolen IPC datagram leaves a sync reply that never comes
   (`futex_wait_queue_me`); a closed IPC socket looks to WebKit like a crashed WebProcess, so it spawns
   replacements while BS stays stuck. Fix: the returned end is WebKit's — never touched again; the
   channel is reaped on HUP/EOF; `recv()` is `MSG_DONTWAIT`.
2. **Unread FRAME_ACK backlog.** The UIProcess answered every frame with an ack that the WebProcess
   never read. Unread datagrams stay charged to the *sender's* socket buffer, so after a few hundred
   frames on one page (an animated menu, a video, a long scroll session) the blocking `send(ack)`
   parked the main loop forever — right after a perfectly good frame, which is exactly how the wedge
   presented. Fix: the WebProcess drains acks before each FRAME_READY and the ack send is
   `MSG_DONTWAIT`.

3. **SIGUSR1 stolen from JavaScriptCore** (`BrowserPageWPE.cpp`, not this file). A debug trigger for
   the autonomous scroll test did `g_unix_signal_add(SIGUSR1, …)` once a page was live. WTF/JSC uses
   SIGUSR1 (`sigThreadSuspendResume`) to suspend threads for conservative GC stack scanning, and glib's
   `sigaction` replaced WTF's handler. The next concurrent GC of the UIProcess JSC VM (the one behind
   `webkit_web_view_evaluate_javascript`'s JSCValues) then hung the collector in `Thread::suspend →
   sem_wait(globalSemaphoreForSuspendResume)` waiting for a handler that no longer ran, and the main
   thread's next heap entry parked in `JSC::Heap::acquireAccessSlow` — the `futex_wait_queue_me` wedge
   caught on-device 2026-08-23 *after* fixes 1+2 were in. Fix: the SIGUSR1 hook is gone (the
   `/tmp/atlas_scrolltest` file trigger remains). Nothing else in the engine may take SIGUSR1; if it ever
   must, set `JSC_SIGNAL_FOR_GC=<signo>` in the boot wrapper instead.

None of this needs the watchdog that used to abort+respawn the engine; that watchdog is off (0.9.10).

## Build

`libWPEBackend-atlas.so`, linked against `libwpe-1.0`, `libEGL`/`libGLESv2` (stubs at build,
device Adreno at runtime). Run WebKit with `WPE_BACKEND_LIBRARY=libWPEBackend-atlas.so`.
