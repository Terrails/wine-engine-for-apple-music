#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Download the WinRT metadata (.winmd) that the shim generator (tools/gen.py) and tools/q read.
# Official packages from nuget.org, pinned versions with pinned SHA-256, verified before anything
# is extracted, into $AM_METADATA_DIR (default $AM_CACHE_DIR/metadata, see bin/amd-env.sh):
#   sdk/*.winmd       Microsoft.Windows.SDK.Contracts 10.0.26100.7175 (ref/netstandard2.0)
#   winui/Microsoft.UI.Xaml.winmd       Microsoft.WindowsAppSDK.WinUI 2.3.9
#   winui/Microsoft.UI.winmd, winui/Microsoft.Foundation.winmd
#                     Microsoft.WindowsAppSDK.InteractiveExperiences 2.1.9 (metadata/10.0.18362.0)
# The two WinAppSDK packages are the ones Microsoft.WindowsAppSDK 2.5.1 references; their .winmd
# files are byte-identical to the copies inside the app's MSIX (1.1792.39941.0). Only the few
# WinUI classes AMP.Services touches are shimmed, so only these three files are extracted.
#
# License: these Microsoft packages require license acceptance (requireLicenseAcceptance in their
# .nuspec), as when they are added to a project with NuGet. Running this script downloads them for
# you, so read and accept the terms first:
#   Microsoft.Windows.SDK.Contracts   https://aka.ms/WinSDKLicenseURL
#   Microsoft.WindowsAppSDK.*         license.txt inside each package, kept as
#                                     $AM_METADATA_DIR/licenses/<package>.txt
# Nothing from them is part of this repository; the metadata stays in your cache.
#
# usage: tools/fetch-metadata.sh      (idempotent: returns at once when everything is in place)
# Environment: AM_METADATA_DIR, AM_CACHE_DIR (see bin/amd-env.sh).
set -e
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
. "$ROOT/bin/amd-env.sh"
META="$AM_METADATA_DIR"

# id version sha256-of-the-.nupkg
PACKAGES="microsoft.windows.sdk.contracts 10.0.26100.7175 edcfd36ad8e378d206ab00245e74bcf9fcb49d2a14a3e1f3d5e2c2d1adbcd593
microsoft.windowsappsdk.winui 2.3.9 d36145bd6c0f46fc04e07fb4af365d093fb42c39b625948d0ead07a0b26fce96
microsoft.windowsappsdk.interactiveexperiences 2.1.9 f9159d73e3e46717f159606e43b4acf8336aaff5bdd4d395fe3a93fdb41437ac"
STAMP="$META/.complete"
WANT="$(printf '%s\n' "$PACKAGES" | sha256sum | cut -d' ' -f1)"

metadata_ok() {
    [ "$(cat "$STAMP" 2>/dev/null)" = "$WANT" ] &&
        ls "$META"/sdk/*.winmd >/dev/null 2>&1 &&
        [ -f "$META/winui/Microsoft.UI.Xaml.winmd" ] && [ -f "$META/winui/Microsoft.UI.winmd" ] &&
        [ -f "$META/winui/Microsoft.Foundation.winmd" ] && [ -d "$META/licenses" ]
}
metadata_ok && exit 0

for t in curl unzip sha256sum; do
    command -v "$t" >/dev/null 2>&1 || { echo "error: $t is required" >&2; exit 1; }
done

cat >&2 <<'EOF'
Downloading Windows SDK and Windows App SDK metadata from nuget.org.
These Microsoft packages require license acceptance; running this script means you accept their terms:
  Microsoft.Windows.SDK.Contracts   https://aka.ms/WinSDKLicenseURL
  Microsoft.WindowsAppSDK.WinUI, Microsoft.WindowsAppSDK.InteractiveExperiences
                                    license.txt in each package (kept in the licenses/ folder of the metadata)
EOF

mkdir -p "$META/nupkg"
printf '%s\n' "$PACKAGES" | while read -r id ver sum; do
    f="$META/nupkg/$id.$ver.nupkg"
    if [ -f "$f" ] && [ "$(sha256sum "$f" | cut -d' ' -f1)" = "$sum" ]; then continue; fi
    echo "  $id $ver" >&2
    curl -fL --proto '=https' --tlsv1.2 --retry 3 -sS -o "$f.part" \
        "https://api.nuget.org/v3-flatcontainer/$id/$ver/$id.$ver.nupkg"
    got="$(sha256sum "$f.part" | cut -d' ' -f1)"
    if [ "$got" != "$sum" ]; then
        rm -f "$f.part"
        echo "error: SHA-256 mismatch for $id $ver: got $got, expected $sum" >&2
        exit 1
    fi
    mv -f "$f.part" "$f"
done

# extract into a fresh directory, then swap it in
TMP="$META/.extract.$$"
rm -rf "$TMP"
mkdir -p "$TMP/sdk" "$TMP/winui"
trap 'rm -rf "$TMP"' EXIT
N="$META/nupkg"
unzip -q -j -o "$N/microsoft.windows.sdk.contracts.10.0.26100.7175.nupkg" 'ref/netstandard2.0/*.winmd' -d "$TMP/sdk"
unzip -q -j -o "$N/microsoft.windowsappsdk.winui.2.3.9.nupkg" 'metadata/Microsoft.UI.Xaml.winmd' -d "$TMP/winui"
unzip -q -j -o "$N/microsoft.windowsappsdk.interactiveexperiences.2.1.9.nupkg" \
    'metadata/10.0.18362.0/Microsoft.UI.winmd' 'metadata/10.0.18362.0/Microsoft.Foundation.winmd' -d "$TMP/winui"
# the WinAppSDK license texts, kept next to the metadata (SDK Contracts: licenseUrl only)
mkdir -p "$TMP/licenses"
unzip -p "$N/microsoft.windowsappsdk.winui.2.3.9.nupkg" license.txt > "$TMP/licenses/Microsoft.WindowsAppSDK.WinUI.txt"
unzip -p "$N/microsoft.windowsappsdk.interactiveexperiences.2.1.9.nupkg" license.txt \
    > "$TMP/licenses/Microsoft.WindowsAppSDK.InteractiveExperiences.txt"
rm -rf "$META/sdk" "$META/winui" "$META/licenses"
mv "$TMP/sdk" "$TMP/winui" "$TMP/licenses" "$META/"
printf '%s\n' "$WANT" > "$STAMP"
# the packages are only needed again when the pins change
rm -rf "$N"
echo "metadata ready in $META (license texts: $META/licenses)" >&2
