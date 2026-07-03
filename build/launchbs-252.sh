#!/bin/sh
# Launch the 2.52 BrowserServer-wpe against the wpe-252 HYBRID runtime (gcc-9.3 glibc + gcc-12.5 libstdc++).
luna-send -n 1 'palm://com.palm.power/com/palm/power/activityStart' '{"id":"wpe","duration_ms":600000}' >/dev/null 2>&1
luna-send -n 1 'palm://com.palm.display/control/setState' '{"state":"on"}' >/dev/null 2>&1
stop atlas 2>/dev/null
for p in $(ps -ef 2>/dev/null|grep BrowserServer-wpe|grep -v grep|awk '{print $2}'); do kill -9 $p 2>/dev/null; done
sleep 1; rm -f /tmp/bpwpe.log /tmp/bs.log
D=/media/internal/wpe-252; ATLAS=/media/internal/atlas
cp -f $D/lib/libWPEBackend-atlas.so $D/lib/libWPEBackend-default.so 2>/dev/null
[ -f $D/fonts.conf ] || cp /media/internal/wpe-238/fonts.conf $D/fonts.conf 2>/dev/null
cd $ATLAS
nohup env BPWPE_DEBUG=1 LD_LIBRARY_PATH=$D/lib:$ATLAS:/usr/lib/ssl11:/usr/lib:/lib WPE_BACKEND_LIBRARY=$D/lib/libWPEBackend-atlas.so QT_QPA_PLATFORM=Minimal QT_QPA_PLATFORM_PLUGIN_PATH=/usr/plugins/platforms QT_PLUGIN_PATH=/usr/plugins FONTCONFIG_FILE=$D/fonts.conf GIO_MODULE_DIR=$D/lib/gio/modules GIO_USE_TLS=openssl SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt $D/BrowserServer-wpe -platform Minimal >/tmp/bs.log 2>&1 &
sleep 6
echo "BS-wpe(2.52) procs: $(ps -ef 2>/dev/null|grep BrowserServer-wpe|grep -v grep|wc -l) | socket: $(ls /tmp/yapserver.atlas 2>/dev/null || echo NONE)"
echo "=== bs.log ==="
tail -30 /tmp/bs.log 2>/dev/null
