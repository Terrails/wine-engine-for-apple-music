#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# amps.dll: proxy/stub DLL for IInspectable + IAsyncInfo (installed + regsvr32'd into $AM_PREFIX
# only if AM_PREFIX is set in the environment; shim/install.sh sets it).
# widl needs Wine's IDL include directory (oaidl.idl, inspectable.idl ...). Its location differs
# between packages; WINE_INCLUDE_DIR=<dir> overrides the search below.
# AM_SHIMS_BUILT=1 (internal, from setup-prefix.sh through shim/install.sh): amps.dll was just
# built, only install it.
set -e
cd "$(dirname "$(readlink -f "$0")")"
# Wine's own headers (wine/debug.h) sit next to the tools: /usr/include for a distribution
# package, /opt/wine-*/include for a WineHQ build
WINE_INC="$(dirname "$(readlink -f "$(command -v winegcc)")")/../include"

wine_idl_dir() {
    if [ -n "$WINE_INCLUDE_DIR" ]; then
        [ -f "$WINE_INCLUDE_DIR/inspectable.idl" ] && { echo "$WINE_INCLUDE_DIR"; return 0; }
        echo "error: WINE_INCLUDE_DIR=$WINE_INCLUDE_DIR has no inspectable.idl" >&2
        return 1
    fi
    # next to the tools first (winegcc does the same: <bindir>/../include/wine/windows), so a
    # WineHQ build in /opt/wine-* on PATH wins over a distribution package
    bin="$(dirname "$(readlink -f "$(command -v widl)")")"
    for d in "$bin/../include/wine/windows" \
             "$bin/../include/wine/wine/windows" \
             /usr/include/wine/windows \
             /usr/include/wine/wine/windows \
             /usr/local/include/wine/windows \
             /usr/include/wine-development/wine/windows \
             /opt/wine-staging/include/wine/windows \
             /opt/wine-devel/include/wine/windows \
             /opt/wine-stable/include/wine/windows; do
        [ -f "$d/inspectable.idl" ] && { (cd "$d" && pwd); return 0; }
    done
    echo "error: Wine's IDL headers (inspectable.idl) not found. Install Wine's development files:" >&2
    echo "  Arch: wine   Fedora (WineHQ): wine-devel-devel   Debian/Ubuntu (WineHQ): wine-devel-dev" >&2
    echo "  (see docs/install.md#requirements)" >&2
    echo "or set WINE_INCLUDE_DIR to the directory that holds inspectable.idl." >&2
    return 1
}

if [ "${AM_SHIMS_BUILT-}" != 1 ] || [ ! -f amps.dll ]; then
    command -v widl >/dev/null 2>&1 || { echo "error: widl not found (Wine development tools)" >&2; exit 1; }
    I="$(wine_idl_dir)"
    widl -I"$I" -m64 --win64 -h -H amps.h amps.idl
    widl -I"$I" -m64 --win64 -p -P amps_p.c amps.idl
    widl -I"$I" -m64 --win64 -u -U amps_i.c amps.idl
    widl -I"$I" -m64 --win64 --dlldata-only --dlldata=dlldata.c amps
    # PROXY_CLSID_IS: the proxy/stub class ID regsvr32 registers for the interfaces in amps.idl
    # ({38d27ce3-b664-433d-a89d-a5a25a7dc55b}, a random GUID that belongs to this project)
    winegcc -b x86_64-windows -shared -O1 -g -D__WINESRC__ -DREGISTER_PROXY_DLL -DPROXY_CLSID_IS='{0x38d27ce3,0xb664,0x433d,{0xa8,0x9d,0xa5,0xa2,0x5a,0x7d,0xc5,0x5b}}' \
        -idirafter "$WINE_INC" -idirafter /usr/include -I. amps_p.c amps_i.c dlldata.c amps.spec \
        -lrpcrt4 -lcombase -lole32 -luuid -o amps.dll
fi
[ -n "$AM_PREFIX" ] || exit 0
export WINEPREFIX="$AM_PREFIX"
cp amps.dll "$WINEPREFIX/drive_c/windows/system32/amps.dll"
WINEDEBUG=-all wine regsvr32 /s amps.dll
echo "amps installed"
