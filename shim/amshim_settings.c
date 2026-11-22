/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Persistent ApplicationData settings (the values behind ApplicationDataContainer.Values).
 *
 * LocalSettings / RoamingSettings and their child containers are registry keys under
 *   HKCU\Software\AppleMusicWrapper\Settings\{Local,Roaming}[\<child container>...]
 * One registry value per setting. Its registry type is SETTINGS_REG_TYPE + the value's
 * Windows.Foundation.PropertyType, its data the raw value (little endian; strings UTF-16 with a
 * terminating NUL; Boolean one byte; DateTime/TimeSpan an INT64 of 100 ns units).
 * Values are read from the registry on every lookup, so processes sharing the prefix (amd.exe,
 * lab.exe, the agent) see each other's changes, as with the package's settings on Windows.
 * Values come in and go out as Wine's Windows.Foundation.PropertyValue objects. Types that are
 * not handled here (arrays, Point/Size/Rect, composite values, other objects) return
 * E_NOTIMPL from settings_write; the map then keeps them in memory (ov_collections.c). */
#include "gen/amshim_gen.h"
#include <winreg.h>

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

#define SETTINGS_ROOT L"Software\\AppleMusicWrapper\\Settings"
#define SETTINGS_REG_TYPE 0x100

static const GUID IID_IPropertyValue = {0x4bd682dd, 0x7554, 0x40e9, {0x9a, 0x9b, 0x82, 0x65, 0x4e, 0xde, 0x7e, 0x62}};
static const GUID IID_IPropertyValueStatics = {0x629bdbc8, 0xd932, 0x4ff4, {0x96, 0xb9, 0x8d, 0x96, 0xc5, 0xc1, 0xe8, 0x58}};

#define VCALL(obj, slot, type) ((type)((void **)*(void ***)(obj))[slot])

/* Windows.Foundation.PropertyType */
enum
{
    PT_UINT8 = 1, PT_INT16, PT_UINT16, PT_INT32, PT_UINT32, PT_INT64, PT_UINT64, PT_SINGLE, PT_DOUBLE,
    PT_CHAR16, PT_BOOLEAN, PT_STRING, PT_INSPECTABLE, PT_DATETIME, PT_TIMESPAN, PT_GUID,
};

/* size of a fixed-size value; 0 for types not stored */
static unsigned int value_size(INT32 type)
{
    switch (type)
    {
    case PT_UINT8: case PT_BOOLEAN: return 1;
    case PT_INT16: case PT_UINT16: case PT_CHAR16: return 2;
    case PT_INT32: case PT_UINT32: case PT_SINGLE: return 4;
    case PT_INT64: case PT_UINT64: case PT_DOUBLE: case PT_DATETIME: case PT_TIMESPAN: return 8;
    case PT_GUID: return 16;
    }
    return 0;
}

/* IPropertyValue::GetXxx and IPropertyValueStatics::CreateXxx vtable slots */
static int getter_slot(INT32 type)
{
    if (type >= PT_UINT8 && type <= PT_STRING) return 7 + type;
    switch (type) { case PT_GUID: return 20; case PT_DATETIME: return 21; case PT_TIMESPAN: return 22; }
    return -1;
}

static int creator_slot(INT32 type)
{
    if (type >= PT_UINT8 && type <= PT_STRING) return 6 + type;
    switch (type) { case PT_GUID: return 20; case PT_DATETIME: return 21; case PT_TIMESPAN: return 22; }
    return -1;
}

static IUnknown *pv_statics(void)
{
    static IUnknown *statics;
    IUnknown *s;
    HSTRING cls;

    if (statics) return statics;
    hstring_from_w(L"Windows.Foundation.PropertyValue", &cls);
    if (FAILED(RoGetActivationFactory(cls, &IID_IPropertyValueStatics, (void **)&s))) s = NULL;
    WindowsDeleteString(cls);
    if (s && InterlockedCompareExchangePointer((void **)&statics, s, NULL)) IUnknown_Release(s);
    return statics;
}

HKEY settings_open(HKEY parent, const WCHAR *sub, BOOL create)
{
    WCHAR path[512];
    HKEY key;
    LSTATUS st;

    if (!parent)
    {
        swprintf(path, ARRAY_SIZE(path), L"%s\\%s", SETTINGS_ROOT, sub);
        parent = HKEY_CURRENT_USER;
        sub = path;
    }
    if (create) st = RegCreateKeyExW(parent, sub, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL);
    else st = RegOpenKeyExW(parent, sub, 0, KEY_ALL_ACCESS, &key);
    if (st) { if (create) ERR("cannot create settings key %s: %ld\n", debugstr_w(sub), st); return NULL; }
    return key;
}

