#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compact disassembly of one function from an llvm-objdump listing, with imports and strings
resolved.  usage: disfn.py <pe file> <listing.asm> <start va hex> [max lines] [--all]
Without --all only calls, returns, and lines that reference data are shown."""
import sys, re, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
base = pe.OPTIONAL_HEADER.ImageBase
imps = {}
for e in pe.DIRECTORY_ENTRY_IMPORT:
    for im in e.imports:
        if im.name: imps[im.address] = im.name.decode()
def strat(a):
    if a in imps: return '<' + imps[a][:70] + '>'
    try:
        b = pe.get_data(a - base, 240)
        s8 = b.split(b'\0')[0]
        if len(s8) >= 4 and all(32 <= c < 127 for c in s8): return '"' + s8.decode()[:110] + '"'
        s16 = b.decode('utf-16le', errors='ignore').split('\0')[0]
        if len(s16) >= 3 and all(32 <= ord(c) < 127 for c in s16): return 'L"' + s16[:110] + '"'
    except Exception:
        pass
start = int(sys.argv[2 + 1], 16)
maxl = int(sys.argv[4]) if len(sys.argv) > 4 and sys.argv[4].isdigit() else 200
show_all = '--all' in sys.argv
pref = '%x:' % start
n = 0; on = False
for l in open(sys.argv[2]):
    if not on:
        if l.startswith(pref): on = True
        else: continue
    n += 1
    if n > maxl: break
    l = l.rstrip('\n')
    m = re.search(r'# 0x([0-9a-f]+)', l); extra = ''
    if m:
        s = strat(int(m.group(1), 16))
        if s: extra = '   ; ' + s
    ins = l.split('\t')
    op = ins[1] if len(ins) > 1 else ''
    if show_all or extra or op in ('call', 'ret', 'jmp') :
        print(re.sub(r'\s+', ' ', l.strip())[:100] + extra)
    if op == 'ret' and not show_all and n > 3: pass
    if 'int3' in l: break
