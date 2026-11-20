/* Compile-time contract sanity test. No runtime implementation implied. */
#include "../../include/idw/contracts/nt_runtime.h"
#include "../../include/idw/contracts/vidmem.h"
#include "../../include/idw/contracts/gpuvm.h"
#include "../../include/idw/contracts/haswell_ppgtt.h"
#include "../../include/idw/contracts/i915_assisted.h"
#include "../../include/idw/contracts/native_dma.h"
#include "../../include/idw/contracts/scheduler.h"
#include "../../include/idw/contracts/hw_backend.h"
#include "../../include/idw/contracts/umd_protocol.h"
#include "../../include/idw/contracts/dxgk_dispatch.h"

_Static_assert(sizeof(struct idw_uuid) == 16, "UUID ABI drift");
_Static_assert(IDW_UMD_PROTOCOL_MAJOR == 1u, "UMD protocol major drift");
_Static_assert(IDW_CONTRACT_VERSION_MAJOR == 1u, "contract major drift");
_Static_assert(IDW_HASWELL_PPGTT_PAGE_SIZE == 4096u, "Haswell PPGTT page-size drift");
_Static_assert(IDW_HASWELL_PPGTT_PTES_PER_TABLE == 1024u, "Gen7 PTE geometry drift");
_Static_assert(IDW_HASWELL_PPGTT_GEN7_ADDRESS_SPACE_SIZE == (1ULL << 31),
               "Gen7 address-space baseline drift");
_Static_assert(IDW_PT_PAGE_BYTES == 4096u, "DMA page size drift");
_Static_assert(IDW_PT_MEMORY_VERSION == 1u, "page-memory contract version drift");

int main(void)
{
    struct idw_submit_desc submit = { .size = sizeof(submit) };
    struct idw_vm_map_desc map = { .size = sizeof(map) };
    struct idw_gen75_ppgtt_caps ppgtt = { .size = sizeof(ppgtt) };
    struct idw_umd_message message = { .size = sizeof(message) };
    return (int)(submit.size == 0 || map.size == 0 || ppgtt.size == 0 ||
                 message.size == 0);
}
