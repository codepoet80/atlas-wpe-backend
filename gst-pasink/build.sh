#!/bin/sh
# Builds libgstatlaspasink.so — the Atlas PulseAudio (pa_simple) audio sink for WPE/GStreamer 1.20.7.
# Registered Sink/Audio at PRIMARY+20 so WebKit's autoaudiosink picks it; routes to webOS audiod -> speaker.
set -e
CC=/home/herrie/x-tools/arm-unknown-linux-gnueabi-gcc125/bin/arm-unknown-linux-gnueabi-gcc
S=/home/herrie/webos/wpe/staging-glibc-252
export PKG_CONFIG_PATH="$S/lib/pkgconfig"
PULSE_INC=/home/herrie/webos/qt5/qt5.9/device/sysroot/usr/include   # PulseAudio 0.9.22 headers (match device)
CF=$(pkg-config --cflags gstreamer-1.0 gstreamer-audio-1.0)
LF=$(pkg-config --libs gstreamer-1.0 gstreamer-audio-1.0)
# -L must point at a dir holding the device libpulse-simple.so / libpulse.so (copied from device).
$CC -O2 -fPIC -shared -std=gnu99 -DPACKAGE='"atlaspasink"' -DVERSION='"1.0"' \
    -I"$PULSE_INC" $CF -o libgstatlaspasink.so gstpasink.c $LF \
    -L"$(dirname "$0")" -lpulse-simple -lpulse
echo "built libgstatlaspasink.so ($(md5sum libgstatlaspasink.so | cut -d' ' -f1))"
# deploy: stop atlas (plugin in use), then novacom put -> deviceroot/wpe-252/lib/gstreamer-1.0/
