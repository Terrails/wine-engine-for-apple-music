/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* amd.exe: the Apple Music wrapper service.
 *
 *   amd.exe serve [port]   start the app's engine and answer the local API (default port 7654)
 *   amd.exe signin         interactive sign-in (native prompts), then exit
 *
 * Exit codes: 0 normal, 1 start-up failed, 4 the agent is gone (the launcher restarts us).
 * Environment: AM_ACCEPT_WELCOME=1 accepts the first-run notice, AM_STOREFRONT is the default
 * country for album links, AM_TS=1 timestamps log lines. */
#include "common.h"
#include "api.h"
#include "app.h"
#include "oslog.h"
#include "medianame.h"
#include "player.h"
#include "queue.h"
#include "signin.h"

static void exit_agent_lost(const char *what)
{
    LOG("SERVE %s, exiting (the launcher restarts us)", what);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 4);
}

static int serve(int port)
{
    WCHAR token_path[MAX_PATH] = L""; void *stop = NULL;
    if (!app_wait_playback(30000)) exit_agent_lost("no working agent");
    queue_open();         /* the play queue is read when a song is played in its album */
    if (!server_start(port, api_handle, token_path, ARRAY_SIZE(token_path)))
    {
        LOG("SERVE cannot start the server on port %d", port);
        return 1;
    }
    media_name_init();
    LOG("SERVE ready on http://127.0.0.1:%d (token in %ls)", port, token_path);
    while (!api_quit_requested && !oslog_agent_lost) pump(250);
    if (oslog_agent_lost) exit_agent_lost("the agent is gone");
    if (get_player()) { CALL1(get_player(), 14, &stop); pump(1500); }   /* Stop */
    LOG("SERVE quitting");
    /* A normal exit runs the app DLLs' static destructors, and AppleMediaServicesKit's crash under
     * Wine (access violation after "Destroying request scheduler"). Nothing is left to save. */
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "serve";
    if (strcmp(mode, "serve") && strcmp(mode, "signin"))
    {
        fprintf(stderr, "usage: amd.exe serve [port] | amd.exe signin\n");
        return 2;
    }
    if (app_start(0)) return 1;
    if (!strcmp(mode, "signin"))
    {
        if (!app_wait_playback(30000)) return 4;
        return signin_run();
    }
    return serve(argc > 2 && atoi(argv[2]) > 0 ? atoi(argv[2]) : 7654);
}
