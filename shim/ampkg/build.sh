#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# ampkg.dll (build only; setup-prefix.sh and shim/install.sh install it next to the agent)
set -e
cd "$(dirname "$(readlink -f "$0")")"
winegcc -b x86_64-windows -shared -O1 -g -Wall -D__WINESRC__ -idirafter /usr/include \
    *.c ampkg.spec -lntdll -ladvapi32 -lcrypt32 -o ampkg.dll
