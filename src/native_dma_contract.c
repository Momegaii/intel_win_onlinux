/* SPDX-License-Identifier: MIT */
#include "../include/idw/contracts/native_dma.h"

#include <errno.h>

int idw_native_dma_contract_validate(
    const struct idw_native_dma_contract *contract)
{
    const uint64_t required_caps =
        IDW_PT_CAP_DMA_PAGE_TABLE |
        IDW_PT_CAP_DMA_RESOURCE_MAPPING |
        IDW_PT_CAP_SYNC_FOR_DEVICE |
        IDW_PT_CAP_GEN75_INVALIDATE |
        IDW_PT_CAP_GEN75_VM_ACTIVATE;
    int ret;

    if (!contract || contract->size != sizeof(*contract) ||
        contract->version != IDW_NATIVE_DMA_CONTRACT_VERSION ||
        contract->dma_address_bits != 40 || contract->flags)
        return -EINVAL;
    ret = idw_pt_backend_validate(&contract->backend, required_caps);
    if (ret)
        return ret;
    if (contract->backend.kind != IDW_PT_BACKEND_NATIVE_DMA ||
        !(contract->backend.flags & IDW_PT_BACKEND_EXCLUSIVE_DEVICE_OWNER))
        return -EPERM;
    if (contract->memory.size != sizeof(contract->memory) ||
        contract->memory.version != IDW_PT_MEMORY_VERSION ||
        !contract->memory.alloc_page_table_page ||
        !contract->memory.free_page_table_page ||
        !contract->memory.map_allocation ||
        !contract->memory.unmap_allocation ||
        !contract->memory.sync_page_table_for_device ||
        contract->ppgtt.size != sizeof(contract->ppgtt) ||
        contract->ppgtt.version != IDW_PT_MEMORY_VERSION ||
        !contract->ppgtt.query_caps ||
        !contract->ppgtt.invalidate_vm ||
        !contract->ppgtt.activate_vm ||
        !contract->ppgtt.deactivate_vm)
        return -EINVAL;
    return 0;
}
