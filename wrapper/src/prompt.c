/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Native prompt window (see prompt.h). */
#include "prompt.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define PROMPT_BTN_ID 100
#define PROMPT_AUTO_TIMER 7

static void prompt_finish(HWND hwnd, struct prompt *p, int button)
{
    for (int i = 0; i < p->n_edits; i++) GetWindowTextW(p->edits[i], p->edit_text[i], ARRAY_SIZE(p->edit_text[i]));
    p->result = button;
    p->done = 1;
}

static LRESULT CALLBACK prompt_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct prompt *p = (struct prompt *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_COMMAND:
        if (!p) break;
        if (LOWORD(wp) >= PROMPT_BTN_ID && LOWORD(wp) < PROMPT_BTN_ID + p->n_buttons) prompt_finish(hwnd, p, LOWORD(wp) - PROMPT_BTN_ID);
        else if (LOWORD(wp) == IDOK) prompt_finish(hwnd, p, p->default_button);          /* Enter */
        else if (LOWORD(wp) == IDCANCEL) prompt_finish(hwnd, p, p->n_buttons - 1);       /* Esc */
        return 0;
    case WM_CLOSE:
        if (p) prompt_finish(hwnd, p, p->n_buttons - 1);
        return 0;
    case WM_TIMER:
        /* test mode (AM_DIALOG_AUTO): press the last button without user input */
        if (p && wp == PROMPT_AUTO_TIMER) { KillTimer(hwnd, wp); prompt_finish(hwnd, p, p->n_buttons - 1); }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND prompt_child(HWND parent, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(!lstrcmpW(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

int prompt_run(struct prompt *p)
{
    static ATOM cls;
    const int W = 460, M = 16, CW = W - 2 * M;
    int y = M, bw = 110, x;
    RECT rc = { 0, 0, W, 0 };
    HWND hwnd;
    MSG msg;

    if (!cls)
    {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = prompt_wndproc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"AmWrapperPrompt";
        cls = RegisterClassW(&wc);
    }
    hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"AmWrapperPrompt", p->title && *p->title ? p->title : L"Apple Music",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, W, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) { p->result = p->n_buttons - 1; return p->result; }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)p);
    p->done = 0;

    if (p->message && *p->message) { prompt_child(hwnd, L"STATIC", p->message, 0, M, y, CW, 36, -1); y += 40; }
    if (p->explanation && *p->explanation) { prompt_child(hwnd, L"STATIC", p->explanation, 0, M, y, CW, 64, -1); y += 68; }
    if (p->error && *p->error) { prompt_child(hwnd, L"STATIC", p->error, 0, M, y, CW, 32, -1); y += 36; }
    for (int i = 0; i < p->n_edits; i++)
    {
        prompt_child(hwnd, L"STATIC", p->edit_label[i], 0, M, y, CW, 16, -1); y += 18;
        p->edits[i] = prompt_child(hwnd, L"EDIT", p->edit_text[i], WS_TABSTOP | ES_AUTOHSCROLL | (p->edit_password[i] ? ES_PASSWORD : 0),
                                   M, y, CW, 24, 10 + i);
        y += 32;
    }
    y += 8;
    x = W - M - p->n_buttons * (bw + 8) + 8;
    for (int i = 0; i < p->n_buttons; i++, x += bw + 8)
        prompt_child(hwnd, L"BUTTON", p->buttons[i], WS_TABSTOP | (i == p->default_button ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                     x, y, bw, 28, PROMPT_BTN_ID + i);
    y += 28 + M;

    rc.bottom = y;
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    SetWindowPos(hwnd, HWND_TOPMOST, (GetSystemMetrics(SM_CXSCREEN) - (rc.right - rc.left)) / 2,
                 (GetSystemMetrics(SM_CYSCREEN) - (rc.bottom - rc.top)) / 3, rc.right - rc.left, rc.bottom - rc.top, SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    if (p->n_edits) SetFocus(p->edits[p->edit_text[0][0] && p->n_edits > 1 ? 1 : 0]);
    if (GetEnvironmentVariableA("AM_DIALOG_AUTO", NULL, 0)) SetTimer(hwnd, PROMPT_AUTO_TIMER, 1500, NULL);

    while (!p->done && GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyWindow(hwnd);
    return p->result;
}
