#!/bin/sh
# Builds (1) the standalone MDP-overlay validation test, and (2) the libgstmdpoverlay.so gst sink
# for the Atlas/WPE TouchPad target (gst 1.20.7, staging-glibc-252 / gcc125).
set -e
CC=/home/herrie/x-tools/arm-unknown-linux-gnueabi-gcc125/bin/arm-unknown-linux-gnueabi-gcc

# 1) standalone test
$CC -O2 -Wall -Wextra -static -o mdp_overlay_test mdp_overlay_test.c && echo "built mdp_overlay_test ($(ls -l mdp_overlay_test|awk '{print $5}'))"

# 2) gst sink plugin
export PKG_CONFIG_PATH=/home/herrie/webos/wpe/staging-glibc-252/lib/pkgconfig
CF=$(pkg-config --cflags gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0)
LF=$(pkg-config --libs gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0)
$CC -O2 -fPIC -shared -std=gnu99 -DPACKAGE='"mdpoverlay"' -DVERSION='"1.0"' \
    -o libgstmdpoverlay.so gstmdpoverlay.c $CF $LF
echo "built libgstmdpoverlay.so ($(md5sum libgstmdpoverlay.so | cut -d' ' -f1))"
# device deploy: put to the engine gstreamer plugin dir + the gsttest plugins dir
# novacom put file:///media/internal/wpe-252/lib/gstreamer-1.0/libgstmdpoverlay.so < libgstmdpoverlay.so
