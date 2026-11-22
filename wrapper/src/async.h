/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Minimal IAsyncOperation<T>/IAsyncAction handed back to AMP.Services: either completed at
 * creation (zeroed result) or pending until async_complete(). */
#pragma once
#include "common.h"

struct aop
{
    const void **op_vtbl;      /* IAsyncOperation<T> / IAsyncAction (answers any pinterface IID) */
    const void **info_vtbl;    /* IAsyncInfo */
    LONG ref;
    int status;                /* AsyncStatus: 0 started, 1 completed */
    IUnknown *handler;         /* Completed handler */
    BYTE result[16];
    unsigned int rsize;        /* size of the result type (0 for an action) */
};

/* action: IAsyncAction (GetResults takes no argument) instead of IAsyncOperation<T> */
struct aop *aop_new(int action, int completed, unsigned int rsize);
/* store the result, mark completed and call the Completed handler */
void aop_complete(struct aop *a, const void *result, unsigned int size);
/* the object as returned to the app */
static inline void *aop_iface(struct aop *a) { return &a->op_vtbl; }
