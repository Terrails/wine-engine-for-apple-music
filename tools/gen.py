#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate C stub implementations of WinRT runtime classes from metadata.

usage: gen.py <shim-dir>
  reads  <shim-dir>/classes.txt   (one runtime class per line, '#' comments)
         <shim-dir>/*.c           (scanned for OV_* override definitions)
         $AM_METADATA_DIR/{sdk,winui}/*.winmd   (tools/fetch-metadata.sh)
  writes <shim-dir>/gen/amshim_gen.c, gen/amshim_gen.h
"""
import os, re, sys, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wmd import DB, T

# Metadata from tools/fetch-metadata.sh: AM_METADATA_DIR, default $AM_CACHE_DIR/metadata
# (AM_CACHE_DIR default ${XDG_CACHE_HOME:-~/.cache}/wine-engine-for-apple-music).
META = os.environ.get('AM_METADATA_DIR') or os.path.join(
    os.environ.get('AM_CACHE_DIR') or os.path.join(os.environ.get('XDG_CACHE_HOME') or os.path.expanduser('~/.cache'),
                                                   'wine-engine-for-apple-music'), 'metadata')
META_DIRS = [os.path.join(META, 'sdk'),        # Windows SDK contracts
             # WinUI/WinAppSDK metadata (only the few classes AMP.Services touches are shimmed)
             os.path.join(META, 'winui')]

CPRIM = {'Boolean': 'boolean', 'Char16': 'WCHAR', 'Int8': 'INT8', 'UInt8': 'UINT8', 'Int16': 'INT16',
         'UInt16': 'UINT16', 'Int32': 'INT32', 'UInt32': 'UINT32', 'Int64': 'INT64', 'UInt64': 'UINT64',
         'Single': 'FLOAT', 'Double': 'DOUBLE', 'String': 'HSTRING', 'Object': 'IInspectable *', 'Guid': 'GUID'}


def mangle(t):
    if t.kind == 'prim': return t.name
    if t.kind == 'generic':
        return t.name.split('.')[-1].split('`')[0] + '_' + '_'.join(mangle(a) for a in t.args) + '_'
    if t.kind == 'named': return t.name.split('.')[-1]
    if t.kind == 'array': return mangle(t.inner) + 'Arr'
    if t.kind == 'byref': return mangle(t.inner) + 'Ref'
    return 'X'


def fullmangle(t):
    if t.kind == 'generic':
        return t.name.replace('.', '_').split('`')[0] + '__' + '__'.join(fullmangle(a) for a in t.args) + '__'
    if t.kind == 'named': return t.name.replace('.', '_')
    return mangle(t)


class Gen:
    def __init__(self, db):
        self.db = db
        self.structs = {}      # name -> C typedef text (ordered by insertion)
        self.ifaces = {}       # iid -> (T, cname, methods)
        self.out = []

    # ---------- C types ----------
    def ctype(self, t):
        if t.kind == 'prim': return CPRIM[t.name]
        if t.kind == 'generic': return 'void *'
        td = self.db.get(t.name)
        if td is None: return 'void *'
        if td.category == 'enum': return 'UINT32' if td.enum_underlying() == 'UInt32' else 'INT32'
        if td.category == 'struct':
            self.ensure_struct(td)
            return 'S_' + fullmangle(t)
        return 'void *'

    def ensure_struct(self, td):
        name = 'S_' + td.name.replace('.', '_')
        if name in self.structs: return
        self.structs[name] = None  # placeholder for recursion ordering
        fields = []
        for fn, ft, fr in td.fields:
            if fr.Flags.fdStatic: continue
            fields.append(f"    {self.ctype(ft)} {fn};")
        del self.structs[name]
        self.structs[name] = f"typedef struct {{\n" + '\n'.join(fields) + f"\n}} {name};\n"

    def params_c(self, m):
        """Return list of (ctype, name) as passed in ABI, plus retval info."""
        out = []
        for p in m.params:
            if p.name in ('result', 'result_len', 'o', 'hr', 'This') or p.name.endswith('_len'):
                p.name = p.name + '_'
        for p in m.params:
            t = p.type
            if t.kind == 'array':
                inner = self.ctype(t.inner)
                if p.out and not t.kind == 'byref':
                    out.append(('UINT32', f"{p.name}_len")); out.append((f"{inner} *", p.name))
                else:
                    out.append(('UINT32', f"{p.name}_len")); out.append((f"{inner} *", p.name))
            elif t.kind == 'byref':
                inner = t.inner
                if inner.kind == 'array':
                    out.append(('UINT32 *', f"{p.name}_len")); out.append((f"{self.ctype(inner.inner)} **", p.name))
                else:
                    out.append((f"{self.ctype(inner)} *", p.name))
            else:
                ct = self.ctype(t)
                out.append((ct, p.name))
        ret = None
        if not (m.ret.kind == 'prim' and m.ret.name == 'void'):
            if m.ret.kind == 'array':
                out.append(('UINT32 *', 'result_len')); out.append((f"{self.ctype(m.ret.inner)} **", 'result'))
                ret = ('array', m.ret)
            else:
                out.append((f"{self.ctype(m.ret)} *", 'result'))
                ret = ('value', m.ret)
        return out, ret

    # ---------- interfaces ----------
    def iface(self, t):
        iid = self.db.iid(t)
        if iid in self.ifaces: return self.ifaces[iid]
        methods = self.db.methods_of(t)
        cname = fullmangle(t)
        names, seen = [], {}
        for m in methods:
            n = m.name
            seen[n] = seen.get(n, 0) + 1
            names.append(n if seen[n] == 1 else f"{n}_{seen[n]}")
        info = {'t': t, 'iid': iid, 'cname': cname, 'short': mangle(t), 'methods': list(zip(names, methods))}
        self.ifaces[iid] = info
        return info

    def class_ifaces(self, td):
        res, seen = [], set()
        def add(t):
            try:
                info = self.iface(t)
            except Exception as e:
                self.out.append(f"/* skipped interface {t!r}: {e} */")
                return
            if info['iid'] in seen: return
            seen.add(info['iid']); res.append(info)
            try:
                for r in self.db.required(t): add(r)
            except Exception as e:
                self.out.append(f"/* skipped requires of {t!r}: {e} */")
        dflt = td.default_interface
        if dflt is not None: add(dflt)
        for t, _ in td.meta.interface_impls(td.idx): add(t)
        return res

    def factory_ifaces(self, td):
        res = []
        for kind in ('ActivatableAttribute', 'StaticAttribute', 'ComposableAttribute'):
            for n in td.typed_attrs(kind):
                if n is None: continue
                ft = T('named', n)
                if self.db.get(n) is None: continue
                info = self.iface(ft)
                if all(info['iid'] != r['iid'] for r in res): res.append(info)
        return res


