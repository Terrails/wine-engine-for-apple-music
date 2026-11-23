/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A small IVector<T> (with IVectorView, IIterable and IIterator) of 8-byte elements, for
 * arguments the app wants as a collection: UINT64 queue item IDs, or objects. */
#pragma once
#include "common.h"

/* element type: the pinterface IIDs differ per T */
enum { VEC_U64, VEC_INSPECTABLE };

/* new IVector<T> holding a copy of `items` (UINT64s, or objects that it AddRefs); the caller
 * owns the one reference. NULL on failure. At most VEC_MAX elements. */
#define VEC_MAX 1000     /* the next-added and next sections (page walks stop at 500 items each) */
void *vec_new(int type, const void *items, UINT32 n);
