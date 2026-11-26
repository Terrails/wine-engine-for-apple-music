#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Set up (or refresh) a Wine prefix with the Apple Music for Windows engine and the compatibility
# shims. Safe to run again: every step checks or redoes its own work.
#
# usage: ./setup-prefix.sh <MusicPackage_<version>_x64.msix | AppleMusicWin .Msixbundle>
#        ./setup-prefix.sh --check      (only check the requirements; changes nothing)
#
# Environment (defaults in bin/amd-env.sh):
#   AM_PREFIX      the prefix to create/refresh (WINEPREFIX is ignored: this never touches your
#                  default Wine prefix)
#   AM_CACHE_DIR   unpacked package (msix/<version>/), Apple Root CA (applerootca/), metadata
#   AM_PYTHON      Python with dnfile (pip install -r requirements.txt)
#   AM_ALLOW_UNTESTED_MSIX=1   accept a package other than the tested one (see below)
#
# Steps:
#   - check the tools it needs (including a test compile with winegcc); refuse while the service
#     or the agent runs
#   - verify the package (version + SHA-256; a bundle has the x64 .msix extracted from it) and
#     unpack it into $AM_CACHE_DIR/msix/<version>
#   - download the Apple Root CA (apple.com, SHA-256 checked) and the Windows SDK / Windows App SDK
#     metadata (nuget.org, tools/fetch-metadata.sh), build the wrapper and all shims; nothing up
#     to here runs Wine or touches the prefix
#   - create the prefix (no desktop menu entries/file associations); winetricks vcrun2022 dxvk,
#     then sandbox; crash dialog off; no Bluetooth
#   - copy the app into "C:\Program Files\AppleMusic"; AMPLibraryAgent.exe gets ampkg.dll
#     prepended to its imports; the shims (shim/install.sh)
#   - COM registration of the agent (proxy DLL import patched; the .reg is generated from the
#     package's AppxManifest.xml by tools/genagentreg.py)
#   - the Apple Root CA into the prefix's trusted roots
# On failure it names the step that failed.
set -e
ROOT="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
. "$ROOT/bin/amd-env.sh"
export AM_PREFIX

# the tested package: its x64 .msix (a .Msixbundle is checked by the x64 .msix inside it)
MSIX_VERSION=1.1792.39942.0
MSIX_SHA256=f7638e28863fbd21c5c07eb4413f797762fd6f6fdaa0f17cc23a87fb46e7efb7     # MusicPackage_1.1792.39942.0_x64.msix
# Apple Root CA, https://www.apple.com/appleca/AppleIncRootCertificate.cer
CA_URL=https://www.apple.com/appleca/AppleIncRootCertificate.cer
CA_SHA256=b0b1730ecbc7ff4505142c49f1295e6eda6bcaed7e2c68c5be91b5a11001f024

STEP="checking the requirements"
say() { STEP="$*"; printf '==> %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
sha256() { sha256sum "$1" | cut -d' ' -f1; }

[ $# -eq 1 ] || { sed -n '6,7s/^# //p' "$ROOT/setup-prefix.sh" >&2; exit 2; }
CHECK=
case "$1" in
    --check) CHECK=1 ;;
    *) IN="$1"; [ -f "$IN" ] || die "$IN: no such file" ;;
esac

