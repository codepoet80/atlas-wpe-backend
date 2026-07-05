#!/bin/sh
# Build libgstmdpdetile.so for the Atlas/WPE TouchPad target (gst 1.20.7, staging-glibc-252).
set -e
export PKG_CONFIG_PATH=/home/herrie/webos/wpe/staging-glibc-252/lib/pkgconfig
TC=/home/herrie/x-tools/arm-unknown-linux-gnueabi-gcc125/bin/arm-unknown-linux-gnueabi-gcc
CF=$(pkg-config --cflags gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0)
LF=$(pkg-config --libs gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0)
$TC -O2 -fPIC -shared -std=gnu99 -DPACKAGE='"mdpdetile"' -DVERSION='"1.0"' \
    -o libgstmdpdetile.so gstmdpdetile.c $CF $LF
echo "built libgstmdpdetile.so ($(md5sum libgstmdpdetile.so | cut -d' ' -f1))"
# device deploy: put to the browser engine gstreamer plugin dir + the gsttest plugins dir
# novacom put file:///media/internal/wpe-252/lib/gstreamer-1.0/libgstmdpdetile.so < libgstmdpdetile.so
