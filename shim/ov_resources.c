/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Hand-written overrides: Windows.ApplicationModel.Resources{,.Core}
 * Lookups are served by the Windows App SDK's MRT Core (bundled with the app),
 * which reads the package's resources.pri. */
#include "gen/amshim_gen.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

static const GUID IID_MRT_IResourceManager = {0xac2291ef, 0x81be, 0x5c99, {0xa0, 0xae, 0xbc, 0xee, 0x01, 0x80, 0xb8, 0xa8}};
static const GUID IID_MRT_IResourceManagerFactory = {0xd6acf18f, 0x458a, 0x535b, {0xa5, 0xc4, 0xac, 0x2d, 0xc4, 0xe4, 0x90, 0x99}};
#define ERROR_MRM_NAMED_RESOURCE_NOT_FOUND_HR ((HRESULT)0x80073b1f)

static IUnknown *mrt_map;     /* Microsoft.Windows.ApplicationModel.Resources.IResourceMap */
static INIT_ONCE mrt_once = INIT_ONCE_STATIC_INIT;

#define VCALL(obj, slot, type) ((type)((void **)*(void ***)(obj))[slot])

static BOOL CALLBACK mrt_init(INIT_ONCE *once, void *param, void **ctx)
{
    HRESULT (WINAPI *get_factory)(HSTRING, IActivationFactory **);
    IActivationFactory *factory = NULL;
    IInspectable *inst = NULL;
    IUnknown *mgr = NULL;
    HMODULE mod;
    HSTRING cls;
    HRESULT hr;

    if (!(mod = LoadLibraryW(L"Microsoft.Windows.ApplicationModel.Resources.dll")) ||
        !(get_factory = (void *)GetProcAddress(mod, "DllGetActivationFactory")))
    {
        ERR("MRT Core not available\n");
        return TRUE;
    }
    hstring_from_w(L"Microsoft.Windows.ApplicationModel.Resources.ResourceManager", &cls);
    hr = get_factory(cls, &factory);
    WindowsDeleteString(cls);
    if (SUCCEEDED(hr))
    {
        /* explicit resources.pri path: avoids packaged-mode discovery via OS-internal services */
        WCHAR path[MAX_PATH], *p;
        IUnknown *f2;
        HSTRING hpath;
        GetModuleFileNameW(NULL, path, ARRAY_SIZE(path));
        if ((p = wcsrchr(path, '\\'))) p[1] = 0;
        wcscat(path, L"resources.pri");
        hstring_from_w(path, &hpath);
        hr = IActivationFactory_QueryInterface(factory, &IID_MRT_IResourceManagerFactory, (void **)&f2);
        /* IResourceManagerFactory::CreateInstance (slot 6) */
        if (SUCCEEDED(hr)) hr = VCALL(f2, 6, HRESULT (WINAPI *)(IUnknown *, HSTRING, IInspectable **))(f2, hpath, &inst);
        if (SUCCEEDED(hr)) IUnknown_Release(f2);
        WindowsDeleteString(hpath);
        TRACE("MRT ResourceManager(%s) hr %#lx\n", debugstr_w(path), hr);
    }
    if (SUCCEEDED(hr)) hr = IInspectable_QueryInterface(inst, &IID_MRT_IResourceManager, (void **)&mgr);
    /* IResourceManager::get_MainResourceMap (slot 6) */
    if (SUCCEEDED(hr)) hr = VCALL(mgr, 6, HRESULT (WINAPI *)(IUnknown *, IUnknown **))(mgr, &mrt_map);
    TRACE("MRT main map %p hr %#lx\n", mrt_map, hr);
    if (mgr) IUnknown_Release(mgr);
    if (inst) IInspectable_Release(inst);
    if (factory) IActivationFactory_Release(factory);
    return TRUE;
}

/* Normalise "ms-resource:..." references to a map-relative name. */
static void normalise(const WCHAR *ref, const WCHAR *prefix, WCHAR *out, size_t len)
{
    const WCHAR *p = ref;
    if (!wcsnicmp(p, L"ms-resource:", 12))
    {
        p += 12;
        if (p[0] == '/' && p[1] == '/')
        {
            p += 2;                         /* authority (package name), possibly empty */
            while (*p && *p != '/') p++;
        }
        while (*p == '/') p++;
        if (!wcschr(p, '/')) { swprintf(out, len, L"Resources/%s", p); return; }
        swprintf(out, len, L"%s", p);
        return;
    }
    if (*p == '/')
    {
        /* a leading slash makes the name absolute within the main map, e.g. "/AMP.UI.Views/Resources/X" */
        while (*p == '/') p++;
        swprintf(out, len, L"%s", p);
        return;
    }
    if (prefix && *prefix) swprintf(out, len, L"%s/%s", prefix, p);
    else swprintf(out, len, L"%s", p);
}

