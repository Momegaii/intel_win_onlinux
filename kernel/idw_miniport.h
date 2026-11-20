/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IDW_MINIPORT_H
#define IDW_MINIPORT_H
#include <linux/types.h>

/* IDW-native test contract. Deliberately not binary-compatible with WDDM. */
struct idw_miniport_ops {
    int (*add_device)(void *device, void **adapter_context);
    int (*start_device)(void *adapter_context);
    int (*create_allocation)(void *adapter_context, u64 bytes, u64 *allocation_id);
    void (*stop_device)(void *adapter_context);
    void (*remove_device)(void *adapter_context);
};

struct idw_miniport {
    const char *name;
    const struct idw_miniport_ops *ops;
};

int idw_register_miniport(const struct idw_miniport *miniport);
void idw_unregister_miniport(const struct idw_miniport *miniport);
int idw_dispatch_add_device(void *device);
int idw_dispatch_start_device(void);
int idw_dispatch_create_allocation(u64 bytes, u64 *allocation_id);
void idw_dispatch_stop_device(void);
void idw_dispatch_remove_device(void);
bool idw_miniport_is_registered(void);
#endif
