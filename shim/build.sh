#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Generate + build amshim.dll and, only if AM_PREFIX is set in the environment, install + register
# it into that prefix (shim/install.sh sets it; without it this is a build-only step).
# The generator reads the metadata from tools/fetch-metadata.sh, which runs when it is missing.
# Environment: AM_PREFIX (install target), AM_PYTHON, AM_METADATA_DIR, AM_CACHE_DIR (bin/amd-env.sh);
# AM_SHIMS_BUILT=1 (internal, from setup-prefix.sh through shim/install.sh): amshim.dll was just
# built, only install it.
set -e
INSTALL_PREFIX="${AM_PREFIX-}"
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
ROOT="$(dirname "$HERE")"
. "$ROOT/bin/amd-env.sh"
# Wine's own headers (wine/debug.h) sit next to the tools: /usr/include for a distribution
# package, /opt/wine-*/include for a WineHQ build
WINE_INC="$(dirname "$(readlink -f "$(command -v winegcc)")")/../include"
cd "$HERE"
if [ "${AM_SHIMS_BUILT-}" != 1 ] || [ ! -f amshim.dll ]; then
    # overrides built by token pasting are invisible to the generator (it finds OV_* by regex)
    if grep -n 'OV_[A-Za-z0-9_]*##\|##[A-Za-z0-9_]*OV_' ov_*.c; then
        echo "error: macro-generated OV_ names above are never wired up; write plain functions" >&2; exit 1
    fi
    am_python
    "$ROOT/tools/fetch-metadata.sh"
    AM_METADATA_DIR="$AM_METADATA_DIR" "$AM_PYTHON" -I "$ROOT/tools/gen.py" .
    winegcc -b x86_64-windows -shared -O1 -g -Wall -Wno-unused-function -Wno-unused-variable \
        -Wno-incompatible-pointer-types -Wno-unused-but-set-variable \
        -D__WINESRC__ -idirafter "$WINE_INC" -idirafter /usr/include -I. \
        amshim_*.c ov_*.c gen/amshim_gen.c amshim.spec -lcombase -lole32 -lshell32 -luser32 -lwinhttp -lbcrypt -lcrypt32 -lurlmon -lshlwapi -loleaut32 -o amshim.dll
fi
[ -n "$INSTALL_PREFIX" ] || exit 0
export WINEPREFIX="$INSTALL_PREFIX"
cp amshim.dll "$WINEPREFIX/drive_c/windows/system32/amshim.dll"
{
  printf 'Windows Registry Editor Version 5.00\n\n'
  sed 's/#.*//' classes.txt | while read -r c; do
    [ -n "$c" ] || continue
    printf '[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\WindowsRuntime\\ActivatableClassId\\%s]\n' "$c"
    printf '"DllPath"="C:\\\\windows\\\\system32\\\\amshim.dll"\n"ActivationType"=dword:00000000\n"Threading"=dword:00000000\n\n'
  done
} > amshim.reg
# import from inside the prefix: a sandboxed prefix has no Z: drive for Unix paths
mkdir -p "$WINEPREFIX/drive_c/windows/temp"
cp amshim.reg "$WINEPREFIX/drive_c/windows/temp/amshim.reg"
WINEDEBUG=-all wine regedit /S 'C:\windows\temp\amshim.reg' 2>/dev/null
echo installed
