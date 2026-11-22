#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tiny WinRT metadata query tool.

usage: winmd.py <winmd-dir> <TypeName>...    (full names, e.g. Windows.Foundation.Uri)
       winmd.py <winmd-dir> --search <substring>
"""
import os, sys, struct, glob, pickle
import dnfile

ELEM = {0x1: 'void', 0x2: 'boolean', 0x3: 'WCHAR', 0x4: 'INT8', 0x5: 'UINT8', 0x6: 'INT16',
        0x7: 'UINT16', 0x8: 'INT32', 0x9: 'UINT32', 0xa: 'INT64', 0xb: 'UINT64', 0xc: 'FLOAT',
        0xd: 'DOUBLE', 0xe: 'HSTRING', 0x18: 'INT_PTR', 0x19: 'UINT_PTR', 0x1c: 'IInspectable*'}


class Meta:
    def __init__(self, path):
        self.pe = dnfile.dnPE(path)
        self.md = self.pe.net.mdtables
        self.blob = self.pe.net.blobs if hasattr(self.pe.net, 'blobs') else None
        self.types = {}
        for i, td in enumerate(self.md.TypeDef or [], 1):
            self.types[f"{td.TypeNamespace}.{td.TypeName}"] = i

    def blob_bytes(self, b):
        if hasattr(b, 'value'):
            b = b.value
        return bytes(b) if b is not None else b''

    def typedef_or_ref_name(self, idx_obj):
        row = idx_obj.row
        if row is None:
            return '?'
        if hasattr(row, 'TypeNamespace') and hasattr(row, 'TypeName'):
            return f"{row.TypeNamespace}.{row.TypeName}"
        if hasattr(row, 'Signature'):  # TypeSpec
            return self.decode_type(self.blob_bytes(row.Signature), [0])
        return '?'

    def coded_typedeforref(self, v):
        tag, idx = v & 3, v >> 2
        if tag == 0:
            td = self.md.TypeDef[idx - 1]; return f"{td.TypeNamespace}.{td.TypeName}"
        if tag == 1:
            tr = self.md.TypeRef[idx - 1]; return f"{tr.TypeNamespace}.{tr.TypeName}"
        if tag == 2:
            ts = self.md.TypeSpec[idx - 1]; return self.decode_type(self.blob_bytes(ts.Signature), [0])
        return '?'

    @staticmethod
    def cuint(b, p):
        x = b[p[0]]
        if x & 0x80 == 0:
            p[0] += 1; return x
        if x & 0xc0 == 0x80:
            v = ((x & 0x3f) << 8) | b[p[0] + 1]; p[0] += 2; return v
        v = ((x & 0x1f) << 24) | (b[p[0] + 1] << 16) | (b[p[0] + 2] << 8) | b[p[0] + 3]; p[0] += 4; return v

    def decode_type(self, b, p):
        t = b[p[0]]; p[0] += 1
        if t in ELEM:
            return ELEM[t]
        if t == 0x11 or t == 0x12:  # valuetype / class
            return self.coded_typedeforref(self.cuint(b, p))
        if t == 0x10:
            return self.decode_type(b, p) + '&'
        if t == 0x0f:
            return self.decode_type(b, p) + '*'
        if t == 0x1d:
            return self.decode_type(b, p) + '[]'
        if t == 0x13:
            return f"!{self.cuint(b, p)}"
        if t == 0x15:
            base = self.decode_type(b, p)
            n = self.cuint(b, p)
            args = [self.decode_type(b, p) for _ in range(n)]
            return f"{base}<{', '.join(args)}>"
        if t == 0x1f or t == 0x20:  # cmod
            self.cuint(b, p); return self.decode_type(b, p)
        return f"?{t:#x}"

    def method_sig(self, b):
        p = [1]
        n = self.cuint(b, p)
        ret = self.decode_type(b, p)
        params = [self.decode_type(b, p) for _ in range(n)]
        return ret, params

    def attrs_for(self, tdidx):
        out = []
        for ca in self.md.CustomAttribute or []:
            par = ca.Parent
            if par.table is None or par.table.name != 'TypeDef' or par.row_index != tdidx:
                continue
            ctor = ca.Type.row
            cls = ctor.Class.row if hasattr(ctor, 'Class') else None
            name = f"{cls.TypeNamespace}.{cls.TypeName}" if cls is not None and hasattr(cls, 'TypeName') else '?'
            out.append((name, self.blob_bytes(ca.Value)))
        return out

    def describe(self, name):
        idx = self.types.get(name)
        if idx is None:
            return None
        td = self.md.TypeDef[idx - 1]
        lines = []
        flags = td.Flags
        is_iface = bool(flags.tdInterface)
        guid = None
        attrs = []
        for an, val in self.attrs_for(idx):
            if an.endswith('GuidAttribute') and len(val) >= 18:
                a, b_, c = struct.unpack_from('<IHH', val, 2)
                d = val[10:18]
                guid = f"{a:08x}-{b_:04x}-{c:04x}-{d[0]:02x}{d[1]:02x}-{d[2:].hex()}"
            else:
                short = an.split('.')[-1]
                txt = ''
                if short in ('StaticAttribute', 'ActivatableAttribute', 'ComposableAttribute', 'ExclusiveToAttribute'):
                    # first arg is often a System.Type (ser string)
                    if len(val) > 3 and val[2] not in (0xff,):
                        p = [2]
                        try:
                            ln = self.cuint(val, p)
                            s = val[p[0]:p[0] + ln].decode('utf-8', 'replace')
                            if s.startswith('Windows.') or s.startswith('Microsoft.'):
                                txt = s.split(',')[0]
                        except Exception:
                            pass
                attrs.append(f"{short}({txt})" if txt else short)
        kind = 'interface' if is_iface else ('enum/struct' if td.Extends and getattr(td.Extends.row, 'TypeName', '') in ('Enum', 'ValueType') else 'class')
        lines.append(f"{kind} {name}" + (f"  {{{guid}}}" if guid else ''))
        if attrs:
            lines.append("  attrs: " + ', '.join(sorted(set(attrs))))
        for ii in self.md.InterfaceImpl or []:
            if ii.Class.row_index == idx:
                default = False
                lines.append(f"  implements {self.typedef_or_ref_name(ii.Interface)}")
        if kind == 'enum/struct':
            for f in td.FieldList:
                fr = f.row
                if fr.Name != 'value__':
                    sig = self.blob_bytes(fr.Signature)
                    lines.append(f"  field {self.decode_type(sig, [1])} {fr.Name}")
        slot = 6 if is_iface else 0
        for m in td.MethodList:
            mr = m.row
            ret, params = self.method_sig(self.blob_bytes(mr.Signature))
            pnames = [pr.row.Name for pr in mr.ParamList if pr.row.Sequence > 0]
            pflags = [pr.row.Flags for pr in mr.ParamList if pr.row.Sequence > 0]
            ps = []
            for i, t in enumerate(params):
                nm = pnames[i] if i < len(pnames) else f"p{i}"
                out = i < len(pflags) and pflags[i].pdOut
                ps.append(f"{'[out] ' if out else ''}{t} {nm}")
            if ret != 'void':
                ps.append(f"[retval] {ret}* result")
            lines.append(f"  [{slot}] {mr.Name}({', '.join(ps)})")
            slot += 1
        return '\n'.join(lines)


def load_all(d):
    return [Meta(f) for f in sorted(glob.glob(os.path.join(d, '*.winmd')))]


if __name__ == '__main__':
    metas = load_all(sys.argv[1])
    args = sys.argv[2:]
    if args and args[0] == '--search':
        s = args[1].lower()
        for m in metas:
            for n in m.types:
                if s in n.lower():
                    print(n)
        sys.exit()
    for name in args:
        for m in metas:
            r = m.describe(name)
            if r:
                print(r); print(); break
        else:
            print(f"{name}: not found\n")
