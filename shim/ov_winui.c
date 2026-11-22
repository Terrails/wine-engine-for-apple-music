/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* WinAppSDK classes used by AMP.Services outside of any XAML UI (wrapper process). */
#include "amshim_rt.h"
#include "gen/amshim_gen.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

HRESULT OV_IPropertyChangedEventArgsFactory_CreateInstance(struct obj *o, HSTRING name, IInspectable *base,
                                                           IInspectable **inner, void **result)
{
    HRESULT hr;
    if (inner) *inner = NULL;
    if (base) FIXME("aggregation not supported\n");
    hr = create_object_by_name(L"Microsoft.UI.Xaml.Data.PropertyChangedEventArgs", (IInspectable **)result);
    if (SUCCEEDED(hr)) prop_put(impl_from_inspectable(*result), L"PropertyName", PK_STRING, &name, sizeof(name));
    return hr;
}

HRESULT OV_IThicknessHelperStatics_FromLengths(struct obj *o, DOUBLE left, DOUBLE top, DOUBLE right, DOUBLE bottom,
                                               S_Microsoft_UI_Xaml_Thickness *result)
{
    result->Left = left; result->Top = top; result->Right = right; result->Bottom = bottom;
    return S_OK;
}

HRESULT OV_IThicknessHelperStatics_FromUniformLength(struct obj *o, DOUBLE len, S_Microsoft_UI_Xaml_Thickness *result)
{
    result->Left = result->Top = result->Right = result->Bottom = len;
    return S_OK;
}

HRESULT OV_IColorHelperStatics_FromArgb(struct obj *o, UINT8 a, UINT8 r, UINT8 g, UINT8 b, S_Windows_UI_Color *result)
{
    result->A = a; result->R = r; result->G = g; result->B = b;
    return S_OK;
}

/* Windows.Foundation.Deferral: the generated factory stores the DeferralCompletedHandler as
 * the "Handler" property; Complete() invokes it once. */
HRESULT OV_IDeferral_Complete(struct obj *o)
{
    IUnknown *handler = NULL, *none = NULL;
    prop_get(o, L"Handler", PK_OBJECT, &handler, sizeof(handler), NULL);
    if (!handler) return S_OK;
    prop_put(o, L"Handler", PK_OBJECT, &none, sizeof(none));
    ((HRESULT (WINAPI *)(IUnknown *))(*(void ***)handler)[3])(handler);
    IUnknown_Release(handler);
    return S_OK;
}
