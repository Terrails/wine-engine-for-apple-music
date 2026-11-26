/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* ampkg: fake MSIX package identity (AppleInc.AppleMusicWin), see ampkg.h. The package path
 * is the directory of the executable. */
#include "ampkg.h"

WINE_DEFAULT_DEBUG_CHANNEL(ampkg);

#define PKG_NAME       L"AppleInc.AppleMusicWin"
#define PKG_PUBLISHER  L"CN=5BD5593D-A41B-4F89-884E-B4F3E0FBAA75"
#define PKG_PUBID      L"nzyj5cx40ttqa"
#define PKG_VERSION_W  L"1.1792.39942.0"
#define PKG_FULLNAME   PKG_NAME L"_" PKG_VERSION_W L"_x64__" PKG_PUBID
#define PKG_FAMILY     PKG_NAME L"_" PKG_PUBID
#define PKG_VER_MAJOR  1
#define PKG_VER_MINOR  1792
#define PKG_VER_BUILD  39942
#define PKG_VER_REV    0

#ifndef APPMODEL_ERROR_NO_PACKAGE
#define APPMODEL_ERROR_NO_PACKAGE 15700
#endif

typedef int PackagePathType;
typedef struct
{
    UINT32 reserved;
    UINT32 flags;
    WCHAR *path;
    WCHAR *packageFullName;
    WCHAR *packageFamilyName;
    PACKAGE_ID packageId;
} PACKAGE_INFO;

static WCHAR pkg_path[MAX_PATH];
static WCHAR app_id[64] = L"App";


static LONG copy_str(const WCHAR *s, UINT32 *len, WCHAR *buf)
{
    UINT32 need = wcslen(s) + 1;
    if (!len) return ERROR_INVALID_PARAMETER;
    if (!buf || *len < need) { *len = need; return ERROR_INSUFFICIENT_BUFFER; }
    memcpy(buf, s, need * sizeof(WCHAR));
    *len = need;
    return ERROR_SUCCESS;
}

static BOOL is_our_package(const WCHAR *full)
{
    return full && !wcsicmp(full, PKG_FULLNAME);
}

static LONG WINAPI my_GetCurrentPackageFullName(UINT32 *len, WCHAR *name)
{
    TRACE("\n");
    return copy_str(PKG_FULLNAME, len, name);
}

static LONG WINAPI my_GetCurrentPackageFamilyName(UINT32 *len, WCHAR *name)
{
    TRACE("\n");
    return copy_str(PKG_FAMILY, len, name);
}

static LONG WINAPI my_GetCurrentPackagePath(UINT32 *len, WCHAR *path)
{
    TRACE("\n");
    return copy_str(pkg_path, len, path);
}

static LONG WINAPI my_GetCurrentPackagePath2(PackagePathType type, UINT32 *len, WCHAR *path)
{
    return copy_str(pkg_path, len, path);
}

static LONG WINAPI my_GetCurrentApplicationUserModelId(UINT32 *len, WCHAR *id)
{
    WCHAR buf[256];
    wcscpy(buf, PKG_FAMILY L"!");
    wcscat(buf, app_id);
    TRACE("-> %s\n", debugstr_w(buf));
    return copy_str(buf, len, id);
}

static LONG WINAPI my_GetPackageFullName(HANDLE process, UINT32 *len, WCHAR *name)
{
    return copy_str(PKG_FULLNAME, len, name);
}

static LONG WINAPI my_GetPackageFamilyName(HANDLE process, UINT32 *len, WCHAR *name)
{
    return copy_str(PKG_FAMILY, len, name);
}

static LONG WINAPI my_GetApplicationUserModelId(HANDLE process, UINT32 *len, WCHAR *id)
{
    return my_GetCurrentApplicationUserModelId(len, id);
}

static LONG WINAPI my_GetPackagePathByFullName(const WCHAR *full, UINT32 *len, WCHAR *path)
{
    TRACE("%s\n", debugstr_w(full));
    if (!is_our_package(full)) return ERROR_NOT_FOUND;
    return copy_str(pkg_path, len, path);
}

static LONG WINAPI my_GetPackagePathByFullName2(const WCHAR *full, PackagePathType type, UINT32 *len, WCHAR *path)
{
    return my_GetPackagePathByFullName(full, len, path);
}

static LONG WINAPI my_GetStagedPackagePathByFullName(const WCHAR *full, UINT32 *len, WCHAR *path)
{
    return my_GetPackagePathByFullName(full, len, path);
}

/* PACKAGE_ID / PACKAGE_INFO blobs ---------------------------------- */

static size_t strings_size(void)
{
    return (wcslen(PKG_NAME) + wcslen(PKG_PUBLISHER) + 1 + wcslen(PKG_PUBID) + 3) * sizeof(WCHAR);
}

static WCHAR *put_str(WCHAR **dst, const WCHAR *s)
{
    WCHAR *r = *dst;
    wcscpy(r, s);
    *dst += wcslen(s) + 1;
    return r;
}

