/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Hand-written overrides: Windows.ApplicationModel.Package / PackageId (current package only) */
#include "gen/amshim_gen.h"
#include <appmodel.h>

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

LONG WINAPI GetCurrentPackageId(UINT32 *, BYTE *);
LONG WINAPI GetCurrentPackageFullName(UINT32 *, WCHAR *);
LONG WINAPI GetCurrentPackageFamilyName(UINT32 *, WCHAR *);
LONG WINAPI GetCurrentPackagePath(UINT32 *, WCHAR *);

static void set_s(struct obj *o, const WCHAR *name, const WCHAR *val)
{
    HSTRING s;
    hstring_from_w(val ? val : L"", &s);
    prop_put(o, name, PK_STRING, &s, sizeof(s));
    WindowsDeleteString(s);
}

HRESULT INIT_PackageId(struct obj *o)
{
    BYTE buf[1024];
    UINT32 len = sizeof(buf);
    WCHAR name[256];
    PACKAGE_ID *id = (PACKAGE_ID *)buf;
    S_Windows_ApplicationModel_PackageVersion ver;
    INT32 arch;

    if (GetCurrentPackageId(&len, buf)) return S_OK;
    set_s(o, L"Name", id->name);
    set_s(o, L"Publisher", id->publisher);
    set_s(o, L"PublisherId", id->publisherId);
    set_s(o, L"ResourceId", id->resourceId);
    ver.Major = id->version.Major; ver.Minor = id->version.Minor;
    ver.Build = id->version.Build; ver.Revision = id->version.Revision;
    prop_put(o, L"Version", PK_STRUCT, &ver, sizeof(ver));
    arch = 9; /* ProcessorArchitecture.X64 */
    prop_put(o, L"Architecture", PK_PRIM, &arch, sizeof(arch));
    len = ARRAY_SIZE(name);
    if (!GetCurrentPackageFullName(&len, name)) set_s(o, L"FullName", name);
    len = ARRAY_SIZE(name);
    if (!GetCurrentPackageFamilyName(&len, name)) set_s(o, L"FamilyName", name);
    return S_OK;
}

HRESULT INIT_Package(struct obj *o)
{
    WCHAR path[MAX_PATH];
    UINT32 len = ARRAY_SIZE(path);
    IInspectable *folder;

    set_s(o, L"DisplayName", L"Apple Music");
    set_s(o, L"PublisherDisplayName", L"Apple Inc.");
    set_s(o, L"Description", L"Apple Music");
    if (!GetCurrentPackagePath(&len, path))
    {
        const WCHAR *names[] = { L"InstalledPath", L"EffectivePath", L"MutablePath" };
        unsigned int i;
        for (i = 0; i < ARRAY_SIZE(names); i++) set_s(o, names[i], path);
        if (SUCCEEDED(storage_folder_create(path, &folder)))
        {
            prop_put(o, L"InstalledLocation", PK_OBJECT, &folder, sizeof(folder));
            prop_put(o, L"EffectiveLocation", PK_OBJECT, &folder, sizeof(folder));
            IInspectable_Release(folder);
        }
    }
    return S_OK;
}

static IInspectable *current_package;

HRESULT OV_IPackageStatics_get_Current(struct obj *o, void **result)
{
    if (!current_package)
    {
        IInspectable *inst;
        HRESULT hr = create_object_by_name(L"Windows.ApplicationModel.Package", &inst);
        if (FAILED(hr)) return hr;
        if (InterlockedCompareExchangePointer((void **)&current_package, inst, NULL)) IInspectable_Release(inst);
    }
    IInspectable_AddRef(current_package);
    *result = current_package;
    return S_OK;
}

/* ---- Windows.Management.Deployment.PackageManager: Wine's Find* methods return E_NOTIMPL,
 * which the app turns into a fail-fast. Report that no other packages are installed
 * (the app looks for iTunes). An empty IIterable<String> is ABI-compatible with an empty
 * IIterable<Package>. ---- */
static HRESULT empty_iterable(void **result)
{
    IInspectable *vec;
    HRESULT hr = string_vector_create(NULL, 0, &vec);
    if (FAILED(hr)) return hr;
    hr = IInspectable_QueryInterface(vec, &IID_Windows_Foundation_Collections_IIterable__String__, result);
    IInspectable_Release(vec);
    return hr;
}

HRESULT OV_IPackageManager_FindPackages(struct obj *o, void **result) { return empty_iterable(result); }
HRESULT OV_IPackageManager_FindPackagesForUser(struct obj *o, HSTRING sid, void **result) { return empty_iterable(result); }
HRESULT OV_IPackageManager_FindPackages_2(struct obj *o, HSTRING name, HSTRING publisher, void **result) { return empty_iterable(result); }
HRESULT OV_IPackageManager_FindPackagesForUser_2(struct obj *o, HSTRING sid, HSTRING name, HSTRING publisher, void **result) { return empty_iterable(result); }
HRESULT OV_IPackageManager_FindPackages_3(struct obj *o, HSTRING family, void **result) { return empty_iterable(result); }
HRESULT OV_IPackageManager_FindPackagesForUser_3(struct obj *o, HSTRING sid, HSTRING family, void **result) { return empty_iterable(result); }
