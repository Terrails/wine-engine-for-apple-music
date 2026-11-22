/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* amcertreq.dll: the X509_CERT_REQUEST_TO_BE_SIGNED encoder (CERT_REQUEST_INFO -> DER) that Wine's
 * crypt32 lacks ("Unimplemented encoder for lpszStructType OID 4"). ApplePushDirect.dll builds its
 * push client certificate request with CryptSignAndEncodeCertificate(X509_CERT_REQUEST_TO_BE_SIGNED),
 * which fails with ERROR_FILE_NOT_FOUND without it, so push never connects.
 *
 * Plugged in through crypt32's documented OID function registry (CryptDllEncodeObjectEx for
 * encoding type 1, OID #4); `regsvr32 amcertreq.dll` registers it. Only the outer
 * CertificationRequestInfo SEQUENCE is built here; the subject is already DER, the public key and
 * the attributes are encoded by crypt32's own X509_PUBLIC_KEY_INFO / PKCS_ATTRIBUTES encoders.
 *   CertificationRequestInfo ::= SEQUENCE { version INTEGER, subject Name,
 *       subjectPKInfo SubjectPublicKeyInfo, attributes [0] IMPLICIT SET OF Attribute } */
#include <stdarg.h>
#include <string.h>
#include <windef.h>
#include <winbase.h>
#include <wincrypt.h>

static HMODULE self;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) { self = inst; DisableThreadLibraryCalls(inst); }
    return TRUE;
}

/* DER length octets for len; returns their count (out may be NULL) */
static DWORD der_len(DWORD len, BYTE *out)
{
    BYTE tmp[4]; DWORD n = 0, i;
    if (len < 0x80) { if (out) out[0] = (BYTE)len; return 1; }
    while (len) { tmp[n++] = (BYTE)len; len >>= 8; }
    if (out) { out[0] = 0x80 | n; for (i = 0; i < n; i++) out[1 + i] = tmp[n - 1 - i]; }
    return 1 + n;
}

static BOOL encode_alloc(DWORD type, LPCSTR st, const void *info, BYTE **out, DWORD *size)
{
    return CryptEncodeObjectEx(type, st, info, CRYPT_ENCODE_ALLOC_FLAG, NULL, out, size);
}

BOOL WINAPI EncodeCertRequestToBeSigned(DWORD type, LPCSTR st, const void *pv, DWORD flags,
                                        PCRYPT_ENCODE_PARA para, void *encoded, DWORD *pcb)
{
    const CERT_REQUEST_INFO *ri = pv;
    CRYPT_ATTRIBUTES attrs;
    BYTE *spki = NULL, *att = NULL, *buf, ver[7], tmp[5];
    DWORD spki_len = 0, att_len = 0, ver_len, content, total, v, n, i;
    BOOL ok = FALSE;

    if (!ri || !pcb) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    /* version INTEGER, minimal and non-negative */
    v = ri->dwVersion; n = 0;
    do { tmp[n++] = (BYTE)v; v >>= 8; } while (v);
    if (tmp[n - 1] & 0x80) tmp[n++] = 0;
    ver[0] = 0x02; ver[1] = (BYTE)n;
    for (i = 0; i < n; i++) ver[2 + i] = tmp[n - 1 - i];
    ver_len = 2 + n;

    if (!encode_alloc(type, X509_PUBLIC_KEY_INFO, &ri->SubjectPublicKeyInfo, &spki, &spki_len))
        goto done;
    attrs.cAttr = ri->cAttribute; attrs.rgAttr = ri->rgAttribute;
    if (!encode_alloc(type, PKCS_ATTRIBUTES, &attrs, &att, &att_len) || att_len < 2 || att[0] != 0x31)
    {
        if (!GetLastError()) SetLastError(CRYPT_E_ASN1_ERROR);
        goto done;
    }
    content = ver_len + ri->Subject.cbData + spki_len + att_len;
    total = 1 + der_len(content, NULL) + content;

    if (flags & CRYPT_ENCODE_ALLOC_FLAG)
    {
        buf = (para && para->pfnAlloc) ? para->pfnAlloc(total) : LocalAlloc(LPTR, total);
        if (!buf) { SetLastError(ERROR_OUTOFMEMORY); goto done; }
        *(BYTE **)encoded = buf;
    }
    else
    {
        if (!encoded) { *pcb = total; ok = TRUE; goto done; }
        if (*pcb < total) { *pcb = total; SetLastError(ERROR_MORE_DATA); goto done; }
        buf = encoded;
    }
    *buf++ = 0x30;
    buf += der_len(content, buf);
    memcpy(buf, ver, ver_len); buf += ver_len;
    memcpy(buf, ri->Subject.pbData, ri->Subject.cbData); buf += ri->Subject.cbData;
    memcpy(buf, spki, spki_len); buf += spki_len;
    att[0] = 0xa0;  /* SET OF -> [0] IMPLICIT */
    memcpy(buf, att, att_len);
    *pcb = total;
    ok = TRUE;
done:
    LocalFree(spki);
    LocalFree(att);
    return ok;
}

#define ENC_TYPE X509_ASN_ENCODING
HRESULT WINAPI DllRegisterServer(void)
{
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    return CryptRegisterOIDFunction(ENC_TYPE, CRYPT_OID_ENCODE_OBJECT_EX_FUNC,
                                    X509_CERT_REQUEST_TO_BE_SIGNED, path,
                                    "EncodeCertRequestToBeSigned") ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

HRESULT WINAPI DllUnregisterServer(void)
{
    return CryptUnregisterOIDFunction(ENC_TYPE, CRYPT_OID_ENCODE_OBJECT_EX_FUNC,
                                      X509_CERT_REQUEST_TO_BE_SIGNED) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}
