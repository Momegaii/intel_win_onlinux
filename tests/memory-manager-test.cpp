/* Backend operations are deterministic simulations, not GPU validation. */
#include "../include/idw/memory_manager.hpp"

#include <cerrno>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace {

struct FakeBackend {
    uint64_t next_backing = 1;
    uint64_t next_fence = 1;
    int fail_create = 0;
    int fail_destroy = 0;
    int fail_bind = 0;
    int fail_unbind = 0;
    int fail_invalidate = 0;
    int fail_submit = 0;
    int fail_wait = 0;
    int fail_quiesce = 0;
    bool wrong_va = false;
    bool zero_fence = false;
    bool partial_bind = false;
    bool partial_submit = false;
    uint32_t live_backings = 0;
    uint32_t live_bindings = 0;
    std::unordered_map<uint64_t, std::vector<uint32_t>> contents;
};

int create_backing(void *data, uint64_t bytes, uint64_t *backing)
{
    auto& fake = *static_cast<FakeBackend *>(data);
    if (fake.fail_create)
        return fake.fail_create;
    if (!bytes || !backing)
        return -EINVAL;
    *backing = fake.next_backing++;
    ++fake.live_backings;
    fake.contents.emplace(*backing,
                          std::vector<uint32_t>(bytes / sizeof(uint32_t)));
    return 0;
}

int destroy_backing(void *data, uint64_t backing)
{
    auto& fake = *static_cast<FakeBackend *>(data);
    if (fake.fail_destroy)
        return fake.fail_destroy;
    --fake.live_backings;
    fake.contents.erase(backing);
    return 0;
}

int bind_resource(void *data, uint64_t, idw_gpuva requested, uint64_t,
                  idw_gpuva *actual)
{
    auto& fake = *static_cast<FakeBackend *>(data);
    if (fake.fail_bind)
        return fake.fail_bind;
    *actual = fake.wrong_va ? requested + 4096 : requested;
    ++fake.live_bindings;
    if (fake.partial_bind)
        return -EUCLEAN;
    return 0;
}

int unbind_resource(void *data, uint64_t, idw_gpuva, uint64_t)
{
    auto& fake = *static_cast<FakeBackend *>(data);
    if (fake.fail_unbind)
        return fake.fail_unbind;
    --fake.live_bindings;
    return 0;
}

int invalidate(void *data, idw_gpuva, uint64_t)
{
    auto& fake = *static_cast<FakeBackend *>(data);
    return fake.fail_invalidate;
}

int submit_resource(void *data, uint64_t backing, idw_gpuva, uint64_t bytes,
                    const idw::MemorySubmissionCommand& command,
                    uint64_t *fence)
{
    auto& fake = *static_cast<FakeBackend *>(data);
    if (fake.fail_submit)
        return fake.fail_submit;
    if (command.kind == idw::MemorySubmissionKind::WriteDword) {
        if (command.offset > bytes - sizeof(uint32_t) ||
            (command.offset & (sizeof(uint32_t) - 1)))
            return -EINVAL;
        fake.contents[backing][command.offset / sizeof(uint32_t)] =
            command.value;
    } else if (command.kind != idw::MemorySubmissionKind::Noop ||
               command.offset || command.value) {
        return -EINVAL;
    }
    *fence = fake.zero_fence ? 0 : fake.next_fence++;
    if (fake.partial_submit)
        return -EUCLEAN;
    return 0;
}

int cpu_read_dword(void *data, uint64_t backing, uint64_t offset,
                   uint32_t *value)
{
    auto& contents = static_cast<FakeBackend *>(data)->contents;
    const auto it = contents.find(backing);
    if (!value || it == contents.end() ||
        offset / sizeof(uint32_t) >= it->second.size())
        return -EINVAL;
    *value = it->second[offset / sizeof(uint32_t)];
    return 0;
}

