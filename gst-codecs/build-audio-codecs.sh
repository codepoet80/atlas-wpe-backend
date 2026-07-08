#!/bin/bash
# Build the extra GStreamer audio-codec plugins that WPE WebKit needs for the
# Atlas browser but that build-all.sh ships DISABLED (gstbase is configured
# -Dogg=disabled -Dvorbis=disabled -Dopus=disabled -Dtheora=disabled).
#
# Produces, into $STAGING and the gst-plugins-base build dir:
#   libogg.so.0        (xiph libogg 1.3.5, autotools)
#   libvorbis.so.0     (xiph libvorbis 1.3.7, autotools; needs libogg)
#   libvorbisenc.so.2  (pulled in NEEDED by libgstvorbis.so)
#   libgstogg.so       (gst-plugins-base ext/ogg   -> oggdemux/oggparse)
#   libgstvorbis.so    (gst-plugins-base ext/vorbis -> vorbisdec/vorbisparse)
#
# Opus (avdec_opus) and FLAC (avdec_flac/flacparse) already come from gst-libav,
# so once the Ogg *container* is present all four Ogg audio codecs work. See
# ./README.md for WHY the Ogg container alone wasn't enough (WebKit scanner gate).
#
# Usage: WPE_ENV=env-glibc-gcc125.sh ./build-audio-codecs.sh
set -e
WPE=/home/herrie/webos/wpe
. "${WPE_ENV:-$WPE/env-glibc-gcc125.sh}"   # exports TARGET STAGING CC CFLAGS PKG_CONFIG_* WPE_MESON_CROSS
SRC=$WPE/src ; BUILD=$WPE/build
GSTBASE=$BUILD/gst-plugins-base-1.20.7

dl() { [ -f "$SRC/$2" ] || wget -O "$SRC/$2" "$1"; }

echo ">>> libogg 1.3.5"
dl https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.xz libogg-1.3.5.tar.xz
[ -d "$BUILD/libogg-1.3.5" ] || tar -C "$BUILD" -xf "$SRC/libogg-1.3.5.tar.xz"
cd "$BUILD/libogg-1.3.5"
./configure --host=$TARGET --prefix=$STAGING --disable-static --enable-shared
make -j"$(nproc)" && make install

echo ">>> libvorbis 1.3.7 (needs libogg in \$STAGING)"
dl https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz libvorbis-1.3.7.tar.xz
[ -d "$BUILD/libvorbis-1.3.7" ] || tar -C "$BUILD" -xf "$SRC/libvorbis-1.3.7.tar.xz"
cd "$BUILD/libvorbis-1.3.7"
./configure --host=$TARGET --prefix=$STAGING --with-ogg=$STAGING --disable-static --enable-shared
make -j"$(nproc)" && make install

echo ">>> gst-plugins-base ext/ogg + ext/vorbis (flip both enabled in the existing _b)"
cd "$GSTBASE"
meson configure _b -Dogg=enabled -Dvorbis=enabled
ninja -C _b ext/ogg/libgstogg.so ext/vorbis/libgstvorbis.so

echo "=== built ==="
ls -l "$STAGING/lib/libogg.so.0."* "$STAGING/lib/libvorbis.so.0."* "$STAGING/lib/libvorbisenc.so.2."* 2>/dev/null | awk '{print " ",$5,$NF}'
ls -l "$GSTBASE"/_b/ext/ogg/libgstogg.so "$GSTBASE"/_b/ext/vorbis/libgstvorbis.so | awk '{print " ",$5,$NF}'
echo "Now run ./deploy-audio-codecs.sh to push these to the device and rescan the registry."
