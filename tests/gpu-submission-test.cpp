/* Submission callbacks are simulated; this test does not execute GPU commands. */
#include "../include/idw/gpu_submission.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace {

struct Fake {
    int create_context_result = 0;
    int create_gem_result = 0;
    int upload_result = 0;
    int exec_result = 0;
    int wait_result = 0;
    int close_result = 0;
    int destroy_result = 0;
    bool return_context_on_create_failure = false;
    bool return_zero_context = false;
    uint32_t context_creates = 0;
    uint32_t gem_creates = 0;
    uint32_t uploads = 0;
    uint32_t execs = 0;
    uint32_t waits = 0;
    uint32_t closes = 0;
    uint32_t context_destroys = 0;
    uint64_t gpu_va = 0x1000;
    int64_t observed_timeout = 0;
    uint32_t observed_length = 0;
    idw::HaswellEngine observed_engine = idw::HaswellEngine::Render;
};

int create_context(void *data, uint32_t *context_id)
{
    auto& fake = *static_cast<Fake *>(data);
    ++fake.context_creates;
    *context_id = !fake.return_zero_context &&
                  (fake.return_context_on_create_failure ||
                   !fake.create_context_result) ? 7 : 0;
    return fake.create_context_result;
}

int create_gem(void *data, uint64_t size, uint32_t *handle)
{
    auto& fake = *static_cast<Fake *>(data);
    ++fake.gem_creates;
    if (size != 4096)
        return -EINVAL;
    *handle = 11;
    return fake.create_gem_result;
}

int upload(void *data, uint32_t handle, const void *bytes, uint32_t length)
{
    auto& fake = *static_cast<Fake *>(data);
    ++fake.uploads;
    if (handle != 11 || idw::validate_haswell_noop_batch(bytes, length))
        return -EINVAL;
    fake.observed_length = length;
    return fake.upload_result;
}

int exec(void *data, uint32_t context_id, idw::HaswellEngine engine,
         uint32_t handle, uint32_t length, uint64_t *gpu_va)
{
    auto& fake = *static_cast<Fake *>(data);
    ++fake.execs;
    fake.observed_engine = engine;
    if (context_id != 7 || handle != 11 ||
        length != idw::kHaswellNoopBatchLength)
        return -EINVAL;
    *gpu_va = fake.gpu_va;
    return fake.exec_result;
}

int wait(void *data, uint32_t handle, int64_t timeout_ns)
{
    auto& fake = *static_cast<Fake *>(data);
    ++fake.waits;
    if (handle != 11)
        return -EINVAL;
    fake.observed_timeout = timeout_ns;
    return fake.wait_result;
}

int close_gem(void *data, uint32_t handle)
{
    auto& fake = *static_cast<Fake *>(data);
    ++fake.closes;
    return handle == 11 ? fake.close_result : -EINVAL;
}

int destroy_context(void *data, uint32_t context_id)
{
    auto& fake = *static_cast<Fake *>(data);
    ++fake.context_destroys;
    return context_id == 7 ? fake.destroy_result : -EINVAL;
}

const idw::GpuSubmissionOps ops = {
    create_context, create_gem, upload, exec, wait, close_gem, destroy_context
};

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int test_validates_batch_and_arguments()
{
    const uint32_t valid[2] = {
        idw::kHaswellMiBatchBufferEnd, idw::kHaswellMiNoop
    };
    const uint32_t invalid = 0;
    CHECK(idw::validate_haswell_noop_batch(valid, sizeof(valid)) == 0);
    CHECK(idw::validate_haswell_noop_batch(nullptr, sizeof(valid)) == -EINVAL);
    CHECK(idw::validate_haswell_noop_batch(valid, sizeof(uint32_t)) == -EINVAL);
    CHECK(idw::validate_haswell_noop_batch(valid, sizeof(valid) + 4) == -EINVAL);
    CHECK(idw::validate_haswell_noop_batch(&invalid, sizeof(invalid)) == -EINVAL);

    Fake fake;
    idw::GpuSubmissionResult result{};
    CHECK(idw::submit_haswell_noop_batches(
              ops, &fake, valid, sizeof(uint32_t), 1,
              idw::kDefaultGpuWaitTimeoutNs, result) == -EINVAL);
    CHECK(fake.context_creates == 0 && fake.gem_creates == 0);
    CHECK(idw::submit_haswell_noop_batches(
              ops, &fake, valid, sizeof(valid), 0,
              idw::kDefaultGpuWaitTimeoutNs, result) == -EINVAL);
    CHECK(idw::submit_haswell_noop_batches(
              ops, &fake, valid, sizeof(valid), 1, 0, result) == -EINVAL);
    CHECK(result.failed_stage == idw::GpuSubmissionStage::Validate);
    CHECK(idw::submit_haswell_noop_batches(
              ops, &fake, valid, sizeof(valid), 1001,
              idw::kDefaultGpuWaitTimeoutNs, result) == -EINVAL);
    return 0;
}

