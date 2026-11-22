/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Hand-written overrides: small "feature not available" answers */
#include "gen/amshim_gen.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

/* Windows.Security.DataProtection: user data protection is not available */
HRESULT OV_IUserDataProtectionManagerStatics_TryGetDefault(struct obj *o, void **result)
{
    *result = NULL;
    return S_OK;
}

HRESULT OV_IUserDataProtectionManagerStatics_TryGetForUser(struct obj *o, void *user, void **result)
{
    *result = NULL;
    return S_OK;
}

/* Windows.Foundation.Metadata.ApiInformation (Wine's returns E_NOTIMPL).
 * Types: present if activatable. Members: report absent so callers take fallbacks. */
HRESULT OV_IApiInformationStatics_IsTypePresent(struct obj *o, HSTRING type, boolean *result)
{
    IActivationFactory *f = NULL;
    HRESULT hr = RoGetActivationFactory(type, &IID_IActivationFactory, (void **)&f);
    *result = SUCCEEDED(hr);
    if (f) IActivationFactory_Release(f);
    TRACE("%s -> %d\n", debugstr_hstring(type), *result);
    return S_OK;
}

HRESULT OV_IApiInformationStatics_IsMethodPresent(struct obj *o, HSTRING t, HSTRING m, boolean *result) { *result = FALSE; return S_OK; }
HRESULT OV_IApiInformationStatics_IsMethodPresent_2(struct obj *o, HSTRING t, HSTRING m, UINT32 n, boolean *result) { *result = FALSE; return S_OK; }
HRESULT OV_IApiInformationStatics_IsEventPresent(struct obj *o, HSTRING t, HSTRING e, boolean *result) { *result = FALSE; return S_OK; }
HRESULT OV_IApiInformationStatics_IsPropertyPresent(struct obj *o, HSTRING t, HSTRING p, boolean *result) { *result = FALSE; return S_OK; }
HRESULT OV_IApiInformationStatics_IsReadOnlyPropertyPresent(struct obj *o, HSTRING t, HSTRING p, boolean *result) { *result = FALSE; return S_OK; }
HRESULT OV_IApiInformationStatics_IsWriteablePropertyPresent(struct obj *o, HSTRING t, HSTRING p, boolean *result) { *result = FALSE; return S_OK; }
HRESULT OV_IApiInformationStatics_IsEnumNamedValuePresent(struct obj *o, HSTRING t, HSTRING v, boolean *result) { *result = FALSE; return S_OK; }
HRESULT OV_IApiInformationStatics_IsApiContractPresent(struct obj *o, HSTRING c, UINT16 major, boolean *result)
{
    *result = TRUE;
    TRACE("%s %u\n", debugstr_hstring(c), major);
    return S_OK;
}
HRESULT OV_IApiInformationStatics_IsApiContractPresent_2(struct obj *o, HSTRING c, UINT16 major, UINT16 minor, boolean *result)
{
    return OV_IApiInformationStatics_IsApiContractPresent(o, c, major, result);
}

/* ---- CoreApplication ----
 * AMP.Services keeps app-wide state in the CoreApplication property set, which it fetches
 * through an undocumented statics interface {17b0e613-942a-422d-904c-f90dc71a7dae}
 * (vtable slot 7 returns the set; the caller queries it for IMap<String, Object>).
 * Slot 6 is unknown and unused by the app. The set itself is Wine's PropertySet. */
static const GUID IID_ICoreApplicationPrivateProps = {0x17b0e613, 0x942a, 0x422d, {0x90, 0x4c, 0xf9, 0x0d, 0xc7, 0x1a, 0x7d, 0xae}};

static HRESULT app_properties(void **result)
{
    static IInspectable *props;
    static const WCHAR name[] = L"Windows.Foundation.Collections.PropertySet";
    *result = NULL;
    if (!props)
    {
        HSTRING_HEADER hdr;
        HSTRING cls;
        IInspectable *inst;
        HRESULT hr;
        WindowsCreateStringReference(name, ARRAY_SIZE(name) - 1, &hdr, &cls);
        if (FAILED(hr = RoActivateInstance(cls, &inst))) return hr;
        if (InterlockedCompareExchangePointer((void **)&props, inst, NULL)) IInspectable_Release(inst);
    }
    IInspectable_AddRef(props);
    *result = props;
    return S_OK;
}

static HRESULT WINAPI capp_QueryInterface(struct ifc *This, REFIID iid, void **out) { return obj_query(This->obj, iid, out); }
static HRESULT WINAPI capp_slot6(struct ifc *This, void **out)
{
    FIXME("unknown private CoreApplication method\n");
    if (out) *out = NULL;
    return E_NOTIMPL;
}
static HRESULT WINAPI capp_get_Properties(struct ifc *This, void **out)
{
    TRACE("\n");
    return app_properties(out);
}
static const void *capp_vtbl[] = { capp_QueryInterface, ifc_AddRef, ifc_Release, ifc_GetIids, ifc_GetRuntimeClassName,
                                   ifc_GetTrustLevel, capp_slot6, capp_get_Properties };

HRESULT OV_ICoreApplication_get_Properties(struct obj *o, void **result) { return app_properties(result); }

void factory_created(struct obj *factory)
{
    if (!wcscmp(factory->cls->name, L"Windows.ApplicationModel.Core.CoreApplication"))
    {
        factory->extra_iid = &IID_ICoreApplicationPrivateProps;
        factory->extra.vtbl = capp_vtbl;
        factory->extra.obj = factory;
    }
}
