/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* CryptoAPI differences that break the push service (ApplePushDirect.dll) under Wine.
 *
 * APS makes an RSA key pair in the container "APNSDaemon", has Apple sign a client certificate
 * for it, attaches the provider to the certificate and gives it to CFNetwork, which hands it to
 * schannel for the TLS connection to the push courier. Two things go wrong:
 *
 * 1. CertSetCertificateContextProperty(CERT_KEY_PROV_HANDLE_PROP_ID): Windows takes the HCRYPTPROV
 *    itself as pvData (APS passes it that way); Wine's crypt32 reads an HCRYPTPROV from *pvData
 *    and so stores the first bytes of advapi32's provider object (its magic) as the handle.
 * 2. Wine's schannel reads a client certificate's private key from rsaenh's registry storage
 *    (HKCU\Software\Wine\Crypto\RSA\<container>), but rsaenh only writes a generated key pair
 *    there when the provider is released, which APS never does. Windows CSPs persist a new key
 *    pair at once. AcquireCredentialsHandle then fails with SEC_E_INTERNAL_ERROR.
 *
 * 3. APS creates the certificate context from a buffer with a few bytes after the DER
 *    certificate (1-2 seen). crypt32 keeps them in pbCertEncoded, and Wine's schannel hands the whole buffer
 *    to GnuTLS, which rejects it ("ASN1 parser: Error in DER parsing"). Trimmed here, so the copy
 *    APS adds to the "My" store is clean too (a copy stored before this fix must be deleted).
 *
 * Any of them: CFNetwork cannot set up the TLS settings ("CFReadStreamSetProperty failed on
 * kCFStreamPropertySSLSettings") and push never connects. */
#include "ampkg.h"
#include <wincrypt.h>

WINE_DEFAULT_DEBUG_CHANNEL(ampkg);

#define CRYPTPROV_MAGIC 0xa39e741f  /* advapi32's provider object starts with this */

static BOOL WINAPI my_CertSetCertificateContextProperty(const CERT_CONTEXT *cert, DWORD id, DWORD flags,
                                                        const void *data)
{
    HCRYPTPROV prov;
    if (id == CERT_KEY_PROV_HANDLE_PROP_ID && data && *(const DWORD *)data == CRYPTPROV_MAGIC)
    {
        /* pvData is the handle itself (Windows convention): give Wine a pointer to it */
        prov = (HCRYPTPROV)data;
        TRACE("KEY_PROV_HANDLE %#Ix passed by value\n", prov);
        return CertSetCertificateContextProperty(cert, id, flags, &prov);
    }
    return CertSetCertificateContextProperty(cert, id, flags, data);
}

/* rsaenh stores an imported key pair right away: generate it exportable, export it and import it
 * again with the caller's flags (so the caller's key keeps the exportability it asked for) */
static BOOL WINAPI my_CryptGenKey(HCRYPTPROV prov, ALG_ID alg, DWORD flags, HCRYPTKEY *key)
{
    HCRYPTKEY tmp, imported;
    BYTE *blob;
    DWORD len = 0, err;

    if (alg != AT_KEYEXCHANGE && alg != AT_SIGNATURE) return CryptGenKey(prov, alg, flags, key);
    if (!CryptGenKey(prov, alg, flags | CRYPT_EXPORTABLE, &tmp)) return FALSE;
    if (!CryptExportKey(tmp, 0, PRIVATEKEYBLOB, 0, NULL, &len) || !(blob = malloc(len)))
        goto keep;
    if (!CryptExportKey(tmp, 0, PRIVATEKEYBLOB, 0, blob, &len) ||
        !CryptImportKey(prov, blob, len, 0, flags & (CRYPT_EXPORTABLE | CRYPT_USER_PROTECTED), &imported))
    {
        SecureZeroMemory(blob, len);
        free(blob);
        goto keep;
    }
    SecureZeroMemory(blob, len);
    free(blob);
    CryptDestroyKey(tmp);
    TRACE("key pair %u stored (flags %#lx)\n", alg, flags);
    *key = imported;
    return TRUE;
keep:
    err = GetLastError();
    WARN("could not store key pair %u: %#lx\n", alg, err);
    *key = tmp;  /* still usable in this process */
    return TRUE;
}

/* length of the DER element at the start of buf (0 if it is not one or does not fit) */
static DWORD der_size(const BYTE *buf, DWORD size)
{
    DWORD len, n, i;
    if (size < 2) return 0;
    if (!(buf[1] & 0x80)) len = buf[1], n = 0;
    else
    {
        n = buf[1] & 0x7f;
        if (!n || n > 4 || size < 2 + n) return 0;
        for (len = 0, i = 0; i < n; i++) len = (len << 8) | buf[2 + i];
    }
    return (len <= size - 2 - n) ? 2 + n + len : 0;
}

static const CERT_CONTEXT * WINAPI my_CertCreateCertificateContext(DWORD type, const BYTE *data, DWORD size)
{
    DWORD der = data ? der_size(data, size) : 0;
    if (der && der < size && data[0] == 0x30)
    {
        TRACE("dropping %lu bytes after the certificate\n", size - der);
        size = der;
    }
    return CertCreateCertificateContext(type, data, size);
}

const struct hook crypto_hooks[] =
{
    HOOK(CertCreateCertificateContext),
    HOOK(CertSetCertificateContextProperty),
    HOOK(CryptGenKey),
    { NULL }
};
