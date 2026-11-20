#ifndef IDW_NTSHIM_DESIGN_H
#define IDW_NTSHIM_DESIGN_H
/* IDW's narrow NT-inspired compatibility surface; not an NT ABI. */
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/slab.h>

typedef spinlock_t IDW_KSPIN_LOCK;
typedef struct {
    const __u16 *Buffer;
    __u16 Length;        /* bytes, matching UNICODE_STRING convention */
    __u16 MaximumLength; /* bytes */
} IDW_UNICODE_STRING;

/* Caller chooses GFP flags according to Linux execution context. */
void *idw_ExAllocatePool(size_t size, gfp_t flags);
void idw_ExFreePool(void *ptr);
void idw_KeInitializeSpinLock(IDW_KSPIN_LOCK *lock);
void idw_KeAcquireSpinLockAtDpcLevel(IDW_KSPIN_LOCK *lock);
void idw_KeReleaseSpinLockFromDpcLevel(IDW_KSPIN_LOCK *lock);
void idw_RtlInitUnicodeString(IDW_UNICODE_STRING *dst, const __u16 *src);
#endif
