/* SPDX-License-Identifier: MIT */
#ifndef IDW_GPU_SUBMISSION_HPP
#define IDW_GPU_SUBMISSION_HPP

#include <cstddef>
#include <cstdint>

namespace idw {

constexpr uint32_t kHaswellMiBatchBufferEnd = 0x05000000u;
constexpr uint32_t kHaswellMiNoop = 0x00000000u;
constexpr size_t kHaswellNoopBatchLength = 2 * sizeof(uint32_t);
constexpr uint64_t kGpuPageSize = 4096;
constexpr int64_t kDefaultGpuWaitTimeoutNs = 5LL * 1000 * 1000 * 1000;

enum class HaswellEngine {
    Render
};

enum class GpuSubmissionStage {
    None,
    Validate,
    ContextCreate,
    GemCreate,
    BatchUpload,
    Exec,
    GpuAddress,
    Wait,
    GemClose,
    ContextDestroy
};

struct GpuSubmissionOps {
    /* Callbacks return zero or a negative errno value. */
    int (*context_create)(void *userdata, uint32_t *context_id);
    int (*gem_create)(void *userdata, uint64_t size, uint32_t *handle);
    int (*batch_upload)(void *userdata, uint32_t handle,
                        const void *bytes, uint32_t length);
    int (*exec)(void *userdata, uint32_t context_id, HaswellEngine engine,
                uint32_t handle, uint32_t batch_length,
                uint64_t *returned_gpu_va);
    int (*wait)(void *userdata, uint32_t handle, int64_t timeout_ns);
    int (*gem_close)(void *userdata, uint32_t handle);
    int (*context_destroy)(void *userdata, uint32_t context_id);
};

struct GpuSubmissionResult {
    uint32_t context_id;
    uint32_t completed_batches;
    uint64_t last_gpu_va;
    int cleanup_error;
    GpuSubmissionStage failed_stage;
};

const char *gpu_submission_stage_name(GpuSubmissionStage stage);
int validate_haswell_noop_batch(const void *bytes, size_t length);

int submit_haswell_noop_batches(const GpuSubmissionOps& ops, void *userdata,
                                const void *batch, size_t batch_length,
                                uint32_t count, int64_t timeout_ns,
                                GpuSubmissionResult& result);

} // namespace idw

#endif
