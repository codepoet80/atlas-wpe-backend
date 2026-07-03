#!/bin/bash
# Cross-build the Atlas BrowserServer with the WPE WebKit engine (glibc-2.23 toolchain).
# BrowserPage is the WPE implementation (src/BrowserPage.{h,cpp} = merged BrowserPageWPE); the QtWebKit
# engine (libWebKitLuna/v8) is dropped. QtWebKit headers are pulled compile-only (QT_NO_KEYWORDS) for the
# legacy interface TYPES (QWebHitTestResult etc.) — see §8 of BROWSERSERVER-INTEGRATION-PLAN.md for the
# de-QtWebKit cleanup. Iterative, like the dep stack.
set -u
WPE=/home/herrie/webos/wpe; DS=/home/herrie/webos/touchpad-kernel/doctor305; BS=$DS/BrowserServer
SRC=$WPE/browserserver-wpe/src; OBJ=$WPE/browserserver-wpe/obj; L=$WPE/logs
. "${WPE_ENV:-$WPE/env-glibc.sh}"
mkdir -p "$OBJ"
[ -d "$SRC" ] || cp -r "$BS/Src" "$SRC"
# re-merge src/BrowserPage.{h,cpp} from the canonical backend-atlas/BrowserPageWPE.* (orig enums + WPE class)
python3 - "$BS" "$WPE" "$SRC" <<'PYMERGE'
import sys
BS,WPE,SRC=sys.argv[1:4]
def block(t,n):
    i=t.index('class '+n); j=t.index('{',i); d=0; k=j
    while k<len(t):
        c=t[k]
        if c=='{': d+=1
        elif c=='}':
            d-=1
            if d==0: return i, t.index(';',k)+1
        k+=1
orig=open(f'{BS}/Src/BrowserPage.h').read(); wpe=open(f'{WPE}/backend-atlas/BrowserPageWPE.h').read()
o0,o1=block(orig,'BrowserPage'); w0,w1=block(wpe,'BrowserPageWPE')
wc=wpe[w0:w1].replace('BrowserPageWPE','BrowserPage')
open(f'{SRC}/BrowserPage.h','w').write(orig[:o0]+'#include <wpe/webkit.h>\n#include <semaphore.h>\n\n'+wc+orig[o1:])
open(f'{SRC}/BrowserPage.cpp','w').write(open(f'{WPE}/backend-atlas/BrowserPageWPE.cpp').read().replace('BrowserPageWPE','BrowserPage'))
PYMERGE
SI=$DS/build-deps/staging/include
CF=$(pkg-config --cflags wpe-webkit-2.0 wpe-1.0 glib-2.0 2>/dev/null)
INC="-I$WPE/browserserver-wpe -I$SRC -I$BS/Yap -I$WPE/backend-atlas \
 -I$SI -I$SI/webkit -I$SI/QtCore -I$SI/QtGui -I$SI/QtNetwork -I$SI/QtWebKit -I$SI/WebKitSupplemental \
 -I$DS/build-deps/WebKitSupplemental/misc"
DEF="-DQT_NO_KEYWORDS -DNDEBUG -D_GLIBCXX_USE_CXX11_ABI=0 -Demit="
CXXF="-std=c++11 -fPIC -fpermissive -Wno-deprecated $DEF $INC $CF"

# QtWebKit-only units to EXCLUDE (replaced by WPE or not needed for first paint), + the moc files (regen later)
EXCLUDE="BackupManager BrowserComboBox WebKitEventListener WebOSPlatformPlugin qwebkitplatformplugin"

compile_one(){ local cpp="$1"; local n; n=$(basename "$cpp" .cpp)
  for x in $EXCLUDE; do [ "$n" = "$x" ] && { echo "SKIP $n (QtWebKit-only)"; return 0; }; done
  case "$n" in *.moc) echo "SKIP $n (moc)"; return 0;; esac
  if $CXX $CXXF -c "$cpp" -o "$OBJ/$n.o" >"$L/bs-$n.log" 2>&1; then echo "OK  $n"
  else echo "FAIL $n  ($(grep -c 'error:' "$L/bs-$n.log") errors)"; return 1; fi
}

if [ "${1:-all}" = all ]; then
  fails=0
  for cpp in "$SRC"/*.cpp; do
    case "$(basename "$cpp")" in *.moc.cpp) continue;; esac
    compile_one "$cpp" || fails=$((fails+1))
  done
  echo "===== compiled $(ls "$OBJ"/*.o 2>/dev/null | wc -l) objects, $fails failures ====="
else
  compile_one "$SRC/$1.cpp"
fi
