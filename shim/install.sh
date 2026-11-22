#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Build the shims and install them into the prefix (AM_PREFIX, default as in bin/amd-env.sh):
#   ampkg.dll  -> C:\Program Files\AppleMusic\  (next to AMPLibraryAgent.exe, which imports it)
#   amshim.dll -> C:\windows\system32\ + its WinRT class registrations (shim/build.sh)
#   amps.dll   -> C:\windows\system32\ + regsvr32 (shim/ps/build.sh)
#   webauthn.dll, bcp47mrm.dll -> C:\windows\system32\ (stubs Wine lacks: passkeys, BCP-47 tag check)
#   amcertreq.dll -> C:\windows\system32\ + regsvr32 (crypt32 encoder for certificate requests, push)
# The service and the agent must not be running (they have the DLLs loaded): bin/amctl quit
# usage: [AM_PREFIX=<prefix>] shim/install.sh
# AM_SHIMS_BUILT=1 (internal, set by setup-prefix.sh, which has just built all of them): install
# only, without building again.
set -e
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
ROOT="$(dirname "$HERE")"
. "$ROOT/bin/amd-env.sh"
export AM_PREFIX
[ -d "$APPDIR" ] || { echo "no app in $APPDIR (run setup-prefix.sh first)" >&2; exit 1; }
if am_busy; then
    echo "the service or the agent is running; stop it first: bin/amctl quit" >&2
    exit 1
fi
[ "${AM_SHIMS_BUILT-}" = 1 ] || "$HERE/ampkg/build.sh"
cp -f "$HERE/ampkg/ampkg.dll" "$APPDIR/"
echo "ampkg installed"
"$HERE/build.sh"
"$HERE/ps/build.sh"
[ "${AM_SHIMS_BUILT-}" = 1 ] || "$HERE/build-stubs.sh"
for d in webauthn bcp47mrm; do
    cp -f "$HERE/$d/$d.dll" "$AM_PREFIX/drive_c/windows/system32/"
done
echo "webauthn, bcp47mrm installed"
cp -f "$HERE/certreq/amcertreq.dll" "$AM_PREFIX/drive_c/windows/system32/"
WINEPREFIX="$AM_PREFIX" WINEDEBUG=-all wine regsvr32 /s amcertreq.dll
echo "amcertreq installed"