def guid_c(u):
    b = u.bytes
    return ("{0x%s, 0x%s, 0x%s, {%s}}" % (b[0:4].hex(), b[4:6].hex(), b[6:8].hex(),
            ', '.join('0x%02x' % x for x in b[8:])))


def main():
    shim = sys.argv[1]
    classes = [l.split('#')[0].strip() for l in open(os.path.join(shim, 'classes.txt'))]
    classes = [c for c in classes if c]
    overrides = set()
    for f in glob.glob(os.path.join(shim, '*.c')):
        overrides |= set(re.findall(r'\b((?:OV|INIT|DISPATCH)_\w+)\s*\(', open(f).read()))

    for d in META_DIRS:
        if not glob.glob(os.path.join(d, '*.winmd')):
            sys.exit(f"error: no .winmd files in {d}: run tools/fetch-metadata.sh (or set AM_METADATA_DIR)")
    db = DB(META_DIRS)
    g = Gen(db)
    class_infos = []
    for cn in classes:
        td = db.get(cn)
        if td is None:
            print(f"warning: class {cn} not found", file=sys.stderr); continue
        insts = g.class_ifaces(td)
        facts = g.factory_ifaces(td)
        has_default_ctor = None in td.typed_attrs('ActivatableAttribute')
        class_infos.append((cn, td, insts, facts, has_default_ctor))

    # synthetic helper classes implementing generic collection interfaces
    S = T('prim', 'String')
    RM = T('named', 'Windows.ApplicationModel.Resources.Core.ResourceMap')
    KVRM = T('generic', 'Windows.Foundation.Collections.IKeyValuePair`2', [S, RM])
    JV = T('named', 'Windows.Data.Json.IJsonValue')
    KVJ = T('generic', 'Windows.Foundation.Collections.IKeyValuePair`2', [S, JV])
    UE = T('named', 'Windows.Foundation.IWwwFormUrlDecoderEntry')
    SYNTH = {
        'amshim.UrlEntryIterator': [T('generic', 'Windows.Foundation.Collections.IIterator`1', [UE])],
        'amshim.JsonIterator': [T('generic', 'Windows.Foundation.Collections.IIterator`1', [JV])],
        'amshim.JsonKVP': [KVJ],
        'amshim.JsonKVPIterator': [T('generic', 'Windows.Foundation.Collections.IIterator`1', [KVJ])],
        'amshim.StringVectorView': [T('generic', 'Windows.Foundation.Collections.IVectorView`1', [S]),
                                    T('generic', 'Windows.Foundation.Collections.IIterable`1', [S])],
        'amshim.StringIterator': [T('generic', 'Windows.Foundation.Collections.IIterator`1', [S])],
        'amshim.U32Async': [T('generic', 'Windows.Foundation.IAsyncOperation`1', [T('prim', 'UInt32')])],
        'amshim.XmlNodeIterator': [T('generic', 'Windows.Foundation.Collections.IIterator`1',
                                     [T('named', 'Windows.Data.Xml.Dom.IXmlNode')])],
        'amshim.ByteBuffer': [T('named', 'Windows.Storage.Streams.IBuffer')],
        'amshim.BufferInputStream': [T('named', 'Windows.Storage.Streams.IInputStream'),
                                     T('named', 'Windows.Foundation.IClosable')],
        'amshim.StringKVP': [T('generic', 'Windows.Foundation.Collections.IKeyValuePair`2', [S, S])],
        'amshim.StringKVPIterator': [T('generic', 'Windows.Foundation.Collections.IIterator`1',
                                       [T('generic', 'Windows.Foundation.Collections.IKeyValuePair`2', [S, S])])],
        'amshim.StringMapView': [T('generic', 'Windows.Foundation.Collections.IMapView`2', [S, S]),
                                 T('generic', 'Windows.Foundation.Collections.IIterable`1',
                                   [T('generic', 'Windows.Foundation.Collections.IKeyValuePair`2', [S, S])])],
        'amshim.ResourceMapMapView': [T('generic', 'Windows.Foundation.Collections.IMapView`2', [S, RM]),
                                      T('generic', 'Windows.Foundation.Collections.IIterable`1', [KVRM])],
        'amshim.ResourceMapKVPIterator': [T('generic', 'Windows.Foundation.Collections.IIterator`1', [KVRM])],
        'amshim.ResourceMapKVP': [KVRM],
        'amshim.ContainerMapView': [T('generic', 'Windows.Foundation.Collections.IMapView`2',
                                      [S, T('named', 'Windows.Storage.ApplicationDataContainer')])],
    }
    synth_infos = []
    for sname, ts in SYNTH.items():
        synth_infos.append((sname, [g.iface(x) for x in ts]))

    # pre-pass: async return types need IIDs + vtables for async_create()
    ASYNC = {'Windows.Foundation.IAsyncOperation`1': 'ASYNC_OP',
             'Windows.Foundation.IAsyncOperationWithProgress`2': 'ASYNC_OP_PROGRESS',
             'Windows.Foundation.IAsyncAction': 'ASYNC_ACTION',
             'Windows.Foundation.IAsyncActionWithProgress`1': 'ASYNC_ACTION_PROGRESS'}
    async_types = {}
    g.iface(T('named', 'Windows.Foundation.IAsyncInfo'))
    changed = True
    while changed:
        changed = False
        for iid, info in list(g.ifaces.items()):
            for mname, m in info['methods']:
                r = m.ret
                if r.kind in ('generic', 'named') and r.name in ASYNC:
                    try:
                        inf = g.iface(r)
                    except Exception as e:
                        continue
                    if inf['iid'] not in async_types:
                        async_types[inf['iid']] = inf
                        changed = True

    L = []
    w = L.append
    w('/* Generated by tools/gen.py - do not edit */')
    w('#include "amshim_rt.h"')
    w('#include "amshim_gen.h"')
    w('WINE_DEFAULT_DEBUG_CHANNEL(amshim);')
    w('WINE_DECLARE_DEBUG_CHANNEL(amcall);')
    w('')

    # IIDs
    for iid, info in g.ifaces.items():
        w(f"const GUID IID_{info['cname']} = {guid_c(iid)};")
    w('')

    hdr = ['/* Generated by tools/gen.py - do not edit */', '#pragma once', '#include "amshim_rt.h"', '']
    # need structs before prototypes: compute all prototypes first
    protos = []
    bodies = []
    vtbls = []
    for iid, info in list(g.ifaces.items()):
        cn = info['cname']
        hdr.append(f"extern const GUID IID_{cn};")
        entries = []
        for mname, m in info['methods']:
            plist, ret = g.params_c(m)
            fn = f"{cn}_{mname}"
            args = ', '.join(f"{ct} {pn}" for ct, pn in plist)
            ov_names = [f"OV_{info['short']}_{mname}"]
            proto_args = f"struct obj *o{', ' if plist else ''}{args}"
            for ov in ov_names:
                if ov in overrides:
                    protos.append(f"HRESULT {ov}({proto_args});")
            callargs = ', '.join(pn for ct, pn in plist)
            b = [f"static HRESULT WINAPI {fn}(struct ifc *This{', ' if plist else ''}{args})", "{",
                 "    struct obj *o = This->obj;"]
            # class-specific override dispatch
            b.append(f"    HRESULT hr;")
            b.append(f"    if (o->cls->dispatch && o->cls->dispatch(o, &IID_{cn}, {info['methods'].index((mname, m))}, &hr, (void *[]){{{', '.join(['(void *)&' + pn for ct, pn in plist]) or 'NULL'}}})) return hr;")
            ov = ov_names[0]
            if ov in overrides:
                # overridden methods are traced on their own channel (WINEDEBUG=+amcall): name and result
                b.append(f"    hr = {ov}(o{', ' if plist else ''}{callargs});")
                b.append(f'    TRACE_(amcall)("%s {info["short"]}::{mname} -> %#lx\\n", debugstr_w(o->cls->name), hr);')
                b.append("    return hr;")
            else:
                b += default_body(g, db, info, mname, m, plist, ret)
            b.append("}")
            bodies.append('\n'.join(b))
            entries.append(fn)
        vt = [f"static const void *vtbl_{cn}[] =", "{",
              "    ifc_QueryInterface, ifc_AddRef, ifc_Release, ifc_GetIids, ifc_GetRuntimeClassName, ifc_GetTrustLevel,"]
        vt += [f"    {e}," for e in entries]
        vt.append("};")
        vtbls.append('\n'.join(vt))

    for iid, info in async_types.items():
        hdr.append(f"extern const struct iface_entry ASYNC_{info['short']};")
        vtbls.append(f"const struct iface_entry ASYNC_{info['short']} = {{ &IID_{info['cname']}, vtbl_{info['cname']}, \"{info['short']}\" }};")
    vtbls.append("const struct iface_entry ASYNC_INFO_ENTRY = { &IID_Windows_Foundation_IAsyncInfo, vtbl_Windows_Foundation_IAsyncInfo, \"IAsyncInfo\" };")
    hdr += [s for s in g.structs.values() if s]
    hdr += protos
    for b in bodies: w(b); w('')
    for v in vtbls: w(v); w('')

    # class tables
    for cn, td, insts, facts, dctor in class_infos:
        m = cn.replace('.', '_')
        w(f"static const struct iface_entry ifaces_{m}[] = {{")
        for info in insts:
            w(f"    {{ &IID_{info['cname']}, vtbl_{info['cname']}, \"{info['short']}\" }},")
        w("    { NULL }")
        w("};")
        w(f"static const struct iface_entry factory_ifaces_{m}[] = {{")
        for info in facts:
            w(f"    {{ &IID_{info['cname']}, vtbl_{info['cname']}, \"{info['short']}\" }},")
        w("    { NULL }")
        w("};")
        disp = f"DISPATCH_{td.short}" if f"DISPATCH_{td.short}" in overrides else "NULL"
        init = f"INIT_{td.short}" if f"INIT_{td.short}" in overrides else "NULL"
        if disp != "NULL":
            hdr.append(f"BOOL {disp}(struct obj *o, const GUID *iid, unsigned int idx, HRESULT *hr, void **args);")
        if init != "NULL":
            hdr.append(f"HRESULT {init}(struct obj *o);")
        w(f"struct class_info class_{m} = {{ L\"{cn}\", ifaces_{m}, factory_ifaces_{m}, {1 if dctor else 0}, {disp}, {init} }};")
        hdr.append(f"extern struct class_info class_{m};")
        w('')
    for sname, infos in synth_infos:
        m = sname.replace('.', '_')
        w(f"static const struct iface_entry ifaces_{m}[] = {{")
        for info in infos:
            w(f"    {{ &IID_{info['cname']}, vtbl_{info['cname']}, \"{info['short']}\" }},")
        w("    { NULL }")
        w("};")
        w(f"struct class_info class_{m} = {{ L\"{sname}\", ifaces_{m}, NULL, 0, NULL, NULL }};")
        hdr.append(f"extern struct class_info class_{m};")
        class_infos.append((sname, None, infos, [], False))
    w("struct class_info *all_classes[] = {")
    for cn, *_ in class_infos:
        w(f"    &class_{cn.replace('.', '_')},")
    w("    NULL")
    w("};")

    os.makedirs(os.path.join(shim, 'gen'), exist_ok=True)
    open(os.path.join(shim, 'gen', 'amshim_gen.c'), 'w').write('\n'.join(L) + '\n')
    open(os.path.join(shim, 'gen', 'amshim_gen.h'), 'w').write('\n'.join(hdr) + '\n')
    print(f"generated {len(class_infos)} classes, {len(g.ifaces)} interfaces")


