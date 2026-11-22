/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Storage: ApplicationData (overview: amshim_storage.h). */
#include "amshim_storage.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

/* ------------------------------------------------------------------ */
/* ApplicationData                                                     */

static WCHAR pkg_root[MAX_PATH];

static const WCHAR *package_root(void)
{
    if (!pkg_root[0])
    {
        WCHAR family[256];
        UINT32 len = ARRAY_SIZE(family);
        SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, pkg_root);
        if (GetCurrentPackageFamilyName(&len, family)) wcscpy(family, L"AppleInc.AppleMusicWin_nzyj5cx40ttqa");
        wcscat(pkg_root, L"\\Packages\\");
        wcscat(pkg_root, family);
        ensure_dir(pkg_root);
    }
    return pkg_root;
}

static void put_folder(struct obj *o, const WCHAR *prop, const WCHAR *sub)
{
    WCHAR path[MAX_PATH];
    IInspectable *f;
    swprintf(path, ARRAY_SIZE(path), L"%s\\%s", package_root(), sub);
    if (SUCCEEDED(folder_for(path, &f)))
    {
        prop_put(o, prop, PK_OBJECT, &f, sizeof(f));
        IInspectable_Release(f);
    }
}

/* key: the registry key holding the container's values (amshim_settings.c), owned by the new
 * container's Values from then on; NULL keeps them in memory */
static HRESULT container_create(const WCHAR *name, INT32 locality, HKEY key, IInspectable **out)
{
    IInspectable *c, *values, *ps;
    struct obj *co;
    HRESULT hr = create_object_by_name(L"Windows.Storage.ApplicationDataContainer", &c);
    if (FAILED(hr)) { if (key) RegCloseKey(key); return hr; }
    co = impl_from_inspectable(c);
    set_str(co, L"Name", name);
    prop_put(co, L"Locality", PK_PRIM, &locality, sizeof(locality));
    if (SUCCEEDED(create_object_by_name(L"Windows.Storage.ApplicationDataContainerSettings", &values)))
    {
        if (key) { omap_bind_registry(impl_from_inspectable(values), key); key = NULL; }
        if (SUCCEEDED(IInspectable_QueryInterface(values, &IID_Windows_Foundation_Collections_IPropertySet, (void **)&ps)))
        {
            prop_put(co, L"Values", PK_OBJECT, &ps, sizeof(ps));
            IInspectable_Release(ps);
        }
        IInspectable_Release(values);
    }
    if (key) RegCloseKey(key);
    *out = c;
    return S_OK;
}

/* the registry key of a container's values (NULL: in memory) */
static HKEY container_key(struct obj *o)
{
    struct prop *p = prop_find(o, L"Values");
    return p && p->v.unk ? omap_registry(impl_from_inspectable((IInspectable *)p->v.unk)) : NULL;
}

static void put_container(struct obj *o, const WCHAR *prop, const WCHAR *name, INT32 locality, const WCHAR *key)
{
    IInspectable *c;
    if (SUCCEEDED(container_create(name, locality, settings_open(NULL, key, TRUE), &c)))
    {
        prop_put(o, prop, PK_OBJECT, &c, sizeof(c));
        IInspectable_Release(c);
    }
}

/* ApplicationDataCreateDisposition: 0 Always, 1 Existing. Children live in the container's omap
 * and, for persistent containers, as subkeys of its registry key (so they exist in later runs). */
HRESULT OV_IApplicationDataContainer_CreateContainer(struct obj *o, HSTRING name, INT32 disposition, void **result)
{
    IInspectable *c = omap_get(o, name);
    HKEY parent, key = NULL;
    INT32 locality = 0;
    HRESULT hr;

    TRACE("%s disposition %d -> existing %p\n", debugstr_hstring(name), disposition, c);
    if (c) { *result = c; return S_OK; }
    if ((parent = container_key(o)))
    {
        key = settings_open(parent, WindowsGetStringRawBuffer(name, NULL), disposition != 1);
        if (!key && disposition != 1) return E_FAIL;
    }
    if (disposition == 1 && !key) { *result = NULL; return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND); }
    prop_get(o, L"Locality", PK_PRIM, &locality, sizeof(locality), NULL);
    if (FAILED(hr = container_create(WindowsGetStringRawBuffer(name, NULL), locality, key, &c))) return hr;
    omap_set(o, name, c);
    *result = c;
    return S_OK;
}