int test_haswell_store_dword_encoding_and_bounds()
{
    constexpr uint64_t address = 0x10000014;
    constexpr uint32_t value = 0xdeadbeef;
    uint32_t batch[6]{};
    uint64_t decoded_address = 0;
    uint32_t decoded_value = 0;

    CHECK(idw::build_haswell_store_dword_batch(
              address, value, batch, sizeof(batch)) == 0);
    CHECK(batch[0] == 0x10000002u);
    CHECK(batch[1] == 0);
    CHECK(batch[2] == address);
    CHECK(batch[3] == value);
    CHECK(batch[4] == idw::kHaswellMiBatchBufferEnd);
    CHECK(batch[5] == idw::kHaswellMiNoop);
    CHECK(idw::validate_haswell_store_dword_batch(
              batch, sizeof(batch), &decoded_address, &decoded_value) == 0);
    CHECK(decoded_address == address && decoded_value == value);
    CHECK(idw::build_haswell_store_dword_batch(
              0xfffffffcu, value, batch, sizeof(batch)) == 0);
    CHECK(idw::build_haswell_store_dword_batch(
              address + 1, value, batch, sizeof(batch)) == -EINVAL);
    CHECK(idw::build_haswell_store_dword_batch(
              0, value, batch, sizeof(batch)) == -EINVAL);
    CHECK(idw::build_haswell_store_dword_batch(
              0x100000000ULL, value, batch, sizeof(batch)) == -EINVAL);
    CHECK(idw::build_haswell_store_dword_batch(
              address, value, batch, sizeof(batch) - sizeof(uint32_t)) ==
          -EINVAL);
    CHECK(idw::build_haswell_store_dword_batch(
              address, value, nullptr, sizeof(batch)) == -EINVAL);

    const uint32_t valid_batch[6] = {
        idw::kHaswellMiStoreDataImmDword, 0,
        static_cast<uint32_t>(address), value,
        idw::kHaswellMiBatchBufferEnd, idw::kHaswellMiNoop
    };
    CHECK(idw::validate_haswell_store_dword_batch(
              nullptr, sizeof(valid_batch), nullptr, nullptr) == -EINVAL);
    CHECK(idw::validate_haswell_store_dword_batch(
              valid_batch, sizeof(valid_batch) - sizeof(uint32_t),
              nullptr, nullptr) == -EINVAL);
    uint32_t malformed[6]{};
    std::memcpy(malformed, valid_batch, sizeof(malformed));
    malformed[0] |= 1u;
    CHECK(idw::validate_haswell_store_dword_batch(
              malformed, sizeof(malformed), nullptr, nullptr) == -EINVAL);
    std::memcpy(malformed, valid_batch, sizeof(malformed));
    malformed[1] = 1;
    CHECK(idw::validate_haswell_store_dword_batch(
              malformed, sizeof(malformed), nullptr, nullptr) == -EINVAL);
    std::memcpy(malformed, valid_batch, sizeof(malformed));
    malformed[2] |= 1u;
    CHECK(idw::validate_haswell_store_dword_batch(
              malformed, sizeof(malformed), nullptr, nullptr) == -EINVAL);
    std::memcpy(malformed, valid_batch, sizeof(malformed));
    malformed[2] = 0;
    CHECK(idw::validate_haswell_store_dword_batch(
              malformed, sizeof(malformed), nullptr, nullptr) == -EINVAL);
    std::memcpy(malformed, valid_batch, sizeof(malformed));
    malformed[4] = idw::kHaswellMiNoop;
    CHECK(idw::validate_haswell_store_dword_batch(
              malformed, sizeof(malformed), nullptr, nullptr) == -EINVAL);
    std::memcpy(malformed, valid_batch, sizeof(malformed));
    malformed[5] = idw::kHaswellMiBatchBufferEnd;
    CHECK(idw::validate_haswell_store_dword_batch(
              malformed, sizeof(malformed), nullptr, nullptr) == -EINVAL);
    return 0;
}

int test_successful_completion_and_cleanup()
{
    Fake fake;
    const uint32_t batch[2] = {
        idw::kHaswellMiBatchBufferEnd, idw::kHaswellMiNoop
    };
    idw::GpuSubmissionResult result{};
    CHECK(idw::submit_haswell_noop_batches(
              ops, &fake, &batch, sizeof(batch), 3,
              idw::kDefaultGpuWaitTimeoutNs, result) == 0);
    CHECK(result.context_id == 7 && result.completed_batches == 3);
    CHECK(result.last_gpu_va == fake.gpu_va && result.cleanup_error == 0);
    CHECK(fake.context_creates == 1 && fake.context_destroys == 1);
    CHECK(fake.gem_creates == 3 && fake.uploads == 3);
    CHECK(fake.execs == 3 && fake.waits == 3 && fake.closes == 3);
    CHECK(fake.observed_length == idw::kHaswellNoopBatchLength);
    CHECK(fake.observed_timeout == idw::kDefaultGpuWaitTimeoutNs);
    CHECK(fake.observed_engine == idw::HaswellEngine::Render);
    return 0;
}

