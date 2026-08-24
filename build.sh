#!/bin/bash
# Cross-compile the Atlas WPE backend to libWPEBackend-atlas.so (musl/ARM softfp).
set -e
# Toolchain env. Honour an already-sourced cross environment (STAGING/CC/TARGET exported); otherwise
# source one. WPE_ENV overrides; the default sits next to this checkout so the script works from any
# clone rather than only from the tree it was written in.
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
if [ -z "${STAGING:-}" ] || [ -z "${CC:-}" ]; then
    ENV_SH="${WPE_ENV:-$SCRIPT_DIR/../atlas-wpe-env/env-atlas-cross.sh}"
    [ -f "$ENV_SH" ] || { echo "build.sh: no cross env found at $ENV_SH - source it first or set WPE_ENV" >&2; exit 1; }
    . "$ENV_SH"
fi
cd "$SCRIPT_DIR"

# pkg-config: wpe-1.0/egl/glesv2/glib live in the staged webkitdeps tree (Debian multiarch layout),
# not in $STAGING/lib/pkgconfig, so the plain cross env does not resolve them. Mirror what
# cmake/atlas-arm-toolchain.cmake sets so this builds against exactly what WebKit built against -
# and never against the host's /usr.
if ! pkg-config --exists wpe-1.0 2>/dev/null; then
    WKDEPS="${ATLAS_WEBKITDEPS:-$STAGING/webkitdeps}"
    export PKG_CONFIG_SYSROOT_DIR="$WKDEPS"
    export PKG_CONFIG_LIBDIR="$WKDEPS/usr/lib/arm-linux-gnueabihf/pkgconfig:$WKDEPS/usr/share/pkgconfig:$STAGING/lib/pkgconfig"
    unset PKG_CONFIG_PATH
fi
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
# Optional: needs wpe-webkit-2.0 + libpng16 staged. The backend itself is the deliverable, so a
# missing harness dependency must not fail the build.
if ! pkg-config --exists wpe-webkit-2.0 libpng16 2>/dev/null; then
    echo "  skipped (wpe-webkit-2.0/libpng16 not in PKG_CONFIG_LIBDIR)"
    exit 0
fi
HCF=$(pkg-config --cflags wpe-webkit-2.0 wpe-1.0 glib-2.0 libpng16)
HLF=$(pkg-config --libs wpe-webkit-2.0 glib-2.0 libpng16)
$CC $CFLAGS -std=gnu11 -Wall $HCF \
    -o frame-dump frame-dump.c \
    -L. -lWPEBackend-atlas $HLF -Wl,-rpath-link,"$STAGING/lib"
echo "  built: $(ls -l frame-dump | awk '{print $5,$NF}')"
echo "  NEEDED: $($TARGET-readelf -d frame-dump | grep -oE '\[lib[^]]+\]' | tr '\n' ' ')"
