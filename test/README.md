# Atlas scroll / rotation test harness

Objective, closed-loop testing of the pan/scroll + rotation paths.

## Scroll sweep (engine-side, no pixels needed)
1. Load a tall page (e.g. `gridtest.html`, 6400px) in Atlas.
2. On device: `echo "<stepPx> <intervalMs>" > /tmp/atlas_test.cfg; touch /tmp/atlas_scrolltest`
3. Read `/tmp/atlas_scrolltest.result` -> `uncovered%` (steps where the delivered buffer didn't
   cover the viewport = visible tearing/white) + `maxStall` (worst re-render latency).
   Baselines (moderate 120/50): ~7% good. Fast flick (240/30): ~70% (composite can't keep up — settle-defer).

## Rotation (engine-side)
`touch /tmp/atlas_rotatetest` toggles setWindowSize portrait<->landscape (toggleRotationTest).
NOTE: this only rotates the ENGINE; the adapter/LunaSysMgr isn't rotated, so frames mismatch and get
dropped — use it for engine resize/reflow paths, NOT for the visual "wider" glitch (needs a real rotate).

## Framebuffer pixel check (fbcheck.py)
`dd if=/dev/fb0 of=/tmp/fb.raw bs=1024 count=9216` -> pull -> `python3 fbcheck.py fb.raw`.
Decodes gridtest.html: blue gradient=scroll pos, green vlines=width scale (>140 => too wide),
red hlines=vertical scale, blue backward-jumps=tearing. CAVEAT: /dev/fb0 only holds browser CONTENT
while it's actively compositing (e.g. video); a static page shows only the chrome.
