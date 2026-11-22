/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Hand-written overrides: Windows.Storage.Streams.DataReader */
#include "gen/amshim_gen.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

#define VCALL(obj, slot, type) ((type)((void **)*(void ***)(obj))[slot])

struct dreader { BYTE *data; UINT32 len, pos; IUnknown *stream; INT32 order; INT32 encoding; };

static void dreader_free(struct obj *o)
{
    struct dreader *r = o->priv;
    if (r->stream) IUnknown_Release(r->stream);
    free(r->data);
    free(r);
}

static struct dreader *rd(struct obj *o)
{
    if (!o->priv)
    {
        struct dreader *r = calloc(1, sizeof(*r));
        r->order = 1; /* BigEndian */
        o->priv = r;
        o->priv_free = dreader_free;
    }
    return o->priv;
}

HRESULT OV_IDataReaderStatics_FromBuffer(struct obj *o, void *buffer, void **result)
{
    BYTE *d;
    UINT32 len;
    struct dreader *r;
    HRESULT hr = create_object_by_name(L"Windows.Storage.Streams.DataReader", (IInspectable **)result);
    if (FAILED(hr)) return hr;
    r = rd(impl_from_inspectable(*result));
    buffer_bytes(buffer, &d, &len, NULL);
    r->data = malloc(len ? len : 1);
    memcpy(r->data, d, len);
    r->len = len;
    return S_OK;
}

HRESULT OV_IDataReaderFactory_CreateDataReader(struct obj *o, void *stream, void **result)
{
    HRESULT hr = create_object_by_name(L"Windows.Storage.Streams.DataReader", (IInspectable **)result);
    if (FAILED(hr)) return hr;
    rd(impl_from_inspectable(*result))->stream = stream;
    IUnknown_AddRef((IUnknown *)stream);
    return S_OK;
}

HRESULT OV_IDataReader_LoadAsync(struct obj *o, UINT32 count, void **result)
{
    struct dreader *r = rd(o);
    UINT32 loaded = 0;
    if (r->stream)
    {
        IInspectable *chunk, *op, *res = NULL;
        HRESULT hr = byte_buffer_create(NULL, 0, count, &chunk);
        if (FAILED(hr)) return hr;
        /* IInputStream::ReadAsync (slot 6), options None=0 */
        hr = VCALL(r->stream, 6, HRESULT (WINAPI *)(IUnknown *, IInspectable *, UINT32, UINT32, IInspectable **))(r->stream, chunk, count, 0, &op);
        if (SUCCEEDED(hr)) hr = async_wait((IUnknown *)op);
        if (SUCCEEDED(hr)) hr = VCALL(op, 10, HRESULT (WINAPI *)(IInspectable *, IInspectable **))(op, &res);
        if (op) IInspectable_Release(op);
        if (SUCCEEDED(hr) && res)
        {
            BYTE *d;
            buffer_bytes((IUnknown *)res, &d, &loaded, NULL);
            /* drop consumed bytes, append new ones */
            memmove(r->data, r->data + r->pos, r->len - r->pos);
            r->len -= r->pos; r->pos = 0;
            r->data = realloc(r->data, r->len + loaded + 1);
            memcpy(r->data + r->len, d, loaded);
            r->len += loaded;
            IInspectable_Release(res);
        }
        IInspectable_Release(chunk);
        if (FAILED(hr)) return hr;
    }
    return async_create(&class_amshim_U32Async.ifaces[0], ASYNC_OP, S_OK, PK_PRIM, &loaded, sizeof(loaded), result);
}

HRESULT OV_IDataReader_get_UnconsumedBufferLength(struct obj *o, UINT32 *result) { struct dreader *r = rd(o); *result = r->len - r->pos; return S_OK; }
HRESULT OV_IDataReader_get_ByteOrder(struct obj *o, INT32 *result) { *result = rd(o)->order; return S_OK; }
HRESULT OV_IDataReader_put_ByteOrder(struct obj *o, INT32 value) { rd(o)->order = value; return S_OK; }
HRESULT OV_IDataReader_get_UnicodeEncoding(struct obj *o, INT32 *result) { *result = rd(o)->encoding; return S_OK; }
HRESULT OV_IDataReader_put_UnicodeEncoding(struct obj *o, INT32 value) { rd(o)->encoding = value; return S_OK; }

