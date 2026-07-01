#!/bin/bash
# Cross-compile the Isis WPE backend to libWPEBackend-isis.so (musl/ARM softfp).
set -e
WPE=/home/herrie/webos/wpe; . "${WPE_ENV:-$WPE/env-glibc-gcc125.sh}"
cd "$WPE/backend-isis"
PKGS="wpe-1.0 glib-2.0 egl glesv2"
CF=$(pkg-config --cflags $PKGS)
LF=$(pkg-config --libs $PKGS)
$CC $CFLAGS -std=gnu11 -fPIC -fvisibility=hidden -Wall -Wextra $CF \
    -shared -Wl,-soname,libWPEBackend-isis.so \
    -o libWPEBackend-isis.so wpe-isis-backend.c \
    $LF -Wl,-rpath-link,"$STAGING/lib"
echo "=== built ==="
ls -l libWPEBackend-isis.so | awk '{print " ",$5,$NF}'
$TARGET-readelf -hA libWPEBackend-isis.so | grep -iE 'Machine|soft-float' | sed 's/^/  /'
echo "  exports _wpe_loader_interface: $($TARGET-nm -D libWPEBackend-isis.so | grep -c _wpe_loader_interface)"
echo "  NEEDED: $($TARGET-readelf -d libWPEBackend-isis.so | grep -oE '\[lib[^]]+\]' | tr '\n' ' ')"

echo "=== building frame-dump test harness ==="
HCF=$(pkg-config --cflags wpe-webkit-2.0 wpe-1.0 glib-2.0 libpng16)
HLF=$(pkg-config --libs wpe-webkit-2.0 glib-2.0 libpng16)
$CC $CFLAGS -std=gnu11 -Wall $HCF \
    -o frame-dump frame-dump.c \
    -L. -lWPEBackend-isis $HLF -Wl,-rpath-link,"$STAGING/lib"
echo "  built: $(ls -l frame-dump | awk '{print $5,$NF}')"
echo "  NEEDED: $($TARGET-readelf -d frame-dump | grep -oE '\[lib[^]]+\]' | tr '\n' ' ')"
