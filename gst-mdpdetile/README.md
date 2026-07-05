# gst-mdpdetile — MDP4 HW de-tiler for the TouchPad video path

GStreamer element `mdpdetile`: de-tiles the Qualcomm decoder's tiled NV12 (`NV12_64Z32`) to
linear NV12 in **hardware** via the MDP4 rotator (`/dev/msm_rotator`), replacing the CPU
`videoconvert` de-tile that dropped frames / rendered black at >320p.

- `gstmdpdetile.c` — the GStreamer element (GstBaseTransform). Pipeline use:
  `omxh264dec ! mdpdetile ! videoconvert ! <sink>` (the sink gets linear NV12; the WebKit sink
  needs BGRA, so a lightweight convert follows — the expensive tiled de-tile is done in HW).
- `mdp_detile.c` — standalone validation + benchmark tool (bench mode: 8th arg = iterations).
- `build.sh` — cross-compile against staging-glibc-252 (gst 1.20.7, gcc125 toolchain).

Recipe (reverse-engineered from the legacy libPmMediaGstVideoSinkLib.so): rotator START with
src=MDP_Y_CRCB_H2V2_TILE(12) → dst=MDP_Y_CRCB_H2V2(5), ROTATE per frame (version_key=0xA5B4C301),
then swap Cr/Cb → Cb/Cr in the copy-out for true NV12. Measured HW de-tile: ~2.6 ms/frame @640p,
~9.8 ms @720p (vs the CPU videoconvert that caused ~120 QoS frame-drops).

The decoder's tiled output is fed via a copy into a `/dev/pmem_adsp` src buffer (v1); zero-copy
input via the decoder's OMX `pPlatformPrivate` pmem fd is a later optimization.

WPE integration: see `../patches/wpewebkit-2.52.4-mdpdetile-videosink.patch` (createVideoSink wraps
the sink in `[mdpdetile ! videoconvert ! webkitsink]`, env-gated by `ATLAS_MDPDETILE=1`).