def is_obj_type(db, t):
    return db.kind(t) == 'object'


def default_body(g, db, info, mname, m, plist, ret):
    b = []
    base = re.sub(r'_\d+$', '', mname)
    trace = f'    TRACE("%s {info["short"]}::{mname}\\n", debugstr_w(o->cls->name));'
    if base.startswith('add_') and len(plist) == 2:
        b += [trace, f"    if (result) result->Value = 1;", "    return S_OK;"]
        return b
    if base.startswith('remove_'):
        b += [trace, "    return S_OK;"]
        return b
    if base.startswith('put_') and len(m.params) == 1 and ret is None:
        prop = base[4:]
        k = db.kind(m.params[0].type)
        sz = db.size_align(m.params[0].type)[0]
        b += [trace, f"    return prop_put(o, L\"{prop}\", PK_{k.upper()}, &{plist[0][1]}, {sz});"]
        return b
    if base.startswith('get_') and not m.params and ret and ret[0] == 'value':
        prop = base[4:]
        k = db.kind(ret[1])
        sz = db.size_align(ret[1])[0]
        cls = ''
        if k == 'object' and ret[1].kind == 'named':
            cls = ret[1].name
        b += [trace, f"    return prop_get(o, L\"{prop}\", PK_{k.upper()}, result, {sz}, {cstr(cls)});"]
        return b
    # factory create methods: construct instance of the class
    if ret and ret[0] == 'value' and ret[1].kind == 'named' and db.get(ret[1].name) is not None \
            and db.get(ret[1].name).category == 'class' and (base.startswith('Create') or base == 'CreateInstance'):
        b += [trace, f"    hr = create_object_by_name(L\"{ret[1].name}\", (IInspectable **)result);",
              "    if (SUCCEEDED(hr) && *result) {"]
        for (ct, pn), p in zip(plist, m.params):
            if p.out or p.type.kind in ('array', 'byref'): continue
            k = db.kind(p.type)
            sz = db.size_align(p.type)[0]
            pn_cap = pn[0].upper() + pn[1:]
            b.append(f"        prop_put(impl_from_inspectable(*result), L\"{pn_cap}\", PK_{k.upper()}, &{pn}, {sz});")
        b += ["    }", "    return hr;"]
        return b
    # generic: zero outputs
    b.append(f'    FIXME("%s {info["short"]}::{mname} stub\\n", debugstr_w(o->cls->name));')
    for (ct, pn), p in zip(plist, m.params + [None] * (len(plist) - len(m.params))):
        pass
    outs = []
    for i, p in enumerate(m.params):
        if p.type.kind == 'byref':
            if p.type.inner.kind == 'array':
                outs += [f"{p.name}_len", f"{p.name}"]
            else:
                outs.append(p.name)
    if ret:
        if ret[0] == 'array': outs += ['result_len', 'result']
        else: outs.append('result')
    for on in outs:
        b.append(f"    if ({on}) memset({on}, 0, sizeof(*{on}));")
    if ret and ret[0] == 'value' and db.kind(ret[1]) == 'object':
        b.append("    return E_NOTIMPL;")
    else:
        b.append("    return S_OK;")
    return b


def cstr(s):
    return f'L"{s}"' if s else 'NULL'


if __name__ == '__main__':
    main()
