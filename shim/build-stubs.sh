#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Build the small DLLs Wine lacks (build only; shim/install.sh installs them):
#   webauthn/webauthn.dll, bcp47mrm/bcp47mrm.dll   stubs: passkeys, BCP-47 tag check
#   certreq/amcertreq.dll                          crypt32 encoder for certificate requests (push)
set -e
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
for d in webauthn bcp47mrm; do
    (cd "$HERE/$d" && winegcc -b x86_64-windows -shared -O1 $d.c $d.spec -o $d.dll)
done
(cd "$HERE/certreq" && winegcc -b x86_64-windows -shared -O1 -Wall certreq.c certreq.spec -lcrypt32 -o amcertreq.dll)
