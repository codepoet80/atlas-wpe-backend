#!/bin/bash
# Cross-compile the Atlas WPE backend to libWPEBackend-atlas.so (musl/ARM softfp).
set -e
WPE=/home/herrie/webos/wpe; . "${WPE_ENV:-$WPE/env-glibc-gcc125.sh}"
cd "$WPE/backend-atlas"
PKGS="wpe-1.0 glib-2.0 egl glesv2"
CF=$(pkg-config --cflags $PKGS)
LF=$(pkg-config --libs $PKGS)
$CC $CFLAGS -std=gnu11 -fPIC -fvisibility=hidden -Wall -Wextra $CF \
    -shared -Wl,-soname,libWPEBackend-atlas.so \
    -o libWPEBackend-atlas.so wpe-atlas-backend.c \
    $LF -Wl,-rpath-link,"$STAGING/lib"
echo "=== built ==="
ls -l libWPEBackend-atlas.so | awk '{print " ",$5,$NF}'
$TARGET-readelf -hA libWPEBackend-atlas.so | grep -iE 'Machine|soft-float' | sed 's/^/  /'
echo "  exports _wpe_loader_interface: $($TARGET-nm -D libWPEBackend-atlas.so | grep -c _wpe_loader_interface)"
echo "  NEEDED: $($TARGET-readelf -d libWPEBackend-atlas.so | grep -oE '\[lib[^]]+\]' | tr '\n' ' ')"

echo "=== building frame-dump test harness ==="
HCF=$(pkg-config --cflags wpe-webkit-2.0 wpe-1.0 glib-2.0 libpng16)
HLF=$(pkg-config --libs wpe-webkit-2.0 glib-2.0 libpng16)
$CC $CFLAGS -std=gnu11 -Wall $HCF \
    -o frame-dump frame-dump.c \
    -L. -lWPEBackend-atlas $HLF -Wl,-rpath-link,"$STAGING/lib"
echo "  built: $(ls -l frame-dump | awk '{print $5,$NF}')"
echo "  NEEDED: $($TARGET-readelf -d frame-dump | grep -oE '\[lib[^]]+\]' | tr '\n' ' ')"
