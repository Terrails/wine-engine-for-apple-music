/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Hand-written overrides: Windows.Security.Cryptography.Core hashing (bcrypt) */
#include "gen/amshim_gen.h"
#include <bcrypt.h>

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

HRESULT OV_IHashAlgorithmNamesStatics_get_Md5(struct obj *o, HSTRING *r) { return hstring_from_w(L"MD5", r); }
HRESULT OV_IHashAlgorithmNamesStatics_get_Sha1(struct obj *o, HSTRING *r) { return hstring_from_w(L"SHA1", r); }
HRESULT OV_IHashAlgorithmNamesStatics_get_Sha256(struct obj *o, HSTRING *r) { return hstring_from_w(L"SHA256", r); }
HRESULT OV_IHashAlgorithmNamesStatics_get_Sha384(struct obj *o, HSTRING *r) { return hstring_from_w(L"SHA384", r); }
HRESULT OV_IHashAlgorithmNamesStatics_get_Sha512(struct obj *o, HSTRING *r) { return hstring_from_w(L"SHA512", r); }

static const WCHAR *bcrypt_alg(const WCHAR *name)
{
    if (!wcsicmp(name, L"MD5")) return BCRYPT_MD5_ALGORITHM;
    if (!wcsicmp(name, L"SHA1")) return BCRYPT_SHA1_ALGORITHM;
    if (!wcsicmp(name, L"SHA256")) return BCRYPT_SHA256_ALGORITHM;
    if (!wcsicmp(name, L"SHA384")) return BCRYPT_SHA384_ALGORITHM;
    if (!wcsicmp(name, L"SHA512")) return BCRYPT_SHA512_ALGORITHM;
    return NULL;
}

struct hasher { BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE hash; UINT32 len; };

static void hasher_free(struct obj *o)
{
    struct hasher *h = o->priv;
    if (h->hash) BCryptDestroyHash(h->hash);
    if (h->alg) BCryptCloseAlgorithmProvider(h->alg, 0);
    free(h);
}

static HRESULT hasher_init(struct obj *o, const WCHAR *name, BOOL with_hash)
{
    struct hasher *h = calloc(1, sizeof(*h));
    const WCHAR *alg = bcrypt_alg(name);
    DWORD len = 0, size;
    HSTRING s;

    o->priv = h;
    o->priv_free = hasher_free;
    if (!alg || BCryptOpenAlgorithmProvider(&h->alg, alg, NULL, 0)) return NTE_BAD_ALGID;
    BCryptGetProperty(h->alg, BCRYPT_HASH_LENGTH, (UCHAR *)&len, sizeof(len), &size, 0);
    h->len = len;
    if (with_hash && BCryptCreateHash(h->alg, &h->hash, NULL, 0, NULL, 0, 0)) return E_FAIL;
    hstring_from_w(name, &s);
    prop_put(o, L"AlgorithmName", PK_STRING, &s, sizeof(s));
    WindowsDeleteString(s);
    prop_put(o, L"HashLength", PK_PRIM, &h->len, sizeof(h->len));
    return S_OK;
}

HRESULT OV_IHashAlgorithmProviderStatics_OpenAlgorithm(struct obj *o, HSTRING algorithm, void **result)
{
    HRESULT hr = create_object_by_name(L"Windows.Security.Cryptography.Core.HashAlgorithmProvider", (IInspectable **)result);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = hasher_init(impl_from_inspectable(*result), WindowsGetStringRawBuffer(algorithm, NULL), FALSE)))
    {
        IInspectable_Release((IInspectable *)*result);
        *result = NULL;
    }
    return hr;
}

HRESULT OV_IHashAlgorithmProvider_HashData(struct obj *o, void *data, void **result)
{
    struct hasher *h = o->priv;
    BYTE *d, out[64];
    UINT32 len;
    buffer_bytes(data, &d, &len, NULL);
    if (BCryptHash(h->alg, NULL, 0, d, len, out, h->len)) return E_FAIL;
    return byte_buffer_create(out, h->len, h->len, (IInspectable **)result);
}

