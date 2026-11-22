/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Storage (ApplicationData, StorageFolder, StorageFile, FileIO) on plain Win32 file calls.
 *   amshim_storage.c       StorageFolder/StorageFile objects for a path, their properties
 *   ov_storage_appdata.c   ApplicationData (folders under %LOCALAPPDATA%\\Packages\\<family>)
 *   ov_storage_folder.c    StorageFolder
 *   ov_storage_fileio.c    FileIO statics (synchronous I/O behind completed async results)
 *   ov_storage_item.c      basic properties, rename / move / copy */
#pragma once
#include "gen/amshim_gen.h"
#include <shlobj.h>
#include <shellapi.h>
LONG WINAPI GetCurrentPackageFamilyName(UINT32 *, WCHAR *);

#define FILE_ATTR_DIRECTORY 0x10

const WCHAR *path_of(struct obj *o);
void set_str(struct obj *o, const WCHAR *name, const WCHAR *val);
void set_item_props(struct obj *o, const WCHAR *path, BOOL dir);
HRESULT storage_file_create(const WCHAR *path, IInspectable **out);
void ensure_dir(const WCHAR *path);
HRESULT folder_for(const WCHAR *path, IInspectable **out);