static HRESULT mrt_lookup(const WCHAR *name, HSTRING *value)
{
    IUnknown *cand = NULL;
    HSTRING hname;
    HRESULT hr;

    *value = NULL;
    InitOnceExecuteOnce(&mrt_once, mrt_init, NULL, NULL);
    if (!mrt_map) return E_NOTIMPL;
    hstring_from_w(name, &hname);
    /* IResourceMap::TryGetValue (slot 13) */
    hr = VCALL(mrt_map, 13, HRESULT (WINAPI *)(IUnknown *, HSTRING, IUnknown **))(mrt_map, hname, &cand);
    WindowsDeleteString(hname);
    if (SUCCEEDED(hr) && cand)
    {
        /* IResourceCandidate::get_ValueAsString (slot 6) */
        hr = VCALL(cand, 6, HRESULT (WINAPI *)(IUnknown *, HSTRING *))(cand, value);
        IUnknown_Release(cand);
    }
    else if (SUCCEEDED(hr)) hr = ERROR_MRM_NAMED_RESOURCE_NOT_FOUND_HR;
    TRACE("%s -> %#lx %s\n", debugstr_w(name), hr, debugstr_hstring(*value));
    return hr;
}

static const WCHAR *prop_str(struct obj *o, const WCHAR *name)
{
    struct prop *p = prop_find(o, name);
    return p && p->v.str ? WindowsGetStringRawBuffer(p->v.str, NULL) : L"";
}

static void put_str(struct obj *o, const WCHAR *name, const WCHAR *val)
{
    HSTRING s;
    hstring_from_w(val, &s);
    prop_put(o, name, PK_STRING, &s, sizeof(s));
    WindowsDeleteString(s);
}

/* ---- Core.ResourceContext -------------------------------------------- */

static HRESULT new_context(void **result)
{
    return create_object_by_name(L"Windows.ApplicationModel.Resources.Core.ResourceContext", (IInspectable **)result);
}

HRESULT OV_IResourceContextStatics2_GetForViewIndependentUse(struct obj *o, void **result) { return new_context(result); }
HRESULT OV_IResourceContextStatics2_GetForCurrentView(struct obj *o, void **result) { return new_context(result); }
HRESULT OV_IResourceContextStatics4_GetForUIContext(struct obj *o, void *ctx, void **result) { return new_context(result); }
HRESULT OV_IResourceContextStatics2_SetGlobalQualifierValue(struct obj *o, HSTRING k, HSTRING v) { return S_OK; }
HRESULT OV_IResourceContextStatics3_SetGlobalQualifierValue(struct obj *o, HSTRING k, HSTRING v, INT32 p) { return S_OK; }

/* ---- Core.ResourceManager / ResourceMap ------------------------------ */

static IInspectable *res_manager;

static HRESULT new_map(const WCHAR *prefix, void **result)
{
    HRESULT hr = create_object_by_name(L"Windows.ApplicationModel.Resources.Core.ResourceMap", (IInspectable **)result);
    if (SUCCEEDED(hr)) put_str(impl_from_inspectable(*result), L"Prefix", prefix);
    return hr;
}

HRESULT OV_IResourceManagerStatics_get_Current(struct obj *o, void **result)
{
    if (!res_manager)
    {
        IInspectable *inst, *map;
        HRESULT hr = create_object_by_name(L"Windows.ApplicationModel.Resources.Core.ResourceManager", &inst);
        if (FAILED(hr)) return hr;
        if (SUCCEEDED(new_map(L"", (void **)&map)))
        {
            prop_put(impl_from_inspectable(inst), L"MainResourceMap", PK_OBJECT, &map, sizeof(map));
            IInspectable_Release(map);
        }
        if (InterlockedCompareExchangePointer((void **)&res_manager, inst, NULL)) IInspectable_Release(inst);
    }
    IInspectable_AddRef(res_manager);
    *result = res_manager;
    return S_OK;
}

HRESULT OV_IResourceManagerStatics_IsResourceReference(struct obj *o, HSTRING ref, boolean *result)
{
    *result = !wcsnicmp(WindowsGetStringRawBuffer(ref, NULL), L"ms-resource:", 12);
    return S_OK;
}