static HRESULT take(struct obj *o, void *out, UINT32 n, BOOL swap)
{
    struct dreader *r = rd(o);
    BYTE *b = out;
    UINT32 i;
    if (r->len - r->pos < n) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    memcpy(out, r->data + r->pos, n);
    r->pos += n;
    if (swap && r->order == 1)
        for (i = 0; i < n / 2; i++) { BYTE t = b[i]; b[i] = b[n - 1 - i]; b[n - 1 - i] = t; }
    return S_OK;
}

HRESULT OV_IDataReader_ReadByte(struct obj *o, UINT8 *r) { return take(o, r, 1, FALSE); }
HRESULT OV_IDataReader_ReadBoolean(struct obj *o, boolean *r) { UINT8 b; HRESULT hr = take(o, &b, 1, FALSE); *r = !!b; return hr; }
HRESULT OV_IDataReader_ReadBytes(struct obj *o, UINT32 value_len, UINT8 *value) { return take(o, value, value_len, FALSE); }
HRESULT OV_IDataReader_ReadInt16(struct obj *o, INT16 *r) { return take(o, r, 2, TRUE); }
HRESULT OV_IDataReader_ReadInt32(struct obj *o, INT32 *r) { return take(o, r, 4, TRUE); }
HRESULT OV_IDataReader_ReadInt64(struct obj *o, INT64 *r) { return take(o, r, 8, TRUE); }
HRESULT OV_IDataReader_ReadUInt16(struct obj *o, UINT16 *r) { return take(o, r, 2, TRUE); }
HRESULT OV_IDataReader_ReadUInt32(struct obj *o, UINT32 *r) { return take(o, r, 4, TRUE); }
HRESULT OV_IDataReader_ReadUInt64(struct obj *o, UINT64 *r) { return take(o, r, 8, TRUE); }
HRESULT OV_IDataReader_ReadSingle(struct obj *o, FLOAT *r) { return take(o, r, 4, TRUE); }
HRESULT OV_IDataReader_ReadDouble(struct obj *o, DOUBLE *r) { return take(o, r, 8, TRUE); }
HRESULT OV_IDataReader_ReadDateTime(struct obj *o, S_Windows_Foundation_DateTime *r) { return take(o, r, 8, TRUE); }
HRESULT OV_IDataReader_ReadTimeSpan(struct obj *o, S_Windows_Foundation_TimeSpan *r) { return take(o, r, 8, TRUE); }
HRESULT OV_IDataReader_ReadGuid(struct obj *o, GUID *g)
{
    HRESULT hr = take(o, &g->Data1, 4, TRUE);
    if (SUCCEEDED(hr)) hr = take(o, &g->Data2, 2, TRUE);
    if (SUCCEEDED(hr)) hr = take(o, &g->Data3, 2, TRUE);
    if (SUCCEEDED(hr)) hr = take(o, g->Data4, 8, FALSE);
    return hr;
}

HRESULT OV_IDataReader_ReadBuffer(struct obj *o, UINT32 length, void **result)
{
    struct dreader *r = rd(o);
    HRESULT hr;
    if (r->len - r->pos < length) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    hr = byte_buffer_create(r->data + r->pos, length, length, (IInspectable **)result);
    if (SUCCEEDED(hr)) r->pos += length;
    return hr;
}

/* codeUnitCount: bytes for UTF-8, 16-bit units for UTF-16 */
HRESULT OV_IDataReader_ReadString(struct obj *o, UINT32 count, HSTRING *result)
{
    struct dreader *r = rd(o);
    UINT32 bytes = r->encoding == 0 ? count : count * 2, i;
    WCHAR *w;
    int n;
    HRESULT hr;
    if (r->len - r->pos < bytes) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    if (r->encoding == 0)
    {
        n = MultiByteToWideChar(CP_UTF8, 0, (char *)r->data + r->pos, bytes, NULL, 0);
        w = malloc((n + 1) * sizeof(WCHAR));
        MultiByteToWideChar(CP_UTF8, 0, (char *)r->data + r->pos, bytes, w, n);
    }
    else
    {
        BYTE *d = r->data + r->pos;
        n = count;
        w = malloc((n + 1) * sizeof(WCHAR));
        for (i = 0; i < count; i++)
            w[i] = r->encoding == 2 ? (d[2 * i] << 8 | d[2 * i + 1]) : (d[2 * i] | d[2 * i + 1] << 8);
    }
    r->pos += bytes;
    hr = WindowsCreateString(w, n, result);
    free(w);
    return hr;
}

HRESULT OV_IDataReader_DetachStream(struct obj *o, void **result)
{
    struct dreader *r = rd(o);
    *result = r->stream;
    r->stream = NULL;
    return S_OK;
}