int cpu_write_dword(void *data, uint64_t backing, uint64_t offset,
                    uint32_t value)
{
    auto& contents = static_cast<FakeBackend *>(data)->contents;
    const auto it = contents.find(backing);
    if (it == contents.end() ||
        offset / sizeof(uint32_t) >= it->second.size())
        return -EINVAL;
    it->second[offset / sizeof(uint32_t)] = value;
    return 0;
}

int wait_fence(void *data, uint64_t, int64_t timeout_ns)
{
    auto& fake = *static_cast<FakeBackend *>(data);
    if (timeout_ns <= 0)
        return -EINVAL;
    return fake.fail_wait;
}

int quiesce_fence(void *data, uint64_t)
{
    auto& fake = *static_cast<FakeBackend *>(data);
    return fake.fail_quiesce;
}

const idw::MemoryBackendOps kOps{
    create_backing, destroy_backing, bind_resource, unbind_resource,
    invalidate, submit_resource, wait_fence, quiesce_fence,
    cpu_read_dword, cpu_write_dword
};

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int test_complete_lifecycle()
{
    FakeBackend fake;
    idw::MemoryManager manager;
    idw_handle resource = 0;
    idw_gpuva va = 0;
    uint64_t ticket = 0;
    idw::MemoryResourceInfo info{};

    CHECK(manager.initialize(kOps, &fake, 0x10000, 0x100000) == 0);
    CHECK(manager.create(4097, resource) == 0);
    CHECK(fake.live_backings == 1);
    CHECK(manager.query(resource, info) == 0 && info.bytes == 8192);
    CHECK(manager.bind(resource, 0, va) == 0);
    CHECK(va == 0x10000);
    CHECK(manager.submit(resource, ticket) == 0);
    CHECK(manager.query(resource, info) == 0);
    CHECK(info.submission_references == 1 && info.bound);
    CHECK(manager.unbind(resource) == -EBUSY);
    CHECK(manager.destroy(resource) == -EBUSY);
    CHECK(manager.wait(ticket, 1000000) == 0);
    CHECK(manager.submission_count() == 0);
    CHECK(manager.unbind(resource) == 0);
    CHECK(fake.live_bindings == 0);
    CHECK(manager.destroy(resource) == 0);
    CHECK(fake.live_backings == 0);
    CHECK(manager.query(resource, info) == -ENOENT);
    CHECK(manager.shutdown() == 0);
    return 0;
}

int test_mapping_failures_and_recovery()
{
    FakeBackend fake;
    idw::MemoryManager manager;
    idw_handle resource = 0;
    idw_gpuva va = 0;
    idw::MemoryResourceInfo info{};

    CHECK(manager.initialize(kOps, &fake, 0x10000, 0x20000) == 0);
    CHECK(manager.create(4096, resource) == 0);
    fake.fail_bind = -EIO;
    CHECK(manager.bind(resource, 0, va) == -EIO);
    fake.fail_bind = 0;
    CHECK(manager.bind(resource, 0, va) == 0);

    CHECK(manager.unbind(resource) == 0);
    fake.partial_bind = true;
    CHECK(manager.bind(resource, 0, va) == -EUCLEAN);
    CHECK(manager.query(resource, info) == 0);
    CHECK(info.recovery_required && !info.bound);
    CHECK(manager.destroy(resource) == -EBUSY);
    fake.partial_bind = false;
    CHECK(manager.recover_binding(resource) == 0);
    CHECK(fake.live_bindings == 0);
    CHECK(manager.bind(resource, 0, va) == 0);

    fake.fail_unbind = -EIO;
    CHECK(manager.unbind(resource) == -EIO);
    CHECK(manager.query(resource, info) == 0);
    CHECK(!info.bound && info.recovery_required);
    CHECK(manager.destroy(resource) == -EBUSY);
    fake.fail_unbind = 0;
    CHECK(manager.recover_binding(resource) == 0);
    CHECK(fake.live_bindings == 0);
    CHECK(manager.bind(resource, 0, va) == 0);

    fake.fail_invalidate = -EIO;
    CHECK(manager.unbind(resource) == -EIO);
    CHECK(manager.query(resource, info) == 0);
    CHECK(!info.bound && info.recovery_required);
    CHECK(manager.bind(resource, 0, va) == -EBUSY);
    CHECK(manager.destroy(resource) == -EBUSY);
    fake.fail_invalidate = 0;
    CHECK(manager.recover_binding(resource) == 0);
    CHECK(manager.bind(resource, 0, va) == 0);

    fake.wrong_va = true;
    CHECK(manager.unbind(resource) == 0);
    CHECK(manager.bind(resource, 0, va) == -ERANGE);
    CHECK(manager.query(resource, info) == 0);
    CHECK(info.recovery_required);
    uint64_t blocked_ticket = 0;
    CHECK(manager.submit(resource, blocked_ticket) == -EIO);
    idw_handle other_resource = 0;
    CHECK(manager.create(4096, other_resource) == 0);
    idw_gpuva other_va = 0;
    CHECK(manager.bind(other_resource, 0, other_va) == -EIO);
    fake.wrong_va = false;
    CHECK(manager.recover_binding(resource) == 0);
    CHECK(fake.live_bindings == 0);
    CHECK(manager.destroy(resource) == 0);
    CHECK(manager.bind(other_resource, 0, other_va) == 0);
    CHECK(manager.unbind(other_resource) == 0);
    CHECK(manager.destroy(other_resource) == 0);
    CHECK(manager.shutdown() == 0);
    return 0;
}