static void fill_id(PACKAGE_ID *id, WCHAR **strs)
{
    memset(id, 0, sizeof(*id));
    id->processorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
    id->version.Major = PKG_VER_MAJOR;
    id->version.Minor = PKG_VER_MINOR;
    id->version.Build = PKG_VER_BUILD;
    id->version.Revision = PKG_VER_REV;
    id->name = put_str(strs, PKG_NAME);
    id->publisher = put_str(strs, PKG_PUBLISHER);
    id->resourceId = put_str(strs, L"");
    id->publisherId = put_str(strs, PKG_PUBID);
}

static LONG WINAPI my_GetCurrentPackageId(UINT32 *len, BYTE *buffer)
{
    UINT32 need = sizeof(PACKAGE_ID) + strings_size();
    WCHAR *strs;
    TRACE("\n");
    if (!len) return ERROR_INVALID_PARAMETER;
    if (!buffer || *len < need) { *len = need; return ERROR_INSUFFICIENT_BUFFER; }
    strs = (WCHAR *)(buffer + sizeof(PACKAGE_ID));
    fill_id((PACKAGE_ID *)buffer, &strs);
    *len = need;
    return ERROR_SUCCESS;
}

static LONG WINAPI my_GetPackageId(HANDLE process, UINT32 *len, BYTE *buffer)
{
    return my_GetCurrentPackageId(len, buffer);
}

static UINT32 info_size(void)
{
    return sizeof(PACKAGE_INFO) + strings_size() +
           (wcslen(pkg_path) + wcslen(PKG_FULLNAME) + wcslen(PKG_FAMILY) + 3) * sizeof(WCHAR);
}

static void fill_info(BYTE *buffer)
{
    PACKAGE_INFO *info = (PACKAGE_INFO *)buffer;
    WCHAR *strs = (WCHAR *)(buffer + sizeof(PACKAGE_INFO));
    memset(info, 0, sizeof(*info));
    info->path = put_str(&strs, pkg_path);
    info->packageFullName = put_str(&strs, PKG_FULLNAME);
    info->packageFamilyName = put_str(&strs, PKG_FAMILY);
    fill_id(&info->packageId, &strs);
}

static LONG WINAPI my_GetCurrentPackageInfo(UINT32 flags, UINT32 *len, BYTE *buffer, UINT32 *count)
{
    UINT32 need = info_size();
    TRACE("flags %#x\n", flags);
    if (!len) return ERROR_INVALID_PARAMETER;
    if (count) *count = 1;
    if (!buffer || *len < need) { *len = need; return ERROR_INSUFFICIENT_BUFFER; }
    fill_info(buffer);
    *len = need;
    return ERROR_SUCCESS;
}

static LONG WINAPI my_GetCurrentPackageInfo2(UINT32 flags, PackagePathType type, UINT32 *len, BYTE *buffer, UINT32 *count)
{
    return my_GetCurrentPackageInfo(flags, len, buffer, count);
}

static LONG WINAPI my_GetCurrentPackageInfo3(UINT32 flags, UINT32 infotype, UINT32 *len, void *buffer, UINT32 *count)
{
    FIXME("type %u\n", infotype);
    return my_GetCurrentPackageInfo(flags, len, buffer, count);
}

/* PACKAGE_INFO_REFERENCE based API */
static LONG WINAPI my_OpenPackageInfoByFullName(const WCHAR *full, UINT32 reserved, void **ref)
{
    TRACE("%s\n", debugstr_w(full));
    if (!is_our_package(full)) return ERROR_NOT_FOUND;
    *ref = (void *)0x1234;
    return ERROR_SUCCESS;
}

static LONG WINAPI my_ClosePackageInfo(void *ref)
{
    return ERROR_SUCCESS;
}

static LONG WINAPI my_GetPackageInfo(void *ref, UINT32 flags, UINT32 *len, BYTE *buffer, UINT32 *count)
{
    return my_GetCurrentPackageInfo(flags, len, buffer, count);
}

static LONG WINAPI my_GetPackageApplicationIds(void *ref, UINT32 *len, BYTE *buffer, UINT32 *count)
{
    static const WCHAR *ids[] = { PKG_FAMILY L"!App" };
    UINT32 need = sizeof(WCHAR *) + (wcslen(ids[0]) + 1) * sizeof(WCHAR);
    if (count) *count = 1;
    if (!buffer || *len < need) { *len = need; return ERROR_INSUFFICIENT_BUFFER; }
    *(WCHAR **)buffer = (WCHAR *)(buffer + sizeof(WCHAR *));
    wcscpy((WCHAR *)(buffer + sizeof(WCHAR *)), ids[0]);
    *len = need;
    return ERROR_SUCCESS;
}

