# SPDX-License-Identifier: LGPL-2.1-or-later
"""Structured WinRT metadata model on top of dnfile (x64 layout)."""
import os, glob, struct, hashlib, uuid, functools
import dnfile

PRIM = {0x1: 'void', 0x2: 'Boolean', 0x3: 'Char16', 0x4: 'Int8', 0x5: 'UInt8', 0x6: 'Int16',
        0x7: 'UInt16', 0x8: 'Int32', 0x9: 'UInt32', 0xa: 'Int64', 0xb: 'UInt64', 0xc: 'Single',
        0xd: 'Double', 0xe: 'String', 0x1c: 'Object'}
PRIM_SIZE = {'Boolean': 1, 'Char16': 2, 'Int8': 1, 'UInt8': 1, 'Int16': 2, 'UInt16': 2, 'Int32': 4,
             'UInt32': 4, 'Int64': 8, 'UInt64': 8, 'Single': 4, 'Double': 8, 'String': 8, 'Object': 8,
             'Guid': 16}
PRIM_SIG = {'Boolean': 'b1', 'Char16': 'c2', 'Int8': 'i1', 'UInt8': 'u1', 'Int16': 'i2', 'UInt16': 'u2',
            'Int32': 'i4', 'UInt32': 'u4', 'Int64': 'i8', 'UInt64': 'u8', 'Single': 'f4', 'Double': 'f8',
            'String': 'string', 'Object': 'cinterface(IInspectable)', 'Guid': 'g16'}
PINTERFACE_NS = uuid.UUID('11f47ad5-7b73-42c0-abae-878b1e16adee').bytes


class T:
    """A type reference. kind: prim | named | generic | var | array | byref"""
    def __init__(self, kind, name=None, args=None, inner=None, index=None):
        self.kind, self.name, self.args, self.inner, self.index = kind, name, args or [], inner, index

    def subst(self, gargs):
        if self.kind == 'var':
            return gargs[self.index]
        if self.kind == 'generic':
            return T('generic', self.name, [a.subst(gargs) for a in self.args])
        if self.kind in ('array', 'byref'):
            return T(self.kind, inner=self.inner.subst(gargs))
        return self

    def __repr__(self):
        if self.kind == 'prim': return self.name
        if self.kind == 'named': return self.name
        if self.kind == 'generic': return f"{self.name}<{', '.join(map(repr, self.args))}>"
        if self.kind == 'var': return f"!{self.index}"
        if self.kind == 'array': return f"{self.inner!r}[]"
        if self.kind == 'byref': return f"{self.inner!r}&"


class Param:
    def __init__(self, name, type, out):
        self.name, self.type, self.out = name, type, out


class Method:
    def __init__(self, name, params, ret):
        self.name, self.params, self.ret = name, params, ret


