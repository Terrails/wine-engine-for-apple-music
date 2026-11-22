/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Playback objects of the app: the Player and the playback settings. */
#pragma once
#include "common.h"

/* IPlayer (Player.Instance), NULL until the app has one. Slots: 6 NowPlayingItem, 7 PlayerState,
 * 12 Play, 13 Pause, 14 Stop, 16 SkipPrevious, 17 SkipNext, 22 SetPositionAsync, 27 SetVolume */
void *get_player(void);
/* IPlaybackSettings (ServiceProvider.AppSettings.PlaybackSettings), or NULL */
void *get_playback_settings(void);

/* streaming quality setting: "aac", "lossless", "hires" (or "unknown") */
const char *quality_get(void);
/* set it from one of those names; returns E_INVALIDARG for anything else */
HRESULT quality_set(const char *name);

/* PlayerState / NowPlayingItem, read in one go */
struct player_status
{
    int ready, playing, paused;
    double volume;
    int has_item;                  /* a real item (not a placeholder) with a title */
    HSTRING title, artist, album, sample_rate, bit_depth;
    HSTRING song_id;               /* catalogue ID (ContentDescriptor.AdamId), "" if none */
    HSTRING genre, release_date;   /* release date ISO 8601 */
    const char *format;            /* what is decoded now: other (= AAC), lossless, hires, atmos, spatial, surround */
    INT64 position_ms, duration_ms;
};
void player_status(struct player_status *st);
/* delete the strings player_status() returned */
void player_status_free(struct player_status *st);
