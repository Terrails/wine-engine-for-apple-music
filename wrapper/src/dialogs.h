/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The UI side of the app's DialogService (IDialogServiceImpl), first-run notice and
 * NotificationCenter subscription. */
#pragma once
#include "common.h"

/* install our IDialogServiceImpl into ServiceProvider.DialogService */
HRESULT dialogs_install(void);
/* subscribe to NotificationCenter and PageNotificationCenter (requests are logged) */
void notifications_subscribe(void);
/* show queued dialog requests (sign-in, message boxes); called from the message loop */
void dialogs_process(void);

/* the first-run welcome/privacy notice, once the app has shown it */
int welcome_pending(void);
/* press the notice's accept button (explicit opt-in: AM_ACCEPT_WELCOME=1) */
HRESULT welcome_accept(void);