/* name helpers (some missing from Wine) */
static LONG WINAPI my_PackageFamilyNameFromFullName(const WCHAR *full, UINT32 *len, WCHAR *family)
{
    const WCHAR *name_end, *pub;
    WCHAR buf[256];
    if (!full) return ERROR_INVALID_PARAMETER;
    /* name_version_arch_resource_publisherid */
    name_end = wcschr(full, '_');
    pub = wcsrchr(full, '_');
    if (!name_end || !pub || name_end - full > 100) return ERROR_INVALID_PARAMETER;
    memcpy(buf, full, (name_end - full) * sizeof(WCHAR));
    buf[name_end - full] = 0;
    wcscat(buf, pub);
    return copy_str(buf, len, family);
}

static LONG WINAPI my_PackageFamilyNameFromId(const PACKAGE_ID *id, UINT32 *len, WCHAR *family)
{
    WCHAR buf[256];
    if (!id || !id->name) return ERROR_INVALID_PARAMETER;
    wcscpy(buf, id->name);
    wcscat(buf, L"_");
    wcscat(buf, id->publisherId ? id->publisherId : PKG_PUBID);
    return copy_str(buf, len, family);
}

static LONG WINAPI my_VerifyPackageFamilyName(const WCHAR *name)
{
    return name && wcschr(name, '_') ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
}

static LONG WINAPI my_VerifyPackageFullName(const WCHAR *name)
{
    return name && wcschr(name, '_') ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
}

static LONG WINAPI my_VerifyApplicationUserModelId(const WCHAR *id)
{
    return id && wcschr(id, '!') ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
}

static LONG WINAPI my_FindPackagesByPackageFamily(const WCHAR *family, UINT32 filter, UINT32 *count,
                                                  WCHAR **names, UINT32 *buflen, WCHAR *buffer, UINT32 *props)
{
    UINT32 need = wcslen(PKG_FULLNAME) + 1;
    TRACE("%s\n", debugstr_w(family));
    if (!family || wcsicmp(family, PKG_FAMILY)) { *count = 0; *buflen = 0; return ERROR_SUCCESS; }
    if (!names || !buffer || *count < 1 || *buflen < need)
    {
        *count = 1; *buflen = need;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    wcscpy(buffer, PKG_FULLNAME);
    names[0] = buffer;
    if (props) props[0] = 0;
    *count = 1; *buflen = need;
    return ERROR_SUCCESS;
}

static LONG WINAPI my_GetPackagesByPackageFamily(const WCHAR *family, UINT32 *count, WCHAR **names,
                                                 UINT32 *buflen, WCHAR *buffer)
{
    return my_FindPackagesByPackageFamily(family, 0, count, names, buflen, buffer, NULL);
}

static LONG WINAPI my_GetEffectivePackageStatusForUserSid(void *sid, const WCHAR *full, UINT32 *status)
{
    if (status) *status = 0; /* OK */
    return ERROR_SUCCESS;
}

static HRESULT WINAPI my_GetSystemMetadataPathForPackage(const WCHAR *full, WCHAR *path, UINT32 *len)
{
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}


void identity_init(void)
{
    WCHAR exe[MAX_PATH], *p;
    GetModuleFileNameW(NULL, exe, ARRAY_SIZE(exe));
    wcscpy(pkg_path, exe);
    if ((p = wcsrchr(pkg_path, '\\'))) *p = 0;
    p = wcsrchr(exe, '\\'); p = p ? p + 1 : exe;
    if (!wcsicmp(p, L"AMPLibraryAgent.exe")) wcscpy(app_id, L"LibraryServer");
    else if (!wcsicmp(p, L"SharedHelper.exe")) wcscpy(app_id, L"SharedHelper");
    else if (!wcsicmp(p, L"appdefaults.exe")) wcscpy(app_id, L"AppleInc.Music.Defaults");
    TRACE("package path %s app %s\n", debugstr_w(pkg_path), debugstr_w(app_id));
}

const struct hook identity_hooks[] =
{
    HOOK(GetCurrentPackageFullName), HOOK(GetCurrentPackageFamilyName), HOOK(GetCurrentPackagePath),
    HOOK(GetCurrentPackagePath2), HOOK(GetCurrentApplicationUserModelId), HOOK(GetPackageFullName),
    HOOK(GetPackageFamilyName), HOOK(GetApplicationUserModelId), HOOK(GetPackagePathByFullName),
    HOOK(GetPackagePathByFullName2), HOOK(GetStagedPackagePathByFullName), HOOK(GetCurrentPackageId),
    HOOK(GetPackageId), HOOK(GetCurrentPackageInfo), HOOK(GetCurrentPackageInfo2), HOOK(GetCurrentPackageInfo3),
    HOOK(OpenPackageInfoByFullName), HOOK(ClosePackageInfo), HOOK(GetPackageInfo), HOOK(GetPackageApplicationIds),
    HOOK(PackageFamilyNameFromFullName), HOOK(PackageFamilyNameFromId), HOOK(VerifyPackageFamilyName),
    HOOK(VerifyPackageFullName), HOOK(VerifyApplicationUserModelId), HOOK(FindPackagesByPackageFamily),
    HOOK(GetPackagesByPackageFamily), HOOK(GetEffectivePackageStatusForUserSid),
    HOOK(GetSystemMetadataPathForPackage),
    { NULL }
};
