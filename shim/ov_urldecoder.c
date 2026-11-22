/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Foundation.WwwFormUrlDecoder: parses "a=1&b=two%20words" (optionally with a leading
 * '?') into name/value entries. Wine has no implementation. */
#include "gen/amshim_gen.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

struct uentry { WCHAR *name, *value; };
struct udec { struct uentry *e; UINT32 count; };
struct uiter { struct obj *owner; UINT32 pos; };

static void udec_free(struct obj *o)
{
    struct udec *d = o->priv;
    UINT32 i;
    for (i = 0; i < d->count; i++) { free(d->e[i].name); free(d->e[i].value); }
    free(d->e);
    free(d);
}

/* percent-decode [s, s+len) as UTF-8, '+' is a space */
static WCHAR *form_decode(const WCHAR *s, size_t len)
{
    char *utf8 = malloc(len * 3 + 1);
    size_t i, n = 0;
    int wlen;
    WCHAR *w;
    for (i = 0; i < len; i++)
    {
        WCHAR c = s[i];
        if (c == '+') utf8[n++] = ' ';
        else if (c == '%' && i + 2 < len + 0 && iswxdigit(s[i + 1]) && iswxdigit(s[i + 2]))
        {
            WCHAR hex[3] = { s[i + 1], s[i + 2], 0 };
            utf8[n++] = (char)wcstoul(hex, NULL, 16);
            i += 2;
        }
        else n += WideCharToMultiByte(CP_UTF8, 0, &c, 1, utf8 + n, 3, NULL, NULL);
    }
    wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, n, NULL, 0);
    w = malloc((wlen + 1) * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, utf8, n, w, wlen);
    w[wlen] = 0;
    free(utf8);
    return w;
}

HRESULT OV_IWwwFormUrlDecoderRuntimeClassFactory_CreateWwwFormUrlDecoder(struct obj *o, HSTRING query, void **result)
{
    const WCHAR *p = WindowsGetStringRawBuffer(query, NULL), *end;
    struct udec *d;
    HRESULT hr = create_object_by_name(L"Windows.Foundation.WwwFormUrlDecoder", (IInspectable **)result);
    if (FAILED(hr)) return hr;
    d = calloc(1, sizeof(*d));
    impl_from_inspectable(*result)->priv = d;
    impl_from_inspectable(*result)->priv_free = udec_free;
    if (*p == '?') p++;
    while (*p)
    {
        const WCHAR *eq;
        for (end = p; *end && *end != '&'; end++) ;
        for (eq = p; eq < end && *eq != '='; eq++) ;
        if (end > p)
        {
            d->e = realloc(d->e, (d->count + 1) * sizeof(*d->e));
            d->e[d->count].name = form_decode(p, eq - p);
            d->e[d->count].value = eq < end ? form_decode(eq + 1, end - eq - 1) : form_decode(p, 0);
            d->count++;
        }
        p = *end ? end + 1 : end;
    }
    TRACE("%u entries\n", d->count);
    return S_OK;
}

HRESULT OV_IWwwFormUrlDecoderRuntimeClass_GetFirstValueByName(struct obj *o, HSTRING name, HSTRING *result)
{
    struct udec *d = o->priv;
    const WCHAR *n = WindowsGetStringRawBuffer(name, NULL);
    UINT32 i;
    *result = NULL;
    for (i = 0; d && i < d->count; i++)
        if (!wcscmp(d->e[i].name, n)) return hstring_from_w(d->e[i].value, result);
    return E_INVALIDARG;   /* what Windows returns for a missing name */
}

static HRESULT entry_at(struct obj *o, UINT32 index, void **result)
{
    struct udec *d = o->priv;
    IInspectable *entry;
    HSTRING h;
    HRESULT hr;
    *result = NULL;
    if (!d || index >= d->count) return E_BOUNDS;
    if (FAILED(hr = create_object_by_name(L"Windows.Foundation.WwwFormUrlDecoderEntry", &entry))) return hr;
    hstring_from_w(d->e[index].name, &h);
    prop_put(impl_from_inspectable(entry), L"Name", PK_STRING, &h, sizeof(h));
    WindowsDeleteString(h);
    hstring_from_w(d->e[index].value, &h);
    prop_put(impl_from_inspectable(entry), L"Value", PK_STRING, &h, sizeof(h));
    WindowsDeleteString(h);
    *result = entry;
    return S_OK;
}

HRESULT OV_IVectorView_IWwwFormUrlDecoderEntry__GetAt(struct obj *o, UINT32 index, void **result) { return entry_at(o, index, result); }
HRESULT OV_IVectorView_IWwwFormUrlDecoderEntry__get_Size(struct obj *o, UINT32 *result)
{
    struct udec *d = o->priv;
    *result = d ? d->count : 0;
    return S_OK;
}
HRESULT OV_IVectorView_IWwwFormUrlDecoderEntry__IndexOf(struct obj *o, void *value, UINT32 *index, boolean *result)
{
    *index = 0; *result = FALSE;
    return S_OK;
}
HRESULT OV_IVectorView_IWwwFormUrlDecoderEntry__GetMany(struct obj *o, UINT32 start, UINT32 items_len, void **items, UINT32 *result)
{
    UINT32 i = 0;
    while (i < items_len && SUCCEEDED(entry_at(o, start + i, &items[i]))) i++;
    *result = i;
    return S_OK;
}

static void uiter_free(struct obj *o)
{
    struct uiter *it = o->priv;
    IInspectable_Release((IInspectable *)&it->owner->ifcs[0]);
    free(it);
}

HRESULT OV_IIterable_IWwwFormUrlDecoderEntry__First(struct obj *o, void **result)
{
    struct uiter *it;
    HRESULT hr = create_object(&class_amshim_UrlEntryIterator, (IInspectable **)result);
    if (FAILED(hr)) return hr;
    it = calloc(1, sizeof(*it));
    it->owner = o;
    IInspectable_AddRef((IInspectable *)&o->ifcs[0]);
    impl_from_inspectable(*result)->priv = it;
    impl_from_inspectable(*result)->priv_free = uiter_free;
    return S_OK;
}

static UINT32 uiter_count(struct uiter *it) { struct udec *d = it->owner->priv; return d ? d->count : 0; }

HRESULT OV_IIterator_IWwwFormUrlDecoderEntry__get_Current(struct obj *o, void **result)
{
    struct uiter *it = o->priv;
    return entry_at(it->owner, it->pos, result);
}
HRESULT OV_IIterator_IWwwFormUrlDecoderEntry__get_HasCurrent(struct obj *o, boolean *result)
{
    struct uiter *it = o->priv;
    *result = it->pos < uiter_count(it);
    return S_OK;
}
HRESULT OV_IIterator_IWwwFormUrlDecoderEntry__MoveNext(struct obj *o, boolean *result)
{
    struct uiter *it = o->priv;
    if (it->pos < uiter_count(it)) it->pos++;
    *result = it->pos < uiter_count(it);
    return S_OK;
}
HRESULT OV_IIterator_IWwwFormUrlDecoderEntry__GetMany(struct obj *o, UINT32 items_len, void **items, UINT32 *result)
{
    struct uiter *it = o->priv;
    UINT32 i = 0;
    while (i < items_len && it->pos < uiter_count(it) && SUCCEEDED(entry_at(it->owner, it->pos, &items[i]))) { i++; it->pos++; }
    *result = i;
    return S_OK;
}