HRESULT settings_write(HKEY key, HSTRING name, IInspectable *value)
{
    union { BYTE b[16]; HSTRING s; } v;
    IUnknown *pv;
    INT32 type = 0;
    const WCHAR *str;
    UINT32 len;
    LSTATUS st;
    HRESULT hr;
    int slot = -1;

    if (!value) return E_NOTIMPL;
    if (FAILED(IInspectable_QueryInterface(value, &IID_IPropertyValue, (void **)&pv))) return E_NOTIMPL;
    hr = VCALL(pv, 6, HRESULT (WINAPI *)(IUnknown *, INT32 *))(pv, &type);   /* get_Type */
    if (SUCCEEDED(hr) && (slot = getter_slot(type)) < 0) hr = E_NOTIMPL;
    if (SUCCEEDED(hr)) hr = VCALL(pv, slot, HRESULT (WINAPI *)(IUnknown *, void *))(pv, &v);
    IUnknown_Release(pv);
    if (FAILED(hr))
    {
        FIXME("%s: PropertyType %d not stored (%08lx), kept in memory\n", debugstr_hstring(name), type, hr);
        return E_NOTIMPL;
    }
    if (type == PT_STRING)
    {
        str = WindowsGetStringRawBuffer(v.s, &len);
        st = RegSetValueExW(key, WindowsGetStringRawBuffer(name, NULL), 0, SETTINGS_REG_TYPE + type,
                            (const BYTE *)str, (len + 1) * sizeof(WCHAR));
        WindowsDeleteString(v.s);
    }
    else st = RegSetValueExW(key, WindowsGetStringRawBuffer(name, NULL), 0, SETTINGS_REG_TYPE + type,
                             v.b, value_size(type));
    TRACE("%s type %d -> %ld\n", debugstr_hstring(name), type, st);
    return st ? HRESULT_FROM_WIN32(st) : S_OK;
}

/* S_FALSE (and NULL) when the value does not exist */
HRESULT settings_read(HKEY key, HSTRING name, IInspectable **out)
{
    const WCHAR *n = WindowsGetStringRawBuffer(name, NULL);
    union { BYTE b[16]; UINT8 u8; INT16 i16; UINT16 u16; INT32 i32; UINT32 u32; INT64 i64; UINT64 u64;
            FLOAT f; DOUBLE d; WCHAR c; boolean z; GUID g; } v;
    IUnknown *statics;
    DWORD type, size = 0;
    WCHAR *buf;
    HSTRING s;
    HRESULT hr;
    INT32 pt;
    int slot;

    *out = NULL;
    if (RegQueryValueExW(key, n, NULL, &type, NULL, &size)) return S_FALSE;
    pt = type - SETTINGS_REG_TYPE;
    if ((slot = creator_slot(pt)) < 0 || (pt != PT_STRING && size != value_size(pt)))
    {
        ERR("%s: unexpected registry type %#lx size %lu\n", debugstr_w(n), type, size);
        return S_FALSE;
    }
    if (!(statics = pv_statics())) return E_FAIL;

#define CREATE(T, field) VCALL(statics, slot, HRESULT (WINAPI *)(IUnknown *, T, IInspectable **))(statics, v.field, out)
    if (pt == PT_STRING)
    {
        if (!(buf = calloc(1, size + sizeof(WCHAR)))) return E_OUTOFMEMORY;
        RegQueryValueExW(key, n, NULL, NULL, (BYTE *)buf, &size);
        WindowsCreateString(buf, wcslen(buf), &s);
        free(buf);
        hr = VCALL(statics, slot, HRESULT (WINAPI *)(IUnknown *, HSTRING, IInspectable **))(statics, s, out);
        WindowsDeleteString(s);
    }
    else
    {
        RegQueryValueExW(key, n, NULL, NULL, v.b, &size);
        switch (pt)
        {
        case PT_UINT8:   hr = CREATE(UINT8, u8); break;
        case PT_INT16:   hr = CREATE(INT16, i16); break;
        case PT_UINT16:  hr = CREATE(UINT16, u16); break;
        case PT_INT32:   hr = CREATE(INT32, i32); break;
        case PT_UINT32:  hr = CREATE(UINT32, u32); break;
        case PT_INT64: case PT_DATETIME: case PT_TIMESPAN: hr = CREATE(INT64, i64); break;
        case PT_UINT64:  hr = CREATE(UINT64, u64); break;
        case PT_SINGLE:  hr = CREATE(FLOAT, f); break;
        case PT_DOUBLE:  hr = CREATE(DOUBLE, d); break;
        case PT_CHAR16:  hr = CREATE(WCHAR, c); break;
        case PT_BOOLEAN: hr = CREATE(boolean, z); break;
        case PT_GUID:    hr = CREATE(GUID, g); break;
        default:         hr = E_NOTIMPL;
        }
    }
#undef CREATE
    TRACE("%s type %d -> %p (%08lx)\n", debugstr_w(n), pt, *out, hr);
    return FAILED(hr) ? hr : S_OK;
}

BOOL settings_has(HKEY key, HSTRING name)
{
    return !RegQueryValueExW(key, WindowsGetStringRawBuffer(name, NULL), NULL, NULL, NULL, NULL);
}

UINT32 settings_count(HKEY key)
{
    DWORD n = 0;
    RegQueryInfoKeyW(key, NULL, NULL, NULL, NULL, NULL, NULL, &n, NULL, NULL, NULL, NULL);
    return n;
}

void settings_delete(HKEY key, HSTRING name)
{
    RegDeleteValueW(key, WindowsGetStringRawBuffer(name, NULL));
}

/* all values of the container (not its child containers) */
void settings_clear(HKEY key)
{
    WCHAR name[16384];
    DWORD len;
    for (;;)
    {
        len = ARRAY_SIZE(name);
        if (RegEnumValueW(key, 0, name, &len, NULL, NULL, NULL, NULL)) break;
        if (RegDeleteValueW(key, name)) break;
    }
}
