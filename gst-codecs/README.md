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

## Video / WMA / WMV  (needs a WebKit rebuild — see below)

WMA/WMV is heavier and is NOT a runtime-only fix:
- gst-libav **blacklists the ASF demuxer** (`gstavdemux.c`, `!strcmp(name,"asf")`),
  so `avdemux_asf` never registers → ASF has no demuxer. Un-blacklisting it needs a
  gst-libav patch + rebuild (the WMA/WMV audio+video decoders `avdec_wmav2/wmapro/
  wmv3/vc1` are themselves NOT blacklisted).
- The scanner has **no `audio/x-ms-wma` / `video/x-ms-wmv` MIME mapping at all**
  (the `video/x-ms-asf` demuxer entry at ~line 656 has empty MIME/codec lists), so
  a WebKit source patch + full `libWPEWebKit` rebuild is required for `canPlayType`.

See `../patches/` for the WebKit scanner patch once landed.

## Definitive on-device codec support
Audio WORKS: mp3, aac, opus (webm+ogg), vorbis (webm+ogg), flac (raw+ogg), wav,
ac3, alac.  Audio FAILS: wma (no ASF demux — WIP).
Video WORKS (HW): h264, mpeg4, h263; vp8 (SW). Video FAILS: vp9, theora, wmv (WIP).