# what is left behind when the script stops half-way, and which step it stopped in
TMPD=
PARTIAL=
on_exit() {
    rc=$?
    [ -z "$TMPD" ] || rm -rf "$TMPD"
    [ -z "$PARTIAL" ] || rm -f "$PARTIAL"
    [ "$rc" = 0 ] || printf 'error: setup-prefix.sh failed (exit %s) at: %s\n' "$rc" "$STEP" >&2
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# ---- preflight ----
missing=
for t in wine wineboot wineserver winegcc widl winetricks unzip curl sha256sum cc make; do
    command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
command -v pactl >/dev/null 2>&1 || command -v pipewire-pulse >/dev/null 2>&1 ||
    missing="$missing pactl-or-pipewire-pulse(audio)"
command -v cc >/dev/null 2>&1 && ! printf '#include <pulse/stream.h>\n' | cc -x c -E - >/dev/null 2>&1 &&
    missing="$missing libpulse-headers(pulse/stream.h)"
if ! command -v "$AM_PYTHON" >/dev/null 2>&1; then
    missing="$missing $AM_PYTHON"
elif ! "$AM_PYTHON" -I -c 'import dnfile, pefile' 2>/dev/null; then
    missing="$missing dnfile(pip install -r requirements.txt; then AM_PYTHON=<venv>/bin/python)"
fi
# winegcc builds x86_64 Windows binaries with a PE cross compiler: clang with lld, or mingw-w64.
# Neither comes with Wine, so link a test program (in a temporary directory, no Wine run).
if command -v winegcc >/dev/null 2>&1; then
    TMPD="$(mktemp -d)"
    printf 'int main(void) { return 0; }\n' > "$TMPD/t.c"
    winegcc -b x86_64-windows "$TMPD/t.c" -o "$TMPD/t.exe" >/dev/null 2>&1 ||
        missing="$missing PE-cross-compiler(clang+lld-or-mingw-w64,for-winegcc)"
    rm -rf "$TMPD"; TMPD=
fi
[ -z "$missing" ] || die "missing:$missing (see docs/install.md#requirements)"
if [ -n "$CHECK" ]; then
    echo "all requirements found" >&2
    exit 0
fi
if am_busy; then
    die "the service or the agent is running; stop it first: bin/amctl quit"
fi

# ---- the package ----
manifest_field() {   # <AppxManifest.xml text on stdin> <attribute>: from the <Identity> element
    tr '\r\n\t' '   ' | grep -o '<Identity [^>]*>' | head -1 | sed -n "s/.* $1=\"\([^\"]*\)\".*/\1/p"
}
say "checking $IN"
MSIX="$IN"
if unzip -l "$IN" AppxMetadata/AppxBundleManifest.xml >/dev/null 2>&1; then
    KIND=bundle
    # the bundle's x64 application package: its file name and version
    PKG="$(unzip -p "$IN" AppxMetadata/AppxBundleManifest.xml | tr '\r\n\t' '   ' | grep -o '<Package [^>]*>' |
        grep 'Architecture="x64"' | grep 'Type="application"' | head -1)"
    INNER="$(printf '%s\n' "$PKG" | sed -n 's/.* FileName="\([^"]*\)".*/\1/p')"
    VERSION="$(printf '%s\n' "$PKG" | sed -n 's/.* Version="\([^"]*\)".*/\1/p')"
    [ -n "$INNER" ] || die "$IN: bundle without an x64 application package"
    # the x64 .msix is what gets checked and unpacked; the temporary copy is removed on exit
    mkdir -p "$AM_CACHE_DIR/msix"
    TMPD="$(mktemp -d "$AM_CACHE_DIR/msix/.bundle.XXXXXX")"
    MSIX="$TMPD/package.msix"
    unzip -p "$IN" "$INNER" > "$MSIX" || die "$IN: cannot extract $INNER"
else
    KIND=msix
    unzip -l "$IN" AppxManifest.xml >/dev/null 2>&1 || die "$IN: not an .msix or .Msixbundle"
    VERSION="$(unzip -p "$IN" AppxManifest.xml | manifest_field Version)"
fi
PKG_SUM="$(sha256 "$MSIX")"
[ "$KIND" = msix ] || echo "$INNER inside the bundle: SHA-256 $PKG_SUM" >&2
if [ "$PKG_SUM" != "$MSIX_SHA256" ] || [ "$VERSION" != "$MSIX_VERSION" ]; then
    echo "package: version $VERSION, x64 .msix SHA-256 $PKG_SUM" >&2
    echo "tested:  version $MSIX_VERSION, x64 .msix SHA-256 $MSIX_SHA256" >&2
    [ "$AM_ALLOW_UNTESTED_MSIX" = 1 ] ||
        die "untested package; set AM_ALLOW_UNTESTED_MSIX=1 to try it anyway (the shims and hard-coded interface IDs may not match)"
    echo "warning: AM_ALLOW_UNTESTED_MSIX=1: continuing with an untested package" >&2
fi
case "$VERSION" in ''|*/*|*..*) die "$IN: no usable version in the manifest" ;; esac

# the unpacked copy is recorded with the .msix hash
STAGE="$AM_CACHE_DIR/msix/$VERSION"
if [ "$(cat "$STAGE.complete" 2>/dev/null)" != "$PKG_SUM" ] || [ ! -f "$STAGE/AppxManifest.xml" ]; then
    say "unpacking into $STAGE"
    mkdir -p "$AM_CACHE_DIR/msix"
    rm -rf "$STAGE.tmp" "$STAGE.tmp.msix" "$STAGE.complete"
    mkdir -p "$STAGE.tmp"
    unzip -q -o "$MSIX" -d "$STAGE.tmp"
    [ "$(manifest_field ProcessorArchitecture < "$STAGE.tmp/AppxManifest.xml")" = x64 ] || die "not an x64 package"
    [ "$(manifest_field Version < "$STAGE.tmp/AppxManifest.xml")" = "$VERSION" ] || die "package version mismatch"
    [ -f "$STAGE.tmp/AMPLibraryAgent.exe" ] || die "no AMPLibraryAgent.exe in the package"
    rm -rf "$STAGE"
    mv "$STAGE.tmp" "$STAGE"
    printf '%s\n' "$PKG_SUM" > "$STAGE.complete"
fi
[ "$KIND" = msix ] || { rm -rf "$TMPD"; TMPD=; }

# ---- Apple Root CA ----
# Apple's sign-in server (gsa.apple.com) uses a certificate from Apple's own authority, which the
# Linux CA bundle Wine relies on does not contain; without it the TLS connection is dropped and
# sign-in "fails" before sending anything.
CA="$AM_CACHE_DIR/applerootca/AppleIncRootCertificate.cer"
if [ ! -f "$CA" ] || [ "$(sha256 "$CA")" != "$CA_SHA256" ]; then
    say "downloading the Apple Root CA"
    mkdir -p "$(dirname "$CA")"
    curl -fL --proto '=https' --tlsv1.2 --retry 3 -sS -o "$CA.part" "$CA_URL"
    got="$(sha256 "$CA.part")"
    [ "$got" = "$CA_SHA256" ] || { rm -f "$CA.part"; die "Apple Root CA: SHA-256 $got, expected $CA_SHA256"; }
    mv -f "$CA.part" "$CA"
fi

# ---- build (nothing here touches the prefix) ----
# Every download and every build happens here, before the first Wine command, so a network or
# toolchain error stops the script before the prefix is changed. AM_PREFIX and WINEPREFIX are
# unset for the shim builds: with AM_PREFIX set, shim/build.sh and shim/ps/build.sh also install.
say "Windows SDK and Windows App SDK metadata (tools/fetch-metadata.sh)"
"$ROOT/tools/fetch-metadata.sh"
say "building the wrapper, the shims and addroot.exe"
STEP="building amshim.dll (shim/build.sh)"
( unset AM_PREFIX WINEPREFIX; "$ROOT/shim/build.sh" )
STEP="building amps.dll (shim/ps/build.sh)"
( unset AM_PREFIX WINEPREFIX; "$ROOT/shim/ps/build.sh" )
STEP="building ampkg.dll and the stubs"
"$ROOT/shim/ampkg/build.sh"
"$ROOT/shim/build-stubs.sh"
STEP="building addroot.exe"
mkdir -p "$ROOT/build"
winegcc -b x86_64-windows -municode -O1 "$ROOT/tools/addroot.c" -lcrypt32 -o "$ROOT/build/addroot.exe"
STEP="building the wrapper (make -C wrapper)"
make -s -C "$ROOT/wrapper"

# ---- the prefix ----
say "prefix $AM_PREFIX"
mkdir -p "$(dirname "$AM_PREFIX")"
# No desktop integration: winemenubuilder would add menu entries and file associations
# (wine-extension-*.desktop, MIME types) for this prefix to your desktop. Disabled for every Wine
# program this script starts, and in the prefix itself for later runs.
export WINEDLLOVERRIDES="winemenubuilder.exe=d${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
STEP="creating the prefix (wineboot)"
[ -d "$WINEPREFIX/drive_c" ] || WINEDEBUG=-all wineboot -i
STEP="wine reg add: winemenubuilder.exe disabled"
WINEDEBUG=-all wine reg add 'HKCU\Software\Wine\DllOverrides' /v winemenubuilder.exe /d '' /f >/dev/null 2>&1
# Official/community components first, so everything below runs against the final layout.
# sandbox: unlink the prefix's Documents/Music/... from the real home and drop the Z: drive, so
# the app's library ("Music\Apple Music") and anything else it writes stay inside the prefix.
# The steps below therefore pass only C: paths to Wine programs.
STEP="winetricks -q vcrun2022 dxvk"
WINEDEBUG=-all winetricks -q vcrun2022 dxvk
STEP="winetricks -q sandbox"
WINEDEBUG=-all winetricks -q sandbox
# no "program crashed" dialog: crashes are in the logs
STEP="wine reg add: crash dialog off"
WINEDEBUG=-all wine reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >/dev/null 2>&1
# No Bluetooth driver: winebth.sys records every device the host sees under
# HKLM\System\CurrentControlSet\Enum\WINEBTH and never forgets one; with randomised BLE addresses
# around, that grows to thousands of devices and tens of MB of system.reg. wineserver rewrites the
# registry every 30 s and blocks every Wine thread meanwhile, so playback drops out for 0.1-0.6 s
# (seconds under load). The agent needs no Bluetooth. Services\winebth Start=4 does not stop it
# (it is a PnP bus driver); a disabled DLL override does. Already recorded devices are deleted.
STEP="wine reg add: winebth.sys disabled"
WINEDEBUG=-all wine reg add 'HKCU\Software\Wine\DllOverrides' /v winebth.sys /d '' /f >/dev/null 2>&1
WINEDEBUG=-all wine reg delete 'HKLM\System\CurrentControlSet\Enum\WINEBTH' /f >/dev/null 2>&1 || true

# ---- the app ----
say "installing the app into $APPDIR"
mkdir -p "$APPDIR"
cp -a "$STAGE/." "$APPDIR/"
# ampkg.dll first in the agent's imports (fake MSIX package identity + missing ntdll APIs)
"$AM_PYTHON" -I "$ROOT/tools/addimport.py" "$STAGE/AMPLibraryAgent.exe" "$APPDIR/AMPLibraryAgent.exe" ampkg.dll ampkg_init

# all shims: ampkg.dll, amshim.dll (WinRT classes missing from Wine), amps.dll
# (IInspectable/IAsyncInfo proxy/stubs, which Wine registers none of), amcertreq.dll,
# webauthn/bcp47mrm stubs; all built above, so it only installs them
say "installing the shims (shim/install.sh)"
AM_SHIMS_BUILT=1 "$ROOT/shim/install.sh"

# COM activation of the agent from a client:
# - Apple's proxy DLL imports rpcrt4!NdrStubCall3, which Wine lacks; NdrStubCall2 has the same
#   signature and the DLL carries DCE format strings, so the import name is patched
# - LocalServer32 + interface registrations: what the package manifest declares, written by
#   tools/genagentreg.py from the user's own AppxManifest.xml (nothing installs the MSIX here)
say "registering the agent's COM classes"
"$AM_PYTHON" -I - "$STAGE/AMPLibraryAgent.Proxies.dll" "$APPDIR/AMPLibraryAgent.Proxies.dll" <<'PYEOF'
import sys
d = open(sys.argv[1], 'rb').read()
assert d.count(b'NdrStubCall3\0') == 1
open(sys.argv[2], 'wb').write(d.replace(b'NdrStubCall3\0', b'NdrStubCall2\0'))
PYEOF
mkdir -p "$WINEPREFIX/drive_c/windows/temp"
"$AM_PYTHON" -I "$ROOT/tools/genagentreg.py" "$STAGE/AppxManifest.xml" \
    "$WINEPREFIX/drive_c/windows/temp/agent.reg"   # imported from C:, no Z: drive once sandboxed
STEP="wine regedit: agent.reg"
WINEDEBUG=-all wine regedit /S 'C:\windows\temp\agent.reg' 2>/dev/null

# Apple Root CA into the prefix's trusted roots (replaces an existing copy)
say "adding the Apple Root CA to the prefix"
cp "$ROOT/build/addroot.exe" "$CA" "$WINEPREFIX/drive_c/windows/temp/"
WINEDEBUG=-all wine 'C:\windows\temp\addroot.exe' 'C:\windows\temp\AppleIncRootCertificate.cer'

wineserver -k 2>/dev/null || true
cat <<EOF
done: $AM_PREFIX
next:
  1. sign in once:   $ROOT/bin/amd-signin
  2. the service:    $ROOT/bin/amd      (or start a UI that runs it, e.g. the Sidra fork)
     control it:     $ROOT/bin/amctl status
EOF
[ "$AM_PREFIX" = "${XDG_DATA_HOME:-$HOME/.local/share}/$AM_NAME/prefix" ] ||
    echo "  (use the same AM_PREFIX=$AM_PREFIX for these)"