int test_failed_submission_and_timeout_cleanup()
{
    const uint32_t batch[2] = {
        idw::kHaswellMiBatchBufferEnd, idw::kHaswellMiNoop
    };
    idw::GpuSubmissionResult result{};
    Fake submit_failure;
    submit_failure.exec_result = -EIO;
    CHECK(idw::submit_haswell_noop_batches(
              ops, &submit_failure, &batch, sizeof(batch), 1,
              idw::kDefaultGpuWaitTimeoutNs, result) == -EIO);
    CHECK(result.failed_stage == idw::GpuSubmissionStage::Exec);
    CHECK(result.completed_batches == 0);
    CHECK(submit_failure.execs == 1 && submit_failure.waits == 0);
    CHECK(submit_failure.closes == 1 && submit_failure.context_destroys == 1);

    Fake timeout;
    timeout.wait_result = -ETIME;
    CHECK(idw::submit_haswell_noop_batches(
              ops, &timeout, &batch, sizeof(batch), 1,
              idw::kDefaultGpuWaitTimeoutNs, result) == -ETIME);
    CHECK(result.failed_stage == idw::GpuSubmissionStage::Wait);
    CHECK(timeout.execs == 1 && timeout.waits == 1);
    CHECK(timeout.closes == 1 && timeout.context_destroys == 1);
    CHECK(result.completed_batches == 0);

    Fake upload_failure;
    upload_failure.upload_result = -EFAULT;
    CHECK(idw::submit_haswell_noop_batches(
              ops, &upload_failure, batch, sizeof(batch), 1,
              idw::kDefaultGpuWaitTimeoutNs, result) == -EFAULT);
    CHECK(result.failed_stage == idw::GpuSubmissionStage::BatchUpload);
    CHECK(upload_failure.execs == 0 && upload_failure.closes == 1);
    CHECK(upload_failure.context_destroys == 1);
    return 0;
}

int test_all_cleanup_is_attempted()
{
    const uint32_t batch[2] = {
        idw::kHaswellMiBatchBufferEnd, idw::kHaswellMiNoop
    };
    Fake fake;
    fake.exec_result = -EIO;
    fake.close_result = -EBUSY;
    fake.destroy_result = -EIO;
    idw::GpuSubmissionResult result{};
    CHECK(idw::submit_haswell_noop_batches(
              ops, &fake, batch, sizeof(batch), 1,
              idw::kDefaultGpuWaitTimeoutNs, result) == -EIO);
    CHECK(result.cleanup_error == -EBUSY);
    CHECK(fake.closes == 1 && fake.context_destroys == 1);

    Fake partial_gem_create;
    partial_gem_create.create_gem_result = -ENOMEM;
    CHECK(idw::submit_haswell_noop_batches(
              ops, &partial_gem_create, batch, sizeof(batch), 1,
              idw::kDefaultGpuWaitTimeoutNs, result) == -ENOMEM);
    CHECK(result.failed_stage == idw::GpuSubmissionStage::GemCreate);
    CHECK(partial_gem_create.closes == 1 &&
          partial_gem_create.context_destroys == 1);

    Fake partial_context_create;
    partial_context_create.create_context_result = -EIO;
    partial_context_create.return_context_on_create_failure = true;
    CHECK(idw::submit_haswell_noop_batches(
              ops, &partial_context_create, batch, sizeof(batch), 1,
              idw::kDefaultGpuWaitTimeoutNs, result) == -EIO);
    CHECK(result.failed_stage == idw::GpuSubmissionStage::ContextCreate);
    CHECK(partial_context_create.context_destroys == 1);

    Fake invalid_context;
    invalid_context.return_zero_context = true;
    CHECK(idw::submit_haswell_noop_batches(
              ops, &invalid_context, batch, sizeof(batch), 1,
              idw::kDefaultGpuWaitTimeoutNs, result) == -EIO);
    CHECK(result.failed_stage == idw::GpuSubmissionStage::ContextCreate);
    CHECK(invalid_context.gem_creates == 0 &&
          invalid_context.context_destroys == 0);

    Fake bad_address;
    bad_address.gpu_va = 0x1001;
    CHECK(idw::submit_haswell_noop_batches(
              ops, &bad_address, batch, sizeof(batch), 1,
              idw::kDefaultGpuWaitTimeoutNs, result) == -EIO);
    CHECK(result.failed_stage == idw::GpuSubmissionStage::GpuAddress);
    CHECK(bad_address.waits == 0);
    CHECK(bad_address.closes == 1 && bad_address.context_destroys == 1);
    return 0;
}

} // namespace

int main()
{
    if (test_validates_batch_and_arguments() ||
        test_haswell_store_dword_encoding_and_bounds() ||
        test_successful_completion_and_cleanup() ||
        test_failed_submission_and_timeout_cleanup() ||
        test_all_cleanup_is_attempted())
        return 1;
    std::puts("PASS: Haswell no-op and store-dword encoding, bounds, failure, and cleanup tests");
    std::puts("NOTE: callbacks are simulated; this is not physical GPU execution evidence.");
    return 0;
}
