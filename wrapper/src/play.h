/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Playing catalogue items. There is no "play store item by ID" (IPlayer's
 * StartOrQueueLibraryPlaybackAsync takes library items only), so do what a link does: open the
 * album page by deep link and run a play action from it. */
#pragma once
#include "common.h"

struct play_request
{
    const char *cc;            /* storefront country code, e.g. "us" */
    const char *album;         /* album ID (numeric) */
    const char *song;          /* song ID, "" for the whole album */
    int single;                /* queue only the song (else: the album, starting at the song) */
};

struct play_result
{
    HRESULT hr;
    int found;                 /* the item was on the album page */
    int page_error;            /* the page reported an error */
    DWORD ms;                  /* time until playback was requested */
    const char *how;           /* which way it was started (log/diagnostics) */
};

void play_catalog(const struct play_request *req, struct play_result *res);