int test_managed_dword_access_and_write_submission()
{
    constexpr uint32_t initial_value = 0x13579bdf;
    constexpr uint32_t gpu_value = 0x2468ace0;
    FakeBackend fake;
    idw::MemoryManager manager;
    idw_handle resource = 0;
    idw_gpuva va = 0;
    uint64_t ticket = 0;
    uint32_t readback = 0;

    CHECK(manager.initialize(kOps, &fake, 0x10000, 0x20000) == 0);
    CHECK(manager.create(4096, resource) == 0);
    CHECK(manager.bind(resource, 0, va) == 0);
    CHECK(manager.cpu_write_dword(resource, 4, initial_value) == 0);
    CHECK(manager.cpu_read_dword(resource, 4, readback) == 0);
    CHECK(readback == initial_value);
    CHECK(manager.cpu_write_dword(resource, 1, gpu_value) == -EINVAL);
    CHECK(manager.cpu_read_dword(resource, 4096, readback) == -EINVAL);
    CHECK(manager.submit_write_dword(resource, 1, gpu_value, ticket) ==
          -EINVAL);
    CHECK(manager.submit_write_dword(resource, 4096, gpu_value, ticket) ==
          -EINVAL);

    CHECK(manager.submit_write_dword(resource, 4, gpu_value, ticket) == 0);
    CHECK(manager.cpu_read_dword(resource, 4, readback) == -EBUSY);
    CHECK(manager.cpu_write_dword(resource, 4, initial_value) == -EBUSY);
    CHECK(manager.wait(ticket, 1000000) == 0);
    CHECK(manager.cpu_read_dword(resource, 4, readback) == 0);
    CHECK(readback == gpu_value);
    CHECK(manager.unbind(resource) == 0);
    CHECK(manager.destroy(resource) == 0);
    CHECK(manager.shutdown() == 0);
    CHECK(fake.contents.empty());
    return 0;
}

