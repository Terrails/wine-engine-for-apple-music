/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* IBuffer (with IBufferByteAccess) and an IInputStream reading from a buffer */
#include "gen/amshim_gen.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

const GUID IID_IBufferByteAccess = {0x905a0fef, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};

struct bytes { BYTE *data; UINT32 cap, len; };

static void bytes_free(struct obj *o)
{
    struct bytes *b = o->priv;
    free(b->data);
    free(b);
}

/* IBufferByteAccess tear-off: QI, AddRef, Release, Buffer */
static HRESULT WINAPI bba_QueryInterface(struct ifc *This, REFIID iid, void **out) { return obj_query(This->obj, iid, out); }
static HRESULT WINAPI bba_Buffer(struct ifc *This, BYTE **value)
{
    *value = ((struct bytes *)This->obj->priv)->data;
    return S_OK;
}
static const void *bba_vtbl[] = { bba_QueryInterface, ifc_AddRef, ifc_Release, bba_Buffer };

HRESULT byte_buffer_create(const void *data, UINT32 len, UINT32 cap, IInspectable **out)
{
    struct bytes *b;
    struct obj *o;
    HRESULT hr = create_object(&class_amshim_ByteBuffer, out);
    if (FAILED(hr)) return hr;
    b = calloc(1, sizeof(*b));
    b->cap = max(cap, len);
    b->data = malloc(b->cap ? b->cap : 1);
    if (data) memcpy(b->data, data, len);
    b->len = len;
    o = impl_from_inspectable(*out);
    o->priv = b;
    o->priv_free = bytes_free;
    o->extra_iid = &IID_IBufferByteAccess;
    o->extra.vtbl = bba_vtbl;
    o->extra.obj = o;
    return S_OK;
}

HRESULT OV_IBuffer_get_Capacity(struct obj *o, UINT32 *result) { *result = ((struct bytes *)o->priv)->cap; return S_OK; }
HRESULT OV_IBuffer_get_Length(struct obj *o, UINT32 *result) { *result = ((struct bytes *)o->priv)->len; return S_OK; }
HRESULT OV_IBuffer_put_Length(struct obj *o, UINT32 value)
{
    struct bytes *b = o->priv;
    if (value > b->cap) return E_INVALIDARG;
    b->len = value;
    return S_OK;
}

#define VCALL(obj, slot, type) ((type)((void **)*(void ***)(obj))[slot])

/* Access the bytes of any IBuffer (ours or foreign). */
HRESULT buffer_bytes(IUnknown *buf, BYTE **data, UINT32 *len, UINT32 *cap)
{
    IUnknown *bba, *ib;
    HRESULT hr;

    *data = NULL; *len = 0;
    if (!buf) return S_OK;
    if (FAILED(hr = IUnknown_QueryInterface(buf, &IID_IBufferByteAccess, (void **)&bba))) return hr;
    VCALL(bba, 3, HRESULT (WINAPI *)(IUnknown *, BYTE **))(bba, data);
    IUnknown_Release(bba);
    if (FAILED(hr = IUnknown_QueryInterface(buf, &IID_Windows_Storage_Streams_IBuffer, (void **)&ib))) return hr;
    if (cap) VCALL(ib, 6, HRESULT (WINAPI *)(IUnknown *, UINT32 *))(ib, cap);   /* get_Capacity */
    VCALL(ib, 7, HRESULT (WINAPI *)(IUnknown *, UINT32 *))(ib, len);              /* get_Length */
    IUnknown_Release(ib);
    return S_OK;
}

HRESULT buffer_set_length(IUnknown *buf, UINT32 len)
{
    IUnknown *ib;
    HRESULT hr = IUnknown_QueryInterface(buf, &IID_Windows_Storage_Streams_IBuffer, (void **)&ib);
    if (FAILED(hr)) return hr;
    hr = VCALL(ib, 8, HRESULT (WINAPI *)(IUnknown *, UINT32))(ib, len);       /* put_Length */
    IUnknown_Release(ib);
    return hr;
}

/* ---- BufferInputStream ------------------------------------------------- */

struct bstream { IInspectable *buf; UINT32 pos; };

static void bstream_free(struct obj *o)
{
    struct bstream *s = o->priv;
    if (s->buf) IInspectable_Release(s->buf);
    free(s);
}

HRESULT buffer_input_stream_create(IInspectable *buf, IInspectable **out)
{
    struct bstream *s;
    HRESULT hr = create_object(&class_amshim_BufferInputStream, out);
    if (FAILED(hr)) return hr;
    s = calloc(1, sizeof(*s));
    s->buf = buf;
    IInspectable_AddRef(buf);
    impl_from_inspectable(*out)->priv = s;
    impl_from_inspectable(*out)->priv_free = bstream_free;
    return S_OK;
}

HRESULT OV_IInputStream_ReadAsync(struct obj *o, void *buffer, UINT32 count, UINT32 options, void **result)
{
    struct bstream *s = o->priv;
    BYTE *src, *dst;
    UINT32 len, dlen, dcap = 0, n;
    HRESULT hr;

    buffer_bytes((IUnknown *)s->buf, &src, &len, NULL);
    if (FAILED(hr = buffer_bytes(buffer, &dst, &dlen, &dcap))) return hr;
    n = min(min(count, dcap), len - min(s->pos, len));
    memcpy(dst, src + s->pos, n);
    s->pos += n;
    buffer_set_length(buffer, n);
    return async_create(&ASYNC_IAsyncOperationWithProgress_IBuffer_UInt32_, ASYNC_OP_PROGRESS, S_OK,
                        PK_OBJECT, &buffer, sizeof(buffer), result);
}