HRESULT OV_IResourceMap_GetValue(struct obj *o, HSTRING resource, void **result)
{
    WCHAR name[1024];
    IInspectable *cand;
    HSTRING value;
    boolean yes = TRUE;
    HRESULT hr;

    normalise(WindowsGetStringRawBuffer(resource, NULL), prop_str(o, L"Prefix"), name, ARRAY_SIZE(name));
    if (FAILED(hr = mrt_lookup(name, &value))) { *result = NULL; return hr; }
    hr = create_object_by_name(L"Windows.ApplicationModel.Resources.Core.ResourceCandidate", &cand);
    if (SUCCEEDED(hr))
    {
        struct obj *c = impl_from_inspectable(cand);
        prop_put(c, L"ValueAsString", PK_STRING, &value, sizeof(value));
        prop_put(c, L"IsMatch", PK_PRIM, &yes, sizeof(yes));
        prop_put(c, L"IsDefault", PK_PRIM, &yes, sizeof(yes));
        *result = cand;
    }
    WindowsDeleteString(value);
    return hr;
}

HRESULT OV_IResourceMap_GetValue_2(struct obj *o, HSTRING resource, void *context, void **result)
{
    return OV_IResourceMap_GetValue(o, resource, result);
}

HRESULT OV_IResourceMap_GetSubtree(struct obj *o, HSTRING reference, void **result)
{
    WCHAR name[1024];
    normalise(WindowsGetStringRawBuffer(reference, NULL), prop_str(o, L"Prefix"), name, ARRAY_SIZE(name));
    return new_map(name, result);
}

/* ---- ResourceLoader -------------------------------------------------- */

static HRESULT new_loader(HSTRING name, void **result)
{
    HRESULT hr = create_object_by_name(L"Windows.ApplicationModel.Resources.ResourceLoader", (IInspectable **)result);
    if (SUCCEEDED(hr) && name) prop_put(impl_from_inspectable(*result), L"Name", PK_STRING, &name, sizeof(name));
    return hr;
}

HRESULT OV_IResourceLoaderStatics2_GetForCurrentView(struct obj *o, void **result) { return new_loader(NULL, result); }
HRESULT OV_IResourceLoaderStatics2_GetForCurrentView_2(struct obj *o, HSTRING n, void **result) { return new_loader(n, result); }
HRESULT OV_IResourceLoaderStatics2_GetForViewIndependentUse(struct obj *o, void **result) { return new_loader(NULL, result); }
HRESULT OV_IResourceLoaderStatics2_GetForViewIndependentUse_2(struct obj *o, HSTRING n, void **result) { return new_loader(n, result); }
HRESULT OV_IResourceLoaderStatics3_GetForUIContext(struct obj *o, void *c, void **result) { return new_loader(NULL, result); }

HRESULT OV_IResourceLoader_GetString(struct obj *o, HSTRING resource, HSTRING *result)
{
    WCHAR name[1024];
    const WCHAR *map = prop_str(o, L"Name");
    HRESULT hr;

    normalise(WindowsGetStringRawBuffer(resource, NULL), *map ? map : L"Resources", name, ARRAY_SIZE(name));
    hr = mrt_lookup(name, result);
    if (FAILED(hr)) { *result = NULL; return S_OK; } /* missing strings come back empty */
    return S_OK;
}

HRESULT INIT_ResourceContext(struct obj *o)
{
    IInspectable *langs;
    if (SUCCEEDED(user_languages_vector(&langs)))
    {
        prop_put(o, L"Languages", PK_OBJECT, &langs, sizeof(langs));
        IInspectable_Release(langs);
    }
    return S_OK;
}

/* AllResourceMaps: { <package name> : MainResourceMap } */
HRESULT OV_IResourceManager_get_AllResourceMaps(struct obj *o, void **result)
{
    IInspectable *map = NULL;
    HSTRING key;
    WCHAR name[] = L"AppleInc.AppleMusicWin";
    HRESULT hr = create_object(&class_amshim_ResourceMapMapView, (IInspectable **)result);
    if (FAILED(hr)) return hr;
    prop_get(o, L"MainResourceMap", PK_OBJECT, &map, sizeof(map), NULL);
    if (map)
    {
        hstring_from_w(name, &key);
        omap_set(impl_from_inspectable(*result), key, map);
        WindowsDeleteString(key);
        IInspectable_Release(map);
    }
    return S_OK;
}

HRESULT OV_IMapView_String_ResourceMap__Lookup(struct obj *o, HSTRING key, void **result)
{
    *result = omap_get(o, key);
    if (!*result) TRACE("no map %s\n", debugstr_hstring(key));
    return *result ? S_OK : E_BOUNDS;
}

HRESULT OV_IMapView_String_ResourceMap__HasKey(struct obj *o, HSTRING key, boolean *result)
{
    IInspectable *v = omap_get(o, key);
    *result = v != NULL;
    if (v) IInspectable_Release(v);
    return S_OK;
}

HRESULT OV_IMapView_String_ResourceMap__get_Size(struct obj *o, UINT32 *result)
{
    *result = 1;
    return S_OK;
}
