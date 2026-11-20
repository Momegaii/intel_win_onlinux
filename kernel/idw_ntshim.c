// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include "../compat/ntshim-design.h"

void *idw_ExAllocatePool(size_t size, gfp_t flags)
{
    if (!size) return NULL;
    return kmalloc(size, flags);
}

void idw_ExFreePool(void *ptr)
{
    kfree(ptr);
}

void idw_KeInitializeSpinLock(IDW_KSPIN_LOCK *lock)
{
    spin_lock_init(lock);
}

/* These two calls are valid only when the caller already runs in a context
 * equivalent to DISPATCH_LEVEL and owns the lock protocol. They do not
 * emulate Windows IRQL or save/restore interrupt state.
 */
void idw_KeAcquireSpinLockAtDpcLevel(IDW_KSPIN_LOCK *lock)
{
    spin_lock(lock);
}

void idw_KeReleaseSpinLockFromDpcLevel(IDW_KSPIN_LOCK *lock)
{
    spin_unlock(lock);
}

void idw_RtlInitUnicodeString(IDW_UNICODE_STRING *dst, const __u16 *src)
{
    size_t units = 0;
    if (!dst) return;
    dst->Buffer = src;
    dst->Length = 0;
    dst->MaximumLength = 0;
    if (!src) return;
    while (units < (U16_MAX / sizeof(__u16)) && src[units])
        units++;
    dst->Length = (__u16)(units * sizeof(__u16));
    dst->MaximumLength = dst->Length;
}