HRESULT OV_IApplicationDataContainer_DeleteContainer(struct obj *o, HSTRING name)
{
    HKEY parent = container_key(o);
    omap_remove(o, name);
    if (parent) RegDeleteTreeW(parent, WindowsGetStringRawBuffer(name, NULL));
    return S_OK;
}

HRESULT OV_IApplicationDataContainer_get_Containers(struct obj *o, void **result)
{
    HKEY parent = container_key(o);
    WCHAR name[256];
    DWORD i, len;
    HSTRING s;
    void *c;
    HRESULT hr;

    /* persisted children not opened in this run yet */
    for (i = 0; parent; i++)
    {
        len = ARRAY_SIZE(name);
        if (RegEnumKeyExW(parent, i, name, &len, NULL, NULL, NULL, NULL)) break;
        hstring_from_w(name, &s);
        if (SUCCEEDED(OV_IApplicationDataContainer_CreateContainer(o, s, 1, &c))) IInspectable_Release((IInspectable *)c);
        WindowsDeleteString(s);
    }
    hr = create_object(&class_amshim_ContainerMapView, (IInspectable **)result);
    if (SUCCEEDED(hr)) omap_copy(impl_from_inspectable(*result), o);
    return hr;
}

HRESULT INIT_ApplicationData(struct obj *o)
{
    TRACE("root %s\n", debugstr_w(package_root()));
    put_folder(o, L"LocalFolder", L"LocalState");
    put_folder(o, L"RoamingFolder", L"RoamingState");
    put_folder(o, L"TemporaryFolder", L"TempState");
    put_folder(o, L"LocalCacheFolder", L"LocalCache");
    put_container(o, L"LocalSettings", L"", 0, L"Local");
    put_container(o, L"RoamingSettings", L"", 1, L"Roaming");
    return S_OK;
}

static IInspectable *app_data;

HRESULT OV_IApplicationDataStatics_get_Current(struct obj *o, void **result)
{
    IInspectable *inst;
    if (!app_data)
    {
        HRESULT hr = create_object_by_name(L"Windows.Storage.ApplicationData", &inst);
        if (FAILED(hr)) return hr;
        if (InterlockedCompareExchangePointer((void **)&app_data, inst, NULL)) IInspectable_Release(inst);
    }
    IInspectable_AddRef(app_data);
    *result = app_data;
    return S_OK;
}

HRESULT OV_IApplicationDataStatics2_GetForUserAsync(struct obj *o, void *user, void **result)
{
    IInspectable *cur;
    HRESULT hr = OV_IApplicationDataStatics_get_Current(o, (void **)&cur);
    if (FAILED(hr)) return hr;
    hr = async_create(&ASYNC_IAsyncOperation_ApplicationData_, ASYNC_OP, S_OK, PK_OBJECT, &cur, sizeof(cur), result);
    IInspectable_Release(cur);
    return hr;
}

HRESULT OV_IApplicationData3_GetPublisherCacheFolder(struct obj *o, HSTRING name, void **result)
{
    WCHAR path[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, path);
    wcscat(path, L"\\Publishers\\nzyj5cx40ttqa\\");
    wcscat(path, WindowsGetStringRawBuffer(name, NULL));
    TRACE("%s\n", debugstr_w(path));
    return folder_for(path, (IInspectable **)result);
}

HRESULT OV_IApplicationData3_ClearPublisherCacheFolderAsync(struct obj *o, HSTRING name, void **result)
{
    return async_create(&ASYNC_IAsyncAction, ASYNC_ACTION, S_OK, PK_PRIM, NULL, 0, result);
}

HRESULT OV_IApplicationData_SetVersionAsync(struct obj *o, UINT32 version, void *handler, void **result)
{
    prop_put(o, L"Version", PK_PRIM, &version, sizeof(version));
    return async_create(&ASYNC_IAsyncAction, ASYNC_ACTION, S_OK, PK_PRIM, NULL, 0, result);
}

HRESULT OV_IApplicationData_ClearAsync(struct obj *o, void **result)
{
    FIXME("not clearing\n");
    return async_create(&ASYNC_IAsyncAction, ASYNC_ACTION, S_OK, PK_PRIM, NULL, 0, result);
}

HRESULT OV_IApplicationData_ClearAsync_2(struct obj *o, INT32 locality, void **result)
{
    return OV_IApplicationData_ClearAsync(o, result);
}
