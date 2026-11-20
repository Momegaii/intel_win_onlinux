/* SPDX-License-Identifier: MIT */
#include "../include/idw/gpu_submission.hpp"

#include <cerrno>

namespace idw {
namespace {

constexpr uint64_t kBatchObjectSize = 4096;
constexpr uint32_t kMaxBatchCount = 1000;

int first_error(int current, int candidate)
{
    return current ? current : candidate;
}

bool valid_ops(const GpuSubmissionOps& ops)
{
    return ops.context_create && ops.gem_create && ops.batch_upload &&
           ops.exec && ops.wait && ops.gem_close && ops.context_destroy;
}

} // namespace

int validate_haswell_noop_batch(const void *bytes, size_t length)
{
    if (!bytes || length != kHaswellNoopBatchLength)
        return -EINVAL;
    uint32_t command = 0;
    uint32_t alignment_noop = 0;
    const auto *raw = static_cast<const uint8_t *>(bytes);
    command = static_cast<uint32_t>(raw[0]) |
              (static_cast<uint32_t>(raw[1]) << 8) |
              (static_cast<uint32_t>(raw[2]) << 16) |
              (static_cast<uint32_t>(raw[3]) << 24);
    alignment_noop = static_cast<uint32_t>(raw[4]) |
                     (static_cast<uint32_t>(raw[5]) << 8) |
                     (static_cast<uint32_t>(raw[6]) << 16) |
                     (static_cast<uint32_t>(raw[7]) << 24);
    return command == kHaswellMiBatchBufferEnd &&
           alignment_noop == kHaswellMiNoop ? 0 : -EINVAL;
}

const char *gpu_submission_stage_name(GpuSubmissionStage stage)
{
    switch (stage) {
    case GpuSubmissionStage::None: return "none";
    case GpuSubmissionStage::Validate: return "batch validation";
    case GpuSubmissionStage::ContextCreate: return "context creation";
    case GpuSubmissionStage::GemCreate: return "batch GEM creation";
    case GpuSubmissionStage::BatchUpload: return "batch upload";
    case GpuSubmissionStage::Exec: return "execbuffer submission";
    case GpuSubmissionStage::GpuAddress: return "GPU address validation";
    case GpuSubmissionStage::Wait: return "completion wait";
    case GpuSubmissionStage::GemClose: return "GEM cleanup";
    case GpuSubmissionStage::ContextDestroy: return "context cleanup";
    }
    return "unknown";
}

int submit_haswell_noop_batches(const GpuSubmissionOps& ops, void *userdata,
                                const void *batch, size_t batch_length,
                                uint32_t count, int64_t timeout_ns,
                                GpuSubmissionResult& result)
{
    result = {};
    if (!valid_ops(ops) || count == 0 || count > kMaxBatchCount ||
        timeout_ns <= 0 || batch_length > UINT32_MAX) {
        result.failed_stage = GpuSubmissionStage::Validate;
        return -EINVAL;
    }
    int ret = validate_haswell_noop_batch(batch, batch_length);
    if (ret) {
        result.failed_stage = GpuSubmissionStage::Validate;
        return ret;
    }

    uint32_t context_id = 0;
    result.failed_stage = GpuSubmissionStage::ContextCreate;
    ret = ops.context_create(userdata, &context_id);
    if (ret) {
        if (context_id) {
            const int destroy_ret = ops.context_destroy(userdata, context_id);
            result.cleanup_error = first_error(result.cleanup_error, destroy_ret);
        }
        return ret;
    }
    if (!context_id) {
        result.failed_stage = GpuSubmissionStage::ContextCreate;
        return -EIO;
    }
    result.context_id = context_id;

    for (uint32_t i = 0; !ret && i < count; ++i) {
        uint32_t handle = 0;
        result.failed_stage = GpuSubmissionStage::GemCreate;
        ret = ops.gem_create(userdata, kBatchObjectSize, &handle);
        if (ret) {
            if (handle) {
                const int close_ret = ops.gem_close(userdata, handle);
                result.cleanup_error =
                    first_error(result.cleanup_error, close_ret);
            }
            break;
        }
        if (!handle) {
            ret = -EIO;
            break;
        }

        result.failed_stage = GpuSubmissionStage::BatchUpload;
        ret = ops.batch_upload(userdata, handle, batch,
                               static_cast<uint32_t>(batch_length));
        if (!ret) {
            uint64_t gpu_va = 0;
            result.failed_stage = GpuSubmissionStage::Exec;
            ret = ops.exec(userdata, context_id, HaswellEngine::Render,
                           handle, static_cast<uint32_t>(batch_length),
                           &gpu_va);
            if (!ret && (!gpu_va || (gpu_va & (kGpuPageSize - 1)))) {
                result.failed_stage = GpuSubmissionStage::GpuAddress;
                ret = -EIO;
            }
            if (!ret) {
                result.last_gpu_va = gpu_va;
                result.failed_stage = GpuSubmissionStage::Wait;
                ret = ops.wait(userdata, handle, timeout_ns);
                if (!ret)
                    ++result.completed_batches;
            }
        }

        const int close_ret = ops.gem_close(userdata, handle);
        result.cleanup_error = first_error(result.cleanup_error, close_ret);
        if (close_ret && !ret)
            result.failed_stage = GpuSubmissionStage::GemClose;
        ret = first_error(ret, close_ret);
    }

    if (!ret)
        result.failed_stage = GpuSubmissionStage::ContextDestroy;
    const int destroy_ret = ops.context_destroy(userdata, context_id);
    result.cleanup_error = first_error(result.cleanup_error, destroy_ret);
    if (destroy_ret && !ret)
        result.failed_stage = GpuSubmissionStage::ContextDestroy;
    ret = first_error(ret, destroy_ret);
    if (!ret)
        result.failed_stage = GpuSubmissionStage::None;
    return ret;
}

} // namespace idw
