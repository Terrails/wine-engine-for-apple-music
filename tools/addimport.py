#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Prepend an import of <dll>!<func> to a PE32+ image's import table.

usage: addimport.py <in.exe> <out.exe> <dll> <func>
Adds a new section with a rebuilt import directory; original descriptors are kept.
"""
import sys, struct, pefile


def align(x, a):
    return (x + a - 1) // a * a


def main():
    src, dst, dll, func = sys.argv[1:5]
    pe = pefile.PE(src)
    oh = pe.OPTIONAL_HEADER
    fa, sa = oh.FileAlignment, oh.SectionAlignment

    imp_dir = oh.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']]
    old = pe.get_data(imp_dir.VirtualAddress, imp_dir.Size)
    descs = []
    for i in range(0, len(old) - 19, 20):
        d = old[i:i + 20]
        if d == b'\0' * 20:
            break
        descs.append(d)

    last = pe.sections[-1]
    new_rva = align(last.VirtualAddress + max(last.Misc_VirtualSize, last.SizeOfRawData), sa)
    new_raw = align(last.PointerToRawData + last.SizeOfRawData, fa)

    # layout: [descs (n+2)*20][ILT 16][IAT 16][hint/name][dllname]
    n = len(descs) + 1
    off_desc = 0
    off_ilt = align((n + 1) * 20, 8)
    off_iat = off_ilt + 16
    off_hn = off_iat + 16
    hn = struct.pack('<H', 0) + func.encode() + b'\0'
    if len(hn) % 2: hn += b'\0'
    off_name = off_hn + len(hn)
    blob_len = off_name + len(dll) + 1

    data = bytearray(align(blob_len, fa))
    newdesc = struct.pack('<IIIII', new_rva + off_ilt, 0, 0, new_rva + off_name, new_rva + off_iat)
    alld = [newdesc] + descs
    for i, d in enumerate(alld):
        data[i * 20:(i + 1) * 20] = d
    struct.pack_into('<Q', data, off_ilt, new_rva + off_hn)
    struct.pack_into('<Q', data, off_iat, new_rva + off_hn)
    data[off_hn:off_hn + len(hn)] = hn
    data[off_name:off_name + len(dll)] = dll.encode()

    # add section header
    sh_off = pe.sections[-1].get_file_offset() + 40
    if sh_off + 40 > oh.SizeOfHeaders or sh_off + 40 > pe.sections[0].PointerToRawData:
        sys.exit("no room for another section header")
    hdr = struct.pack('<8sIIIIIIHHI', b'.ampkg\0\0', blob_len, new_rva, len(data), new_raw, 0, 0, 0, 0,
                      0xC0000040)  # initialized data | read | write
    pe.set_bytes_at_offset(sh_off, hdr)
    pe.FILE_HEADER.NumberOfSections += 1
    oh.SizeOfImage = align(new_rva + blob_len, sa)
    imp_dir.VirtualAddress = new_rva
    imp_dir.Size = (n + 1) * 20
    bound = oh.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT']]
    bound.VirtualAddress = 0
    bound.Size = 0
    oh.CheckSum = 0
    # drop the Authenticode signature: it no longer matches and sits at the end of the file
    sec = oh.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_SECURITY']]
    sig_off, sig_len = sec.VirtualAddress, sec.Size
    sec.VirtualAddress = 0
    sec.Size = 0

    out = bytearray(pe.write())
    if sig_len and sig_off + sig_len == len(out) and sig_off >= new_raw:
        out = out[:sig_off]
    out = out[:new_raw] + b'\0' * max(0, new_raw - len(out))
    out += data
    open(dst, 'wb').write(out)
    print(f"{dst}: added {dll}!{func} ({len(descs)} existing imports)")


if __name__ == '__main__':
    main()
