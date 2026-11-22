#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Write the agent's COM registration (a .reg file) from the app's own AppxManifest.xml.

usage: genagentreg.py <AppxManifest.xml> <out.reg> [install dir, Windows path]

Nothing installs the MSIX under Wine, so the COM classes and proxy/stub interfaces the package
manifest declares are registered by hand:
  - every com:ExeServer class -> HKCR\\CLSID\\{id}\\LocalServer32 = "<dir>\\<exe>" -Embedding
  - every proxyStub class     -> HKCR\\CLSID\\{id}\\InprocServer32 = <dir>\\<dll>, ThreadingModel Both
  - every proxyStub interface -> HKCR\\Interface\\{iid}\\ProxyStubClsid32 = {proxy class}
The names and GUIDs come from the user's copy of the app; none are stored in this repository.
"""
import sys
import xml.etree.ElementTree as ET

DEFAULT_DIR = r'C:\Program Files\AppleMusic'


def local(tag):
    return tag.rsplit('}', 1)[-1]


def reg_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__.strip().splitlines()[2])
    manifest, out = sys.argv[1], sys.argv[2]
    appdir = sys.argv[3] if len(sys.argv) == 4 else DEFAULT_DIR
    root = ET.parse(manifest).getroot()

    lines = ['Windows Registry Editor Version 5.00', '']

    def key(path, values):
        lines.append('[HKEY_CLASSES_ROOT\\%s]' % path)
        for name, value in values:
            lines.append('%s=%s' % ('@' if name is None else reg_str(name), reg_str(value)))
        lines.append('')

    servers = proxies = 0
    for exe in root.iter():
        if local(exe.tag) != 'ExeServer':
            continue
        path = '%s\\%s' % (appdir, exe.get('Executable'))
        for cls in exe:
            if local(cls.tag) != 'Class':
                continue
            clsid = '{%s}' % cls.get('Id').upper()
            key('CLSID\\' + clsid, [(None, cls.get('DisplayName', ''))])
            key('CLSID\\%s\\LocalServer32' % clsid, [(None, '"%s" -Embedding' % path)])
            servers += 1

    for ps in root.iter():
        if local(ps.tag) != 'ProxyStub':
            continue
        clsid = '{%s}' % ps.get('ClassId').upper()
        dll = next((c.text.strip() for c in ps if local(c.tag) == 'Path'), None)
        if not dll:
            sys.exit('genagentreg: ProxyStub %s has no Path' % clsid)
        name = dll.rsplit('.', 2)[0] if dll.lower().endswith('.proxies.dll') else dll
        key('CLSID\\' + clsid, [(None, '%s PSFactory' % name)])
        key('CLSID\\%s\\InprocServer32' % clsid,
            [(None, '%s\\%s' % (appdir, dll)), ('ThreadingModel', 'Both')])
        for itf in ps:
            if local(itf.tag) != 'Interface':
                continue
            iid = '{%s}' % itf.get('InterfaceId').upper()
            key('Interface\\' + iid, [(None, itf.get('Name', ''))])
            key('Interface\\%s\\ProxyStubClsid32' % iid, [(None, clsid)])
            proxies += 1

    if not servers or not proxies:
        sys.exit('genagentreg: %s declares no COM server or no proxy/stub interfaces' % manifest)
    with open(out, 'w', newline='\n') as f:
        f.write('\n'.join(lines))
    print('genagentreg: %d class(es), %d interface(s) -> %s' % (servers, proxies, out))


if __name__ == '__main__':
    main()
