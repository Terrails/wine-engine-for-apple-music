/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The now-playing name for the mixer: "<artist> - <title>" in %LOCALAPPDATA%\AppleMusicWrapper\media-name
 * (UTF-8, empty when nothing is loaded). pulse/ampulse.c, preloaded into the Wine session by bin/amd,
 * sets the agent's PulseAudio stream name (media.name) from it, so mixers show
 * "Apple Music · <artist> - <title>" (Plasma) / "Apple Music: <artist> - <title>" (pavucontrol). */
#include "medianame.h"
#include <shlobj.h>
#include "player.h"

static WCHAR mn_path[MAX_PATH], mn_tmp[MAX_PATH];
static char mn_last[1024];
static DWORD mn_next;

static void mn_write(const char *name)
{
    HANDLE f; DWORD w;
    if (!strcmp(name, mn_last)) return;
    f = CreateFileW(mn_tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, name, strlen(name), &w, NULL);
    CloseHandle(f);
    /* replaced in one go: the reader never sees a half-written name */
    if (!MoveFileExW(mn_tmp, mn_path, MOVEFILE_REPLACE_EXISTING)) return;
    lstrcpynA(mn_last, name, sizeof(mn_last));
}

void media_name_init(void)
{
    WCHAR dir[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, dir))) return;
    lstrcatW(dir, L"\\AppleMusicWrapper");
    CreateDirectoryW(dir, NULL);
    swprintf(mn_path, ARRAY_SIZE(mn_path), L"%s\\media-name", dir);
    swprintf(mn_tmp, ARRAY_SIZE(mn_tmp), L"%s\\media-name.tmp", dir);
    mn_last[0] = 1;                                   /* differs from anything: write once now */
    mn_write("");
}

void media_name_tick(void)
{
    static int busy;
    struct player_status st;
    WCHAR wname[600];
    char name[1024] = "";

    if (!mn_path[0] || busy || (int)(GetTickCount() - mn_next) < 0) return;
    busy = 1;
    mn_next = GetTickCount() + 500;
    player_status(&st);
    if (st.has_item)
    {
        if (WindowsGetStringLen(st.artist)) swprintf(wname, ARRAY_SIZE(wname), L"%s - %s", wz(st.artist), wz(st.title));
        else lstrcpynW(wname, wz(st.title), ARRAY_SIZE(wname));
        for (WCHAR *p = wname; *p; p++) if (*p < 0x20) *p = ' ';
        if (!WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof(name), NULL, NULL)) name[0] = 0;
    }
    player_status_free(&st);
    mn_write(name);
    busy = 0;
}