class TypeDef:
    def __init__(self, meta, idx):
        self.meta, self.idx = meta, idx
        td = meta.md.TypeDef[idx - 1]
        self.td = td
        self.ns, self.short = str(td.TypeNamespace), str(td.TypeName)
        self.name = f"{self.ns}.{self.short}"
        self.is_interface = bool(td.Flags.tdInterface)
        ext = td.Extends.row
        base = f"{ext.TypeNamespace}.{ext.TypeName}" if ext is not None and hasattr(ext, 'TypeName') else ''
        if self.is_interface: self.category = 'interface'
        elif base == 'System.Enum': self.category = 'enum'
        elif base == 'System.ValueType': self.category = 'struct'
        elif base == 'System.MulticastDelegate': self.category = 'delegate'
        else: self.category = 'class'
        self.generic_count = int(self.short.split('`')[1]) if '`' in self.short else 0

    @functools.cached_property
    def attrs(self):
        return self.meta.attrs_for(self.idx)

    @functools.cached_property
    def guid(self):
        for n, v in self.attrs:
            if n == 'Windows.Foundation.Metadata.GuidAttribute':
                a, b, c = struct.unpack_from('<IHH', v, 2)
                return uuid.UUID(fields=(a, b, c, v[10], v[11], int.from_bytes(v[12:18], 'big')))
        return None

    @functools.cached_property
    def interfaces(self):
        """[(T, is_default)]"""
        out = []
        for ii in self.meta.md.InterfaceImpl or []:
            if ii.Class.row_index == self.idx:
                default = False
                for ca in self.meta.md.CustomAttribute or []:
                    p = ca.Parent
                    if p.table is not None and p.table.name == 'InterfaceImpl' and p.row_index == ii._rid if hasattr(ii, '_rid') else False:
                        pass
                out.append(self.meta.typeref_from_index(ii.Interface))
        return out

    @functools.cached_property
    def default_interface(self):
        ifs = self.meta.interface_impls(self.idx)
        for t, d in ifs:
            if d: return t
        return ifs[0][0] if ifs else None

    @functools.cached_property
    def methods(self):
        out = []
        for m in self.td.MethodList:
            mr = m.row
            sig = self.meta.blob(mr.Signature)
            p = [1]
            n = cuint(sig, p)
            ret = self.meta.decode(sig, p)
            ptypes = [self.meta.decode(sig, p) for _ in range(n)]
            prows = {pr.row.Sequence: pr.row for pr in mr.ParamList}
            params = []
            for i, t in enumerate(ptypes):
                r = prows.get(i + 1)
                params.append(Param(str(r.Name) if r else f"p{i}", t, bool(r and r.Flags.pdOut)))
            out.append(Method(str(mr.Name), params, ret))
        return out

    @functools.cached_property
    def fields(self):
        out = []
        for f in self.td.FieldList:
            fr = f.row
            sig = self.meta.blob(fr.Signature)
            t = self.meta.decode(sig, [1])
            out.append((str(fr.Name), t, fr))
        return out

    def enum_underlying(self):
        for n, t, _ in self.fields:
            if n == 'value__': return t.name
        return 'Int32'

    def typed_attrs(self, attr_name):
        """Types named in attributes like StaticAttribute(typeof(X), ...)."""
        out = []
        for n, v in self.attrs:
            if n.endswith('.' + attr_name) and len(v) > 3:
                p = [2]
                try:
                    ln = cuint(v, p)
                    s = v[p[0]:p[0] + ln].decode('utf-8', 'replace')
                    if '.' in s and not s[0].isdigit():
                        out.append(s.split(',')[0])
                        continue
                except Exception:
                    pass
                out.append(None)  # attribute without type -> default activation
        return out


def cuint(b, p):
    x = b[p[0]]
    if x & 0x80 == 0:
        p[0] += 1; return x
    if x & 0xc0 == 0x80:
        v = ((x & 0x3f) << 8) | b[p[0] + 1]; p[0] += 2; return v
    v = ((x & 0x1f) << 24) | (b[p[0] + 1] << 16) | (b[p[0] + 2] << 8) | b[p[0] + 3]; p[0] += 4; return v


class Meta:
    def __init__(self, path):
        self.path = path
        self.pe = dnfile.dnPE(path)
        self.md = self.pe.net.mdtables
        self.types = {}
        for i, td in enumerate(self.md.TypeDef or [], 1):
            self.types[f"{td.TypeNamespace}.{td.TypeName}"] = i
        self._ii = None

    @staticmethod
    def blob(b):
        if hasattr(b, 'value'): b = b.value
        return bytes(b) if b is not None else b''

    def coded(self, v):
        tag, idx = v & 3, v >> 2
        if tag == 0:
            td = self.md.TypeDef[idx - 1]; return self.named(f"{td.TypeNamespace}.{td.TypeName}")
        if tag == 1:
            tr = self.md.TypeRef[idx - 1]; return self.named(f"{tr.TypeNamespace}.{tr.TypeName}")
        ts = self.md.TypeSpec[idx - 1]
        return self.decode(self.blob(ts.Signature), [0])

    @staticmethod
    def named(n):
        if n == 'System.Guid': return T('prim', 'Guid')
        return T('named', n)

    def typeref_from_index(self, ix):
        row = ix.row
        if hasattr(row, 'TypeName'):
            return self.named(f"{row.TypeNamespace}.{row.TypeName}")
        return self.decode(self.blob(row.Signature), [0])

    def decode(self, b, p):
        t = b[p[0]]; p[0] += 1
        if t in PRIM: return T('prim', PRIM[t])
        if t in (0x11, 0x12): return self.coded(cuint(b, p))
        if t == 0x10: return T('byref', inner=self.decode(b, p))
        if t == 0x0f: return T('byref', inner=self.decode(b, p))
        if t == 0x1d: return T('array', inner=self.decode(b, p))
        if t == 0x13: return T('var', index=cuint(b, p))
        if t == 0x15:
            base = self.decode(b, p)
            n = cuint(b, p)
            return T('generic', base.name, [self.decode(b, p) for _ in range(n)])
        if t in (0x1f, 0x20):
            cuint(b, p); return self.decode(b, p)
        raise ValueError(f"sig elem {t:#x}")

    def attrs_for(self, tdidx, table='TypeDef'):
        out = []
        for ca in self.md.CustomAttribute or []:
            par = ca.Parent
            if par.table is None or par.table.name != table or par.row_index != tdidx:
                continue
            ctor = ca.Type.row
            cls = ctor.Class.row if hasattr(ctor, 'Class') else None
            name = f"{cls.TypeNamespace}.{cls.TypeName}" if cls is not None and hasattr(cls, 'TypeName') else '?'
            out.append((name, self.blob(ca.Value)))
        return out

    def interface_impls(self, tdidx):
        res = []
        for rid, ii in enumerate(self.md.InterfaceImpl or [], 1):
            if ii.Class.row_index == tdidx:
                default = any(n.endswith('DefaultAttribute') for n, _ in self.attrs_for(rid, 'InterfaceImpl'))
                res.append((self.typeref_from_index(ii.Interface), default))
        return res


