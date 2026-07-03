#!/bin/sh
# WPE 2.52 BrowserServer BOOT wrapper. Deployed to /media/internal/atlas/BrowserServer and exec'd by the
# `atlas` upstart job (which passes `-platform qbs`; we ignore it and force Minimal). This is the
# file that must live at /media/internal/atlas/BrowserServer — the real ELF is $D/BrowserServer-wpe.
#
# Two things ONLY this wrapper does (both were lost when the raw ELF was mistakenly put over this path):
#   1. GIO_MODULE_DIR + GIO_USE_TLS=openssl  -> the GnuTLS/OpenSSL GIO module (libgioopenssl.so) that
#      provides the TLS backend. Without it every https load fails "TLS support is not available".
#   2. mount -o remount,size=131072k /dev    -> /dev tmpfs (holds /dev/shm) is 2MB by default; WebKit's
#      IPC SharedMemory overflows it (SIGBUS / white past ~1/3 scroll). 128MB fixes it.
#
# The upstart job also launches a second `-platform offscreen` instance; guard against it (exit 0).
case "$*" in *offscreen*) exit 0 ;; esac

# Boot-safe: don't start before the system UI (too early => no GL => blank pages).
i=0; while [ $i -lt 90 ] && ! ps -ef 2>/dev/null | grep -q '[L]unaSysMgr'; do sleep 1; i=$((i+1)); done
sleep 3

# WebKit IPC SharedMemory lives in /dev/shm (a dir on the /dev tmpfs). Enlarge 2MB -> 128MB.
mount -o remount,size=131072k /dev 2>/dev/null

unset LD_PRELOAD
D=/media/internal/wpe-252; ATLAS=/media/internal/atlas
cp -f $D/lib/libWPEBackend-atlas.so $D/lib/libWPEBackend-default.so 2>/dev/null
[ -f $D/fonts.conf ] || cp /media/internal/wpe-238/fonts.conf $D/fonts.conf 2>/dev/null

export BPWPE_DEBUG=1
export LD_LIBRARY_PATH=$D/lib:$ATLAS:/usr/lib/ssl11:/usr/lib:/lib
export WPE_BACKEND_LIBRARY=$D/lib/libWPEBackend-atlas.so
export QT_QPA_PLATFORM=Minimal QT_QPA_PLATFORM_PLUGIN_PATH=/usr/plugins/platforms QT_PLUGIN_PATH=/usr/plugins
export FONTCONFIG_FILE=$D/fonts.conf GIO_MODULE_DIR=$D/lib/gio/modules GIO_USE_TLS=openssl SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt

cd $ATLAS
rm -f /tmp/yapserver.atlas*
exec $D/BrowserServer-wpe -platform Minimal >/tmp/bs.log 2>&1
