/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Apple Account sign-in (see signin.h).
 *
 * SignInAsync(true) makes the app call our dialog service's ShowAuthDialogAsync with a
 * SignInHandler; signin_dialog() prompts and calls Invoke(NULL, user, pass, NULL). The sign-in
 * itself runs in this process (AuthKitWin -> CFNetwork); its interactive steps (2FA code,
 * alerts) arrive through the AuthKit UI provider (akui.c). */
#include "signin.h"
#include "prompt.h"

static const GUID IID_IAccountViewModel = {0x947f2669,0x9b1b,0x57a1,{0x97,0x4d,0x5e,0x0e,0x4b,0x19,0x2d,0xa6}};

INT32 signin_dialog(IUnknown *handler, HSTRING title, HSTRING message, HSTRING explanation, HSTRING user)
{
    static const WCHAR *const failed = L"Sign-in failed. Check the email/phone number and password and try again.";
    const WCHAR *error = NULL;
    WCHAR last_user[256];
    INT32 auth_result = 1;

    lstrcpynW(last_user, wz(user), ARRAY_SIZE(last_user));
    for (;;)
    {
        struct prompt p = {0};
        HSTRING huser, hpass;
        void *op = NULL;
        INT32 status, sign_in = 1;     /* SignInStatus: 0 success, 1 failure, 2 silent failure */
        HRESULT hr, err = S_OK;

        p.title = wz(title); p.message = wz(message); p.explanation = wz(explanation); p.error = error;
        p.n_edits = 2;
        p.edit_label[0] = L"Email or phone number"; p.edit_label[1] = L"Password"; p.edit_password[1] = 1;
        lstrcpynW(p.edit_text[0], last_user, ARRAY_SIZE(p.edit_text[0]));
        p.n_buttons = 2; p.buttons[0] = L"Sign In"; p.buttons[1] = L"Cancel"; p.default_button = 0;
        if (!error && GetEnvironmentVariableW(L"AM_TEST_USER", p.edit_text[0], ARRAY_SIZE(p.edit_text[0])))
        {
            /* pipeline test with a made-up account (first attempt only, no prompt) */
            GetEnvironmentVariableW(L"AM_TEST_PASS", p.edit_text[1], ARRAY_SIZE(p.edit_text[1]));
            LOG("sign-in: using the AM_TEST_USER test account");
        }
        else if (prompt_run(&p) != 0) { LOG("sign-in prompt: cancelled"); break; }
        else LOG("sign-in prompt: submitted");

        WindowsCreateString(p.edit_text[0], lstrlenW(p.edit_text[0]), &huser);
        WindowsCreateString(p.edit_text[1], lstrlenW(p.edit_text[1]), &hpass);
        lstrcpynW(last_user, p.edit_text[0], ARRAY_SIZE(last_user));      /* keep the username for a retry */
        SecureZeroMemory(p.edit_text, sizeof(p.edit_text));
        /* SignInHandler::Invoke(context, username, password, adsid) -> IAsyncOperation<SignInStatus> */
        hr = ((HRESULT (WINAPI *)(void *, void *, HSTRING, HSTRING, HSTRING, void **))VT(handler)[3])(handler, NULL, huser, hpass, NULL, &op);
        WindowsDeleteString(hpass); WindowsDeleteString(huser);
        LOG("SignInHandler.Invoke -> %08lx", hr);
        if (FAILED(hr) || !op) break;
        status = await_async(op, 200, 1500, &err);                        /* up to 5 minutes */
        if (status == 1) CALL1(op, 8, &sign_in);                          /* GetResults */
        LOG("sign-in: async status=%d error=%08lx SignInStatus=%d (0 success, 1 failure, 2 silent failure)", status, err, sign_in);
        if (status == 1 && sign_in == 0) { auth_result = 0; break; }
        if (status != 1 || sign_in == 2) break;
        error = failed;
    }
    SecureZeroMemory(last_user, sizeof(last_user));
    return auth_result;
}

/* IAccountViewModel (one per ServiceProvider: kept) */
static void *account_vm(void)
{
    static void *iacc;
    if (!iacc)
    {
        IUnknown *acc = NULL;
        CALL1(g_sp, 34, &acc);                /* ServiceProvider.AccountViewModel */
        if (acc)
        {
            IUnknown_QueryInterface(acc, &IID_IAccountViewModel, &iacc);
            IUnknown_Release(acc);
        }
    }
    return iacc;
}

int account_signed_in(void)
{
    void *iacc = account_vm(); boolean signed_in = 0;
    if (iacc) CALL1(iacc, 11, &signed_in);    /* SignedIn */
    return signed_in;
}

int signin_run(void)
{
    void *iacc = account_vm(), *act = NULL; boolean enrolled = 0; HRESULT hr, err; INT32 status;
    if (!iacc) { LOG("sign-in: no AccountViewModel"); return 1; }
    if (account_signed_in())
    {
        pump(5000);
        CALL1(iacc, 12, &enrolled);           /* IsEnrolledInMusic */
        LOG("already signed in; enrolled in Apple Music=%d", enrolled);
        return 0;
    }
    hr = ((HRESULT (WINAPI *)(void *, boolean, void **))VT(iacc)[16])(iacc, TRUE, &act);   /* SignInAsync(userInitiated) */
    say("AccountViewModel.SignInAsync(userInitiated)", hr);
    if (FAILED(hr)) return 1;
    status = await_async(act, 250, 2400, &err);                   /* up to 10 minutes */
    LOG("SignInAsync finished: status=%d SignedIn=%d", status, account_signed_in());
    if (!account_signed_in()) return 1;
    {
        HSTRING name = NULL;
        CALL1(iacc, 7, &name); CALL1(iacc, 12, &enrolled);
        LOG("account: name present=%d, enrolled in Apple Music=%d", WindowsGetStringLen(name) != 0, enrolled);
    }
    pump(15000);   /* let the app finish its post-sign-in work (cloud library, settings) */
    return 0;
}
