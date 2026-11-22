/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Storage: item objects for a path (overview: amshim_storage.h). */
#include "amshim_storage.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

const WCHAR *path_of(struct obj *o)
{
    struct prop *p = prop_find(o, L"Path");
    return p ? WindowsGetStringRawBuffer(p->v.str, NULL) : L"";
}

void set_str(struct obj *o, const WCHAR *name, const WCHAR *val)
{
    HSTRING s;
    hstring_from_w(val, &s);
    prop_put(o, name, PK_STRING, &s, sizeof(s));
    WindowsDeleteString(s);
}

void set_item_props(struct obj *o, const WCHAR *path, BOOL dir)
{
    const WCHAR *name = wcsrchr(path, '\\');
    UINT32 attrs;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    INT64 created = 0;

    name = name ? name + 1 : path;
    set_str(o, L"Path", path);
    set_str(o, L"Name", name);
    set_str(o, L"DisplayName", name);
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
    {
        attrs = fad.dwFileAttributes & (dir ? ~0u : ~FILE_ATTR_DIRECTORY);
        created = ((INT64)fad.ftCreationTime.dwHighDateTime << 32) | fad.ftCreationTime.dwLowDateTime;
    }
    else attrs = dir ? FILE_ATTR_DIRECTORY : 0x80;
    prop_put(o, L"Attributes", PK_PRIM, &attrs, sizeof(attrs));
    prop_put(o, L"DateCreated", PK_PRIM, &created, sizeof(created));
    if (!dir)
    {
        const WCHAR *ext = wcsrchr(name, '.');
        set_str(o, L"FileType", ext ? ext : L"");
        set_str(o, L"ContentType", L"application/octet-stream");
    }
}

HRESULT storage_folder_create(const WCHAR *path, IInspectable **out)
{
    HRESULT hr = create_object_by_name(L"Windows.Storage.StorageFolder", out);
    if (SUCCEEDED(hr)) set_item_props(impl_from_inspectable(*out), path, TRUE);
    return hr;
}

HRESULT storage_file_create(const WCHAR *path, IInspectable **out)
{
    HRESULT hr = create_object_by_name(L"Windows.Storage.StorageFile", out);
    if (SUCCEEDED(hr)) set_item_props(impl_from_inspectable(*out), path, FALSE);
    return hr;
}

void ensure_dir(const WCHAR *path)
{
    SHCreateDirectoryExW(NULL, path, NULL);
}

HRESULT folder_for(const WCHAR *path, IInspectable **out)
{
    ensure_dir(path);
    return storage_folder_create(path, out);
}
