/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Synthetic generic collections: IVectorView<String>, IIterable<String>, IIterator<String> */
#include "gen/amshim_gen.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

struct svec { HSTRING *items; UINT32 count; };
struct siter { struct obj *vec; UINT32 pos; };

static void svec_free(struct obj *o)
{
    struct svec *v = o->priv;
    UINT32 i;
    for (i = 0; i < v->count; i++) WindowsDeleteString(v->items[i]);
    free(v->items);
    free(v);
}

HRESULT string_vector_create(const WCHAR **strs, UINT32 count, IInspectable **out)
{
    struct svec *v;
    UINT32 i;
    HRESULT hr = create_object(&class_amshim_StringVectorView, out);
    if (FAILED(hr)) return hr;
    v = calloc(1, sizeof(*v));
    v->items = calloc(count ? count : 1, sizeof(HSTRING));
    for (i = 0; i < count; i++) hstring_from_w(strs[i], &v->items[i]);
    v->count = count;
    impl_from_inspectable(*out)->priv = v;
    impl_from_inspectable(*out)->priv_free = svec_free;
    return S_OK;
}

HRESULT OV_IVectorView_String__GetAt(struct obj *o, UINT32 index, HSTRING *result)
{
    struct svec *v = o->priv;
    if (index >= v->count) { *result = NULL; return E_BOUNDS; }
    return WindowsDuplicateString(v->items[index], result);
}

HRESULT OV_IVectorView_String__get_Size(struct obj *o, UINT32 *result)
{
    *result = ((struct svec *)o->priv)->count;
    return S_OK;
}

HRESULT OV_IVectorView_String__IndexOf(struct obj *o, HSTRING value, UINT32 *index, boolean *result)
{
    struct svec *v = o->priv;
    UINT32 i;
    INT32 r;
    for (i = 0; i < v->count; i++)
        if (SUCCEEDED(WindowsCompareStringOrdinal(v->items[i], value, &r)) && !r)
        {
            *index = i; *result = TRUE;
            return S_OK;
        }
    *index = 0; *result = FALSE;
    return S_OK;
}

HRESULT OV_IVectorView_String__GetMany(struct obj *o, UINT32 start, UINT32 items_len, HSTRING *items, UINT32 *result)
{
    struct svec *v = o->priv;
    UINT32 i;
    for (i = 0; start + i < v->count && i < items_len; i++)
        WindowsDuplicateString(v->items[start + i], &items[i]);
    *result = i;
    return S_OK;
}

static void siter_free(struct obj *o)
{
    struct siter *it = o->priv;
    IInspectable_Release((IInspectable *)&it->vec->ifcs[0]);
    free(it);
}

HRESULT OV_IIterable_String__First(struct obj *o, void **result)
{
    struct siter *it;
    HRESULT hr = create_object(&class_amshim_StringIterator, (IInspectable **)result);
    if (FAILED(hr)) return hr;
    it = calloc(1, sizeof(*it));
    it->vec = o;
    IInspectable_AddRef((IInspectable *)&o->ifcs[0]);
    impl_from_inspectable(*result)->priv = it;
    impl_from_inspectable(*result)->priv_free = siter_free;
    return S_OK;
}

HRESULT OV_IIterator_String__get_Current(struct obj *o, HSTRING *result)
{
    struct siter *it = o->priv;
    return OV_IVectorView_String__GetAt(it->vec, it->pos, result);
}

HRESULT OV_IIterator_String__get_HasCurrent(struct obj *o, boolean *result)
{
    struct siter *it = o->priv;
    *result = it->pos < ((struct svec *)it->vec->priv)->count;
    return S_OK;
}

HRESULT OV_IIterator_String__MoveNext(struct obj *o, boolean *result)
{
    struct siter *it = o->priv;
    UINT32 n = ((struct svec *)it->vec->priv)->count;
    if (it->pos < n) it->pos++;
    *result = it->pos < n;
    return S_OK;
}

HRESULT OV_IIterator_String__GetMany(struct obj *o, UINT32 items_len, HSTRING *items, UINT32 *result)
{
    struct siter *it = o->priv;
    HRESULT hr = OV_IVectorView_String__GetMany(it->vec, it->pos, items_len, items, result);
    it->pos += *result;
    return hr;
}

/* Languages for resource contexts etc. */
HRESULT user_languages_vector(IInspectable **out)
{
    WCHAR name[LOCALE_NAME_MAX_LENGTH];
    const WCHAR *list[2] = { name, L"en-US" };
    if (!GetUserDefaultLocaleName(name, ARRAY_SIZE(name))) wcscpy(name, L"en-US");
    return string_vector_create(list, wcscmp(name, L"en-US") ? 2 : 1, out);
}