HRESULT OV_IHashAlgorithmProvider_CreateHash(struct obj *o, void **result)
{
    HSTRING name;
    HRESULT hr = create_object_by_name(L"Windows.Security.Cryptography.Core.CryptographicHash", (IInspectable **)result);
    if (FAILED(hr)) return hr;
    prop_get(o, L"AlgorithmName", PK_STRING, &name, sizeof(name), NULL);
    hr = hasher_init(impl_from_inspectable(*result), WindowsGetStringRawBuffer(name, NULL), TRUE);
    WindowsDeleteString(name);
    return hr;
}

HRESULT OV_IHashComputation_Append(struct obj *o, void *data)
{
    struct hasher *h = o->priv;
    BYTE *d;
    UINT32 len;
    buffer_bytes(data, &d, &len, NULL);
    return BCryptHashData(h->hash, d, len, 0) ? E_FAIL : S_OK;
}

HRESULT OV_IHashComputation_GetValueAndReset(struct obj *o, void **result)
{
    struct hasher *h = o->priv;
    BYTE out[64];
    if (BCryptFinishHash(h->hash, out, h->len, 0)) return E_FAIL;
    BCryptDestroyHash(h->hash);
    h->hash = NULL;
    BCryptCreateHash(h->alg, &h->hash, NULL, 0, NULL, 0, 0);
    return byte_buffer_create(out, h->len, h->len, (IInspectable **)result);
}

/* ---- Windows.Security.Cryptography.CryptographicBuffer ---------------- */
#include <wincrypt.h>

HRESULT OV_ICryptographicBufferStatics_Compare(struct obj *o, void *a, void *b, boolean *result)
{
    BYTE *da, *db;
    UINT32 la, lb;
    buffer_bytes(a, &da, &la, NULL);
    buffer_bytes(b, &db, &lb, NULL);
    *result = la == lb && (!la || !memcmp(da, db, la));
    return S_OK;
}

