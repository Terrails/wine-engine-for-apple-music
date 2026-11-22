/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Endpoints of the local control API (protocol: server.h). */
#pragma once
#include "server.h"

/* the request handler passed to server_start() */
void api_handle(struct sreq *r);
/* set by POST /quit */
extern int api_quit_requested;
