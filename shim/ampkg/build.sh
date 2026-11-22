#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# ampkg.dll (build only; setup-prefix.sh and shim/install.sh install it next to the agent)
set -e
cd "$(dirname "$(readlink -f "$0")")"
# Wine's own headers (wine/debug.h) sit next to the tools: /usr/include for a distribution
# package, /opt/wine-*/include for a WineHQ build
WINE_INC="$(dirname "$(readlink -f "$(command -v winegcc)")")/../include"
winegcc -b x86_64-windows -shared -O1 -g -Wall -D__WINESRC__ -idirafter "$WINE_INC" -idirafter /usr/include \
    *.c ampkg.spec -lntdll -ladvapi32 -lcrypt32 -o ampkg.dll