int test_submission_failure_timeout_and_cleanup()
{
    FakeBackend fake;
    idw::MemoryManager manager;
    idw_handle resource = 0;
    idw_gpuva va = 0;
    uint64_t ticket = 0;
    idw::MemoryResourceInfo info{};

    CHECK(manager.initialize(kOps, &fake, 0x10000, 0x20000) == 0);
    CHECK(manager.create(4096, resource) == 0);
    CHECK(manager.bind(resource, 0, va) == 0);
    fake.fail_submit = -EIO;
    CHECK(manager.submit(resource, ticket) == -EIO);
    CHECK(manager.submission_count() == 0);
    CHECK(manager.query(resource, info) == 0);
    CHECK(info.submission_references == 0);

    fake.fail_submit = 0;
    fake.partial_submit = true;
    CHECK(manager.submit(resource, ticket) == -EUCLEAN);
    CHECK(ticket != 0);
    CHECK(manager.query(resource, info) == 0);
    CHECK(info.submission_references == 1);
    CHECK(manager.destroy(resource) == -EBUSY);
    fake.partial_submit = false;
    CHECK(manager.recover_submission(ticket) == 0);

    fake.zero_fence = true;
    CHECK(manager.submit(resource, ticket) == -EPROTO);
    CHECK(ticket != 0);
    CHECK(manager.wait(ticket, 1000000) == -EPROTO);
    CHECK(manager.destroy(resource) == -EBUSY);
    CHECK(manager.recover_submission(ticket) == 0);
    fake.zero_fence = false;

    CHECK(manager.submit(resource, ticket) == 0);
    uint64_t second_ticket = 0;
    CHECK(manager.submit(resource, second_ticket) == 0);
    fake.fail_wait = -ETIMEDOUT;
    CHECK(manager.wait(ticket, 1000000) == -ETIMEDOUT);
    CHECK(manager.wait(second_ticket, 1000000) == -ETIMEDOUT);
    CHECK(manager.destroy(resource) == -EBUSY);
    CHECK(manager.unbind(resource) == -EBUSY);
    fake.fail_quiesce = -EIO;
    CHECK(manager.recover_submission(ticket) == -EIO);
    CHECK(manager.query(resource, info) == 0);
    CHECK(info.submission_references == 2);
    fake.fail_quiesce = 0;
    CHECK(manager.recover_submission(ticket) == 0);
    CHECK(manager.query(resource, info) == 0);
    CHECK(info.submission_references == 1);
    CHECK(manager.wait(second_ticket, 1000000) == -ETIMEDOUT);
    CHECK(manager.recover_submission(second_ticket) == 0);
    CHECK(manager.destroy(resource) == -EBUSY);
    CHECK(manager.unbind(resource) == 0);

    fake.fail_destroy = -EBUSY;
    CHECK(manager.destroy(resource) == -EBUSY);
    CHECK(manager.query(resource, info) == 0);
    fake.fail_destroy = 0;
    CHECK(manager.destroy(resource) == 0);
    CHECK(manager.shutdown() == 0);
    return 0;
}

int test_validation_and_shutdown_guards()
{
    FakeBackend fake;
    idw::MemoryManager manager;
    idw_handle resource = 0;
    idw_gpuva va = 0;
    uint64_t ticket = 0;

    CHECK(manager.initialize(kOps, &fake, 0x10000, 0x20000) == 0);
    CHECK(manager.create(0, resource) == -EINVAL);
    fake.fail_create = -ENOMEM;
    CHECK(manager.create(4096, resource) == -ENOMEM);
    CHECK(fake.live_backings == 0);
    fake.fail_create = 1;
    CHECK(manager.create(4096, resource) == -EPROTO);
    fake.fail_create = 0;
    CHECK(manager.create(4096, resource) == 0);
    CHECK(manager.shutdown() == -EBUSY);
    CHECK(manager.submit(resource, ticket) == -EINVAL);
    CHECK(manager.bind(resource, 0x1, va) == -EINVAL);
    CHECK(manager.destroy(resource) == 0);
    CHECK(manager.shutdown() == 0);
    return 0;
}

} // namespace

int main()
{
    if (test_complete_lifecycle() || test_mapping_failures_and_recovery() ||
        test_managed_dword_access_and_write_submission() ||
        test_submission_failure_timeout_and_cleanup() ||
        test_validation_and_shutdown_guards())
        return 1;
    std::puts("PASS: integrated memory allocation, VA, binding, submission, and recovery lifecycle");
    std::puts("NOTE: backend is simulated; this is not physical GPU validation.");
    return 0;
}
