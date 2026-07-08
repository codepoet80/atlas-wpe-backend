# Atlas GStreamer codec extras

WPE WebKit decides `canPlayType` / `MediaPlayer::supportsType` by scanning the
live GStreamer registry (`GStreamerRegistryScanner`). Getting a codec to play in
the browser therefore needs **two** things, not one:

1. the GStreamer **decode chain** (demuxer + parser + decoder) registered, and
2. the WebKit scanner to **map the container MIME** to that chain.

`build-all.sh` ships gst-plugins-base with `-Dogg/-Dvorbis/-Dopus/-Dtheora=disabled`,
so these plugins are built here separately and deployed on top of the runtime.

## Audio: Ogg Vorbis / Opus / FLAC  (DONE)

`build-audio-codecs.sh` builds libogg, libvorbis, and the gst `ogg`+`vorbis`
plugins; `deploy-audio-codecs.sh` pushes them and rescans the registry.

### The non-obvious part — why the Ogg demuxer alone wasn't enough
`avdec_opus`, `avdec_flac`, `flacparse` all come from **gst-libav** already, and
after deploying `libgstogg.so` the raw pipeline decodes Ogg Opus/FLAC fine under
`gst-launch`. But the browser still returned a hard **No**.

Root cause = `GStreamerRegistryScanner.cpp` (~line 671): when the Ogg demuxer is
present it registers the `audio/ogg` **container** MIME **only if Vorbis or Speex
is supported** — there is no opus/flac branch. `canPlayType("audio/ogg; codecs=opus")`
checks the container first, finds `audio/ogg` absent, and returns "" before ever
looking at the (present) opus/flac codec.

Fix chosen: build **native Vorbis** (`libvorbis` + gst `vorbis` plugin). Its mere
presence flips `vorbisSupported=true` at runtime — the scanner reads the live
registry — which registers `audio/ogg` and thereby unblocks **Vorbis + Opus + FLAC
in Ogg all at once, with no WebKit rebuild**. gst-libav deliberately blacklists its
own vorbis decoder (`gstavauddec.c` "we have native gstreamer plugins"), so native
libvorbis is the only path regardless.

## WMA / WMV  (DONE — verified playing)

Three parts, all landed (patches in `../patches/`):

1. **ffmpeg**: rebuilt with `wmv1,wmv2,wmv3,vc1` added to `build-ffmpeg.sh`'s
   `--enable-decoder` list (WMA `wmav1/2/wmapro` + the `asf` demuxer were already on).
   Deploy the new `libavcodec.so.59.37.100` as SONAME `libavcodec.so.59`.
2. **gst-libav ASF demuxer** (`gst-libav-1.20.7-atlas-register-asf-demuxer.patch`):
   the non-obvious bug — `gstavdemux.c` has TWO `asf` spots. The "don't use typefind
   functions" list (~L2073) only sets `register_typefind_func=FALSE`; editing it does
   NOT register the element. The element is gated by the **rank whitelist** (~L2114-2151)
   where anything not listed hits `else { rank=NONE; continue; }` = unregistered. FIX =
   add `"asf"` to that whitelist → `avdemux_asf` registers at `GST_RANK_MARGINAL`.
   `ninja -C _b ext/libav/libgstlibav.so`, redeploy `libgstlibav.so`.
3. **WebKit scanner** (`wpewebkit-2.52.4-wma-wmv-canplaytype.patch`): adds the
   `audio/x-ms-wma` / `video/x-ms-wmv` / `video/x-ms-asf` container MIME mapping (queries
   `video/x-ms-asf` demuxer + `audio/x-wma` / `video/x-wmv` decoders). Without it
   `canPlayType` returns "" even though the decode chain exists. Compiled into the
   **WebGL-OFF** libWPEWebKit (WebGL-ON/ANGLE whites out the compositor — see the repo
   memory note). Remember to prefix-patch the lib on deploy (`deploy-252.sh` step 6).

After deploying all three + `rm /tmp/atlas-gstreg.bin` + restart, `avdemux_asf`,
`avdec_wmav2/wmapro`, `avdec_wmv3/wmv2/vc1` register and WMA + WMV both play. Verified
via `luna-send … launch {params:{target:"file://…/codecplay.html"}}` + the scrape tool.

## Definitive on-device codec support
Audio WORKS: mp3, aac, opus (webm+ogg), vorbis (webm+ogg), flac (raw+ogg), wav,
ac3, alac, **wma**.
Video WORKS (HW): h264, mpeg4, h263; vp8 (SW); **wmv/vc-1 (SW)**. Video FAILS: vp9, theora.
