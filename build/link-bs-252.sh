#!/bin/bash
# Relink BrowserServer-wpe from the prebuilt .o's. Single source of truth for the link line so the
# Qt-removal audit can drop -lQt* deps here and verify 0 undefined references.
set -e
WPE=/home/herrie/webos/wpe
DS=/home/herrie/webos/touchpad-kernel/doctor305
STAGING=$WPE/staging-glibc-252
OBJ=$WPE/browserserver-wpe/obj
DEVLIB=$WPE/browserserver-wpe/devlib
RL=$DS/untouched-rootfs
. $WPE/env-glibc-gcc125.sh 2>/dev/null

$CXX -o $OBJ/BrowserServer-wpe $OBJ/*.o \
  -Wl,--dynamic-linker=/media/internal/wpe-252/lib/ld-linux.so.3 \
  -Wl,-rpath=/media/internal/wpe-252/lib:/media/internal/atlas:/usr/lib:/lib \
  -L$STAGING/lib -L$WPE/backend-atlas -L$DEVLIB \
  -lWPEWebKit-2.0 -lwpe-1.0 -lWPEBackend-atlas \
  -lglib-2.0 -lgobject-2.0 -lgio-2.0 -lgthread-2.0 -lpng16 \
  -lQtCore -lQtGui -lQtNetwork \
  -lssl -lcrypto -llunaservice -lpbnjson_cpp -lpbnjson_c -lyajl -luriparser \
  -lcjson -lmjson -laffinity -lmemchute -lPmCertificateMgr -leventreporter \
  -lpthread -lrt -ldl \
  -Wl,-rpath-link,$STAGING/lib -Wl,-rpath-link,$RL/usr/lib -Wl,-rpath-link,$RL/lib \
  > $WPE/logs/bs-link-252.log 2>&1
rc=$?
echo "relink rc=$rc undefs=$(grep -c 'undefined ref' $WPE/logs/bs-link-252.log)"
grep 'undefined ref' $WPE/logs/bs-link-252.log | head -8
exit $rc
