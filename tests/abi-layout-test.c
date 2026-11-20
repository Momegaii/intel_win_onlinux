// SPDX-License-Identifier: GPL-2.0
#include <stddef.h>
#include <stdint.h>
#include "../include/uapi/idw_ioctl.h"

_Static_assert(sizeof(struct idw_allocation_create) == 24,
               "idw_allocation_create ABI size changed");
_Static_assert(offsetof(struct idw_allocation_create, struct_size) == 0,
               "idw_allocation_create.struct_size ABI drift");
_Static_assert(offsetof(struct idw_allocation_create, size) == 8,
               "idw_allocation_create.size ABI drift");
_Static_assert(offsetof(struct idw_allocation_create, allocation_id) == 16,
               "idw_allocation_create.allocation_id ABI drift");
_Static_assert(offsetof(struct idw_allocation_create, reserved) == 20,
               "idw_allocation_create.reserved ABI drift");

_Static_assert(sizeof(struct idw_allocation_import_dmabuf) == 16,
               "idw_allocation_import_dmabuf ABI size changed");
_Static_assert(offsetof(struct idw_allocation_import_dmabuf, allocation_id) == 8,
               "idw_allocation_import_dmabuf.allocation_id ABI drift");
_Static_assert(offsetof(struct idw_allocation_import_dmabuf, flags) == 12,
               "idw_allocation_import_dmabuf.flags ABI drift");

_Static_assert(sizeof(struct idw_allocation_destroy) == 8,
               "idw_allocation_destroy ABI size changed");

_Static_assert(sizeof(struct idw_query) == 40,
               "idw_query ABI size changed");
_Static_assert(IDW_ABI_MINOR == 4,
               "IDW UAPI minor version was not updated");
_Static_assert(offsetof(struct idw_query, features) == 16,
               "idw_query.features ABI drift");
_Static_assert(offsetof(struct idw_query, reserved) == 24,
               "idw_query.reserved ABI drift");

int main(void)
{
    return 0;
}