class DB:
    def __init__(self, dirs):
        self.metas = []
        for d in dirs:
            self.metas += [Meta(f) for f in sorted(glob.glob(os.path.join(d, '*.winmd')))]
        self.cache = {}

    def get(self, name):
        if name in self.cache: return self.cache[name]
        for m in self.metas:
            if name in m.types:
                td = TypeDef(m, m.types[name]); self.cache[name] = td; return td
        self.cache[name] = None
        return None

    # ---- type signatures / IIDs ----
    def sig(self, t):
        if t.kind == 'prim': return PRIM_SIG[t.name]
        if t.kind == 'generic':
            g = self.get(t.name)
            return f"pinterface({{{g.guid}}};{';'.join(self.sig(a) for a in t.args)})"
        td = self.get(t.name)
        if td is None: raise KeyError(t.name)
        if td.category == 'interface': return f"{{{td.guid}}}"
        if td.category == 'delegate': return f"delegate({{{td.guid}}})"
        if td.category == 'enum':
            return f"enum({td.name};{'u4' if td.enum_underlying() == 'UInt32' else 'i4'})"
        if td.category == 'struct':
            return f"struct({td.name};{';'.join(self.sig(ft) for n, ft, fr in td.fields if not fr.Flags.fdStatic)})"
        if td.category == 'class':
            return f"rc({td.name};{self.sig(td.default_interface)})"
        raise KeyError(t)

    def iid(self, t):
        if t.kind == 'generic':
            h = hashlib.sha1(PINTERFACE_NS + self.sig(t).encode()).digest()
            b = bytearray(h[:16])
            b[6] = (b[6] & 0x0f) | 0x50
            b[8] = (b[8] & 0x3f) | 0x80
            return uuid.UUID(bytes=bytes(b))
        return self.get(t.name).guid

    def methods_of(self, t):
        """Methods of an interface type with generic args substituted."""
        if t.kind == 'generic':
            g = self.get(t.name)
            return [Method(m.name, [Param(p.name, p.type.subst(t.args), p.out) for p in m.params],
                           m.ret.subst(t.args)) for m in g.methods]
        return self.get(t.name).methods

    def required(self, t):
        """Interfaces required by interface t (transitively), substituted."""
        name = t.name
        td = self.get(name)
        out = []
        for r, _ in td.meta.interface_impls(td.idx):
            r2 = r.subst(t.args) if t.kind == 'generic' else r
            out.append(r2)
            out += self.required(r2)
        return out

    # ---- layout ----
    def size_align(self, t):
        if t.kind == 'prim':
            s = PRIM_SIZE[t.name]; return s, (4 if t.name == 'Guid' else s)
        if t.kind in ('generic', 'array', 'byref'): return 8, 8
        td = self.get(t.name)
        if td is None: return 8, 8
        if td.category == 'enum': return 4, 4
        if td.category == 'struct':
            off, al = 0, 1
            for n, ft, fr in td.fields:
                if fr.Flags.fdStatic: continue
                s, a = self.size_align(ft)
                off = (off + a - 1) // a * a + s
                al = max(al, a)
            return (off + al - 1) // al * al, al
        return 8, 8

    def kind(self, t):
        """'prim' | 'string' | 'object' | 'enum' | 'struct' | 'guid'"""
        if t.kind == 'prim':
            if t.name == 'String': return 'string'
            if t.name == 'Object': return 'object'
            if t.name == 'Guid': return 'struct'
            return 'prim'
        if t.kind == 'generic': return 'object'
        td = self.get(t.name)
        if td is None: return 'object'
        if td.category in ('enum',): return 'prim'
        if td.category == 'struct':
            # IReference-free struct; may contain HSTRINGs (rare)
            return 'struct'
        return 'object'
