#!/bin/bash
# Push the ogg+vorbis plugins and their libs to the device, then force a
# GStreamer registry rescan so the WPE codec scanner re-runs with them present.
#
# IMPORTANT device quirks:
#  - The app cryptofs does NOT support symlinks, so each versioned .so must be
#    copied to its SONAME name (libvorbis.so.0, NOT a symlink).
#  - `pkill BrowserServer-atlas` FAILS: the process comm truncates to 15 chars
#    ("BrowserServer-a"). Use `pkill -f` or `kill $(pidof ...)`.
#  - The registry lives at GST_REGISTRY=/tmp/atlas-gstreg.bin (volatile); remove
#    it to force a full rescan. BS respawns on the next page load.
set -e
WPE=/home/herrie/webos/wpe ; STAGING=$WPE/staging-glibc-252
GSTBASE=$WPE/build/gst-plugins-base-1.20.7/_b
D=/media/cryptofs/apps/usr/palm/applications/org.webosports.app.atlas/deviceroot/wpe-252

put() { echo "  -> $2"; novacom put file://"$2" < "$1"; sleep 1; }

put "$GSTBASE/ext/ogg/libgstogg.so"        "$D/lib/gstreamer-1.0/libgstogg.so"
put "$GSTBASE/ext/vorbis/libgstvorbis.so"  "$D/lib/gstreamer-1.0/libgstvorbis.so"
put "$STAGING/lib/libogg.so.0"             "$D/lib/libogg.so.0"
put "$STAGING/lib/libvorbis.so.0"          "$D/lib/libvorbis.so.0"
put "$STAGING/lib/libvorbisenc.so.2"       "$D/lib/libvorbisenc.so.2"

echo ">>> clear registry + restart BrowserServer-atlas"
cat > /tmp/atlas-gst-rescan.sh <<'EOF'
#!/bin/sh
rm -f /tmp/atlas-gstreg.bin
kill -9 $(pidof BrowserServer-atlas) 2>/dev/null
pkill -9 -f BrowserServer-atlas 2>/dev/null
echo "registry cleared, BS killed (respawns on next page load)"
EOF
novacom put file:///tmp/atlas-gst-rescan.sh < /tmp/atlas-gst-rescan.sh
novacom run file://bin/sh -- /tmp/atlas-gst-rescan.sh
echo "Done. Reload a page in the browser; canPlayType(audio/ogg; codecs=vorbis|opus|flac) now works."