HRESULT OV_ICryptographicBufferStatics_GenerateRandom(struct obj *o, UINT32 length, void **result)
{
    BYTE *d;
    UINT32 len;
    HRESULT hr = byte_buffer_create(NULL, length, length, (IInspectable **)result);
    if (FAILED(hr)) return hr;
    buffer_bytes(*result, &d, &len, NULL);
    BCryptGenRandom(NULL, d, length, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return S_OK;
}

HRESULT OV_ICryptographicBufferStatics_GenerateRandomNumber(struct obj *o, UINT32 *result)
{
    BCryptGenRandom(NULL, (BYTE *)result, sizeof(*result), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return S_OK;
}

HRESULT OV_ICryptographicBufferStatics_CreateFromByteArray(struct obj *o, UINT32 value_len, UINT8 *value, void **result)
{
    return byte_buffer_create(value, value_len, value_len, (IInspectable **)result);
}

HRESULT OV_ICryptographicBufferStatics_CopyToByteArray(struct obj *o, void *buffer, UINT32 *value_len, UINT8 **value)
{
    BYTE *d;
    UINT32 len;
    buffer_bytes(buffer, &d, &len, NULL);
    *value = CoTaskMemAlloc(len ? len : 1);
    if (!*value) return E_OUTOFMEMORY;
    memcpy(*value, d, len);
    *value_len = len;
    return S_OK;
}

HRESULT OV_ICryptographicBufferStatics_EncodeToHexString(struct obj *o, void *buffer, HSTRING *result)
{
    static const WCHAR hex[] = L"0123456789abcdef";
    BYTE *d;
    UINT32 len, i;
    WCHAR *s;
    HRESULT hr;
    buffer_bytes(buffer, &d, &len, NULL);
    s = malloc((len * 2 + 1) * sizeof(WCHAR));
    for (i = 0; i < len; i++) { s[2 * i] = hex[d[i] >> 4]; s[2 * i + 1] = hex[d[i] & 15]; }
    hr = WindowsCreateString(s, len * 2, result);
    free(s);
    return hr;
}

static int hexval(WCHAR c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

HRESULT OV_ICryptographicBufferStatics_DecodeFromHexString(struct obj *o, HSTRING value, void **result)
{
    UINT32 n, i;
    const WCHAR *s = WindowsGetStringRawBuffer(value, &n);
    BYTE *d;
    HRESULT hr;
    if (n % 2) return E_INVALIDARG;
    d = malloc(n / 2 + 1);
    for (i = 0; i < n / 2; i++)
    {
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) { free(d); return E_INVALIDARG; }
        d[i] = hi << 4 | lo;
    }
    hr = byte_buffer_create(d, n / 2, n / 2, (IInspectable **)result);
    free(d);
    return hr;
}

HRESULT OV_ICryptographicBufferStatics_EncodeToBase64String(struct obj *o, void *buffer, HSTRING *result)
{
    BYTE *d;
    UINT32 len;
    DWORD n = 0;
    WCHAR *s;
    HRESULT hr;
    buffer_bytes(buffer, &d, &len, NULL);
    if (!len) return WindowsCreateString(L"", 0, result);
    CryptBinaryToStringW(d, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &n);
    s = malloc((n + 1) * sizeof(WCHAR));
    CryptBinaryToStringW(d, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, s, &n);
    hr = WindowsCreateString(s, n, result);
    free(s);
    return hr;
}

HRESULT OV_ICryptographicBufferStatics_DecodeFromBase64String(struct obj *o, HSTRING value, void **result)
{
    UINT32 n;
    const WCHAR *s = WindowsGetStringRawBuffer(value, &n);
    DWORD len = 0;
    BYTE *d;
    HRESULT hr;
    if (!n) return byte_buffer_create(NULL, 0, 0, (IInspectable **)result);
    if (!CryptStringToBinaryW(s, n, CRYPT_STRING_BASE64, NULL, &len, NULL, NULL)) return E_INVALIDARG;
    d = malloc(len + 1);
    CryptStringToBinaryW(s, n, CRYPT_STRING_BASE64, d, &len, NULL, NULL);
    hr = byte_buffer_create(d, len, len, (IInspectable **)result);
    free(d);
    return hr;
}

/* BinaryStringEncoding: 0 Utf8, 1 Utf16LE, 2 Utf16BE */
HRESULT OV_ICryptographicBufferStatics_ConvertStringToBinary(struct obj *o, HSTRING value, INT32 encoding, void **result)
{
    UINT32 n, i;
    const WCHAR *s = WindowsGetStringRawBuffer(value, &n);
    HRESULT hr;
    if (encoding == 0)
    {
        int len = WideCharToMultiByte(CP_UTF8, 0, s, n, NULL, 0, NULL, NULL);
        char *u = malloc(len + 1);
        WideCharToMultiByte(CP_UTF8, 0, s, n, u, len, NULL, NULL);
        hr = byte_buffer_create(u, len, len, (IInspectable **)result);
        free(u);
        return hr;
    }
    if (FAILED(hr = byte_buffer_create(s, n * 2, n * 2, (IInspectable **)result))) return hr;
    if (encoding == 2)
    {
        BYTE *d; UINT32 len;
        buffer_bytes(*result, &d, &len, NULL);
        for (i = 0; i + 1 < len; i += 2) { BYTE t = d[i]; d[i] = d[i + 1]; d[i + 1] = t; }
    }
    return S_OK;
}

HRESULT OV_ICryptographicBufferStatics_ConvertBinaryToString(struct obj *o, INT32 encoding, void *buffer, HSTRING *result)
{
    BYTE *d;
    UINT32 len, i;
    WCHAR *w;
    HRESULT hr;
    int n;
    buffer_bytes(buffer, &d, &len, NULL);
    if (encoding == 0)
    {
        n = MultiByteToWideChar(CP_UTF8, 0, (char *)d, len, NULL, 0);
        w = malloc((n + 1) * sizeof(WCHAR));
        MultiByteToWideChar(CP_UTF8, 0, (char *)d, len, w, n);
    }
    else
    {
        n = len / 2;
        w = malloc((n + 1) * sizeof(WCHAR));
        for (i = 0; i < (UINT32)n; i++)
            w[i] = encoding == 2 ? (d[2 * i] << 8 | d[2 * i + 1]) : (d[2 * i] | d[2 * i + 1] << 8);
    }
    hr = WindowsCreateString(w, n, result);
    free(w);
    return hr;
}
