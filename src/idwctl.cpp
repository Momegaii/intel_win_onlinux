#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <i915_drm.h>
#include "../include/idw/i915_gem_resource_backend.hpp"
#include "../include/idw/gpu_submission.hpp"
#include "../include/uapi/idw_ioctl.h"

namespace fs = std::filesystem;

static std::string read_text(const fs::path& p) {
    std::ifstream f(p);
    std::string s;
    if (f) std::getline(f, s);
    return s;
}
static std::string hex_value(const std::string& s) {
    if (s.empty()) return "unknown";
    const char* begin = s.data();
    const char* end = begin + s.size();
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        begin += 2;
    unsigned long value = 0;
    const auto parsed = std::from_chars(begin, end, value, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return s;
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(4) << std::setfill('0') << value;
    return out.str();
}

static bool parse_u64_decimal(const char* text, uint64_t& value) {
    if (!text || !*text) return false;
    const char* end = text + std::strlen(text);
    uint64_t parsed_value = 0;
    const auto parsed = std::from_chars(text, end, parsed_value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return false;
    value = parsed_value;
    return true;
}

static bool parse_count(const char* text, unsigned& count) {
    uint64_t value = 0;
    if (!parse_u64_decimal(text, value) ||
        value > std::numeric_limits<unsigned>::max())
        return false;
    count = static_cast<unsigned>(value);
    return true;
}
static std::string link_basename(const fs::path& p) {
    std::error_code ec;
    auto q = fs::canonical(p, ec);
    return ec ? "unknown" : q.filename().string();
}
struct Gpu {
    std::string node, vendor, device, revision, driver, pci_path, subsystem_vendor, subsystem_device, boot_vga;
};
static std::vector<Gpu> discover() {
    std::vector<Gpu> out;
    const fs::path drm("/sys/class/drm");
    std::error_code ec;
    if (!fs::exists(drm)) return out;
    for (const auto& e : fs::directory_iterator(drm, ec)) {
        const auto n = e.path().filename().string();
        if (n.rfind("renderD", 0) != 0) continue;
        const fs::path dev = e.path() / "device";
        Gpu g;
        g.node = "/dev/dri/" + n;
        g.vendor = read_text(dev / "vendor");
        g.device = read_text(dev / "device");
        g.revision = read_text(dev / "revision");
        g.subsystem_vendor = read_text(dev / "subsystem_vendor");
        g.subsystem_device = read_text(dev / "subsystem_device");
        g.boot_vga = read_text(dev / "boot_vga");
        g.driver = link_basename(dev / "driver");
        g.pci_path = link_basename(dev);
        if (g.vendor == "0x8086") out.push_back(g);
    }
    std::sort(out.begin(), out.end(), [](const Gpu& a, const Gpu& b){ return a.node < b.node; });
    return out;
}
static bool drm_driver_version(int fd, std::string& name, std::string& version) {
    drmVersionPtr v = drmGetVersion(fd);
    if (!v) return false;
    name.assign(v->name ? v->name : "");
    std::ostringstream s;
    s << v->version_major << "." << v->version_minor << "." << v->version_patchlevel;
    version = s.str();
    drmFreeVersion(v);
    return true;
}

static int gem_resource_backend_test() {
    auto gpus = discover();
    int fd = -1;
    const Gpu* chosen = nullptr;
    for (const auto& gpu : gpus) {
        fd = ::open(gpu.node.c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) { chosen = &gpu; break; }
    }
    if (!chosen) {
        std::cerr << "FAIL: cannot open an Intel render node: "
                  << std::strerror(errno) << "\n";
        return 2;
    }
    if (chosen->driver != "i915") {
        std::cerr << "REFUSED: GEM resource backend test requires i915; found "
                  << chosen->driver << "\n";
        ::close(fd);
        return 3;
    }

    idw::I915GemResourceBackend resources(fd);
    uint32_t handle = 0;
    int ret = resources.create(4096, handle);
    bool ok = ret == 0;
    auto fail_backend = [&](const char* operation, int error) {
        std::cerr << "FAIL: " << operation << ": "
                  << std::strerror(-error) << "\n";
        ok = false;
    };
    if (!ok)
        fail_backend("GEM_CREATE", ret);

    const uint32_t expected[16] = {
        0x49445747u, 0x454d0001u, 0x10203040u, 0x50607080u,
        0x90abcdefu, 0xfedcba09u, 0x13579bdfu, 0x2468ace0u,
        0x55aa55aau, 0xaa55aa55u, 0x31415926u, 0x27182818u,
        0xdeadbeefu, 0xc001d00du, 0x0badf00du, 0x1234fedcu
    };
    auto write_pattern = [&]() {
        drm_i915_gem_pwrite write{};
        write.handle = handle;
        write.size = sizeof(expected);
        write.data_ptr = reinterpret_cast<uint64_t>(expected);
        if (drmIoctl(fd, DRM_IOCTL_I915_GEM_PWRITE, &write) != 0) {
            std::cerr << "FAIL: GEM_PWRITE: " << std::strerror(errno) << "\n";
            ok = false;
        }
    };
    auto verify_pattern = [&]() {
        uint32_t actual[16]{};
        drm_i915_gem_pread read{};
        read.handle = handle;
        read.size = sizeof(actual);
        read.data_ptr = reinterpret_cast<uint64_t>(actual);
        if (drmIoctl(fd, DRM_IOCTL_I915_GEM_PREAD, &read) != 0) {
            std::cerr << "FAIL: GEM_PREAD: " << std::strerror(errno) << "\n";
            ok = false;
        } else if (std::memcmp(expected, actual, sizeof(expected)) != 0) {
            std::cerr << "FAIL: GEM contents did not survive advice/recovery\n";
            ok = false;
        }
    };
    if (ok) {
        idw::I915ResourceSnapshot state{};
        ret = resources.advise_willneed(handle, &state);
        if (ret)
            fail_backend("GEM WILLNEED", ret);
        else if (state.backing == idw::I915BackingState::Purged) {
            std::cerr << "FAIL: newly created GEM object has no retained backing\n";
            ok = false;
        }
    }
    if (ok)
        write_pattern();

    bool purge_observed = false;
    bool busy = false;
    if (ok) {
        idw::I915ResourceSnapshot state{};
        ret = resources.query_activity(handle, &state);
        if (ret)
            fail_backend("GEM_BUSY", ret);
        else
            busy = state.busy;
        if (ok) {
            ret = resources.advise_discardable(handle, &state);
            if (ret)
                fail_backend("GEM DONTNEED", ret);
            else
                purge_observed =
                    state.backing == idw::I915BackingState::Purged;
        }
        if (ok) {
            ret = resources.advise_willneed(handle, &state);
            if (ret)
                fail_backend("GEM WILLNEED after DONTNEED", ret);
            else if (state.backing == idw::I915BackingState::Purged)
                purge_observed = true;
        }
    }

    if (ok && purge_observed) {
        ret = resources.destroy(handle);
        if (ret) {
            fail_backend("close purged GEM object", ret);
        } else {
            handle = 0;
            ret = resources.create(4096, handle);
            if (ret)
                fail_backend("recreate purged GEM object", ret);
            if (ok) {
                idw::I915ResourceSnapshot state{};
                ret = resources.advise_willneed(handle, &state);
                if (ret)
                    fail_backend("replacement GEM WILLNEED", ret);
                else if (state.backing == idw::I915BackingState::Purged) {
                    std::cerr << "FAIL: replacement GEM backing is not retained\n";
                    ok = false;
                }
                if (ok)
                    write_pattern();
            }
        }
    }
    if (ok)
        verify_pattern();

    ret = resources.close_all();
    if (ret)
        fail_backend("GEM_CLOSE", ret);
    ::close(fd);
    if (!ok)
        return 4;

    std::cout << "PASS: i915-assisted GEM resource backend created, advised, "
              << "queried activity, verified data, and closed the object.\n"
              << "Backing purge observed: " << (purge_observed ? "yes" : "no")
              << "; GEM_BUSY sample: " << (busy ? "busy" : "idle") << ".\n"
              << "No GPU command was submitted. GEM_BUSY is activity, not "
              << "residency; i915 owns DMA and GPU mappings.\n";
    return 0;
}

static void print_info(bool json) {
    auto gpus = discover();
    if (json) {
        std::cout << "{\"adapters\":[";
        for (size_t i=0;i<gpus.size();++i) {
            const auto& g=gpus[i];
            if(i) std::cout << ",";
            std::cout << "{\"node\":\""<<g.node<<"\",\"vendor\":\""<<hex_value(g.vendor)
                      <<"\",\"device\":\""<<hex_value(g.device)<<"\",\"revision\":\""<<hex_value(g.revision)
                      <<"\",\"driver\":\""<<g.driver<<"\",\"pci_path\":\""<<g.pci_path
                      <<"\",\"subsystem_vendor\":\""<<hex_value(g.subsystem_vendor)
                      <<"\",\"subsystem_device\":\""<<hex_value(g.subsystem_device)
                      <<"\",\"boot_vga\":\""<<(g.boot_vga.empty()?"unknown":g.boot_vga)<<"\"}";
        }
        std::cout << "]}\n";
        return;
    }
    std::cout << "IDW — Intel GPU Properties\n"
              << "==========================\n";
    if (gpus.empty()) {
        std::cout << "No Intel DRM render node found.\n"
                  << "Check: lspci -nnk | grep -A3 -i vga and ls -l /dev/dri\n";
        return;
    }
    for (size_t i=0;i<gpus.size();++i) {
        const auto& g=gpus[i];
        std::cout << "Adapter " << i << "\n"
                  << "  Render node:      " << g.node << "\n"
                  << "  PCI vendor:       " << hex_value(g.vendor) << "\n"
                  << "  PCI device:       " << hex_value(g.device) << "\n"
                  << "  Revision:         " << hex_value(g.revision) << "\n"
                  << "  Kernel driver:    " << g.driver << "\n"
                  << "  PCI path:         " << g.pci_path << "\n"
                  << "  Subsystem vendor: " << hex_value(g.subsystem_vendor) << "\n"
                  << "  Subsystem device: " << hex_value(g.subsystem_device) << "\n"
                  << "  Boot VGA:         " << (g.boot_vga.empty()?"unknown":g.boot_vga) << "\n";
        int fd = ::open(g.node.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            std::cout << "  DRM access:       " << std::strerror(errno) << "\n";
        } else {
            std::string dn,dv;
            if (drm_driver_version(fd,dn,dv))
                std::cout << "  DRM driver:       " << dn << " " << dv << "\n";
            else
                std::cout << "  DRM driver:       query unavailable\n";
            ::close(fd);
        }
    }
}
static int gem_selftest() {
    auto gpus = discover();
    if (gpus.empty()) { std::cerr << "FAIL: no Intel render node found\n"; return 2; }
    const Gpu* chosen = nullptr;
    int fd = -1;
    for (const auto& g : gpus) {
        fd = ::open(g.node.c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) { chosen = &g; break; }
    }
    if (!chosen) {
        std::cerr << "FAIL: cannot open Intel render node: " << std::strerror(errno) << "\n";
        return 2;
    }
    std::cout << "Using " << chosen->node << " (" << chosen->driver << ")\n";
    drm_i915_gem_create create{};
    create.size = 4096;
    if (drmIoctl(fd, DRM_IOCTL_I915_GEM_CREATE, &create) != 0) {
        std::cerr << "FAIL: GEM_CREATE: " << std::strerror(errno) << "\n";
        ::close(fd); return 3;
    }
    constexpr uint32_t N = 64;
    uint32_t source[N], target[N]{};
    for (uint32_t i=0;i<N;++i) source[i] = 0x1D0F0000u ^ (i * 0x10203u);
    drm_i915_gem_pwrite wr{};
    wr.handle = create.handle; wr.offset = 0; wr.size = sizeof(source);
    wr.data_ptr = reinterpret_cast<uint64_t>(source);
    bool ok = drmIoctl(fd, DRM_IOCTL_I915_GEM_PWRITE, &wr) == 0;
    if (!ok) std::cerr << "FAIL: GEM_PWRITE: " << std::strerror(errno) << "\n";
    if (ok) {
        drm_i915_gem_pread rd{};
        rd.handle = create.handle; rd.offset = 0; rd.size = sizeof(target);
        rd.data_ptr = reinterpret_cast<uint64_t>(target);
        ok = drmIoctl(fd, DRM_IOCTL_I915_GEM_PREAD, &rd) == 0;
        if (!ok) std::cerr << "FAIL: GEM_PREAD: " << std::strerror(errno) << "\n";
    }
    if (ok) ok = std::memcmp(source,target,sizeof(source)) == 0;
    drm_gem_close close_req{};
    close_req.handle = create.handle;
    if (drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_req) != 0) {
        std::cerr << "WARN: GEM_CLOSE: " << std::strerror(errno) << "\n";
        ok = false;
    }
    ::close(fd);
    if (!ok) { std::cerr << "FAIL: GEM data verification did not pass\n"; return 4; }
    std::cout << "PASS: created a real 4 KiB i915 GEM object, wrote 256 bytes, read them back, verified, and closed it.\n"
              << "NOTE: this tests DRM/GEM memory handling only; it does not execute GPU commands.\n";
    return 0;
}

struct GemAllocation {
    uint32_t handle = 0;
    uint64_t size = 0;
};

static int gem_memory_lifecycle_test(unsigned count, uint64_t requested_size) {
    constexpr uint64_t PAGE = 4096;
    constexpr uint64_t MAX_OBJECT = 256ULL * 1024 * 1024;
    constexpr unsigned MAX_OBJECTS = 256;
    if (!count || count > MAX_OBJECTS || requested_size < PAGE || requested_size > MAX_OBJECT) {
        std::cerr << "Usage constraints: count=1.." << MAX_OBJECTS
                  << ", size=4096.." << MAX_OBJECT << " bytes\n";
        return 1;
    }
    if (requested_size > UINT64_MAX - (PAGE - 1)) {
        std::cerr << "FAIL: allocation size overflow\n";
        return 1;
    }
    const uint64_t object_size = (requested_size + PAGE - 1) & ~(PAGE - 1);
    auto gpus = discover();
    if (gpus.empty()) { std::cerr << "FAIL: no Intel DRM render node found\n"; return 2; }
    int fd = -1;
    const Gpu* chosen = nullptr;
    for (const auto& gpu : gpus) {
        fd = ::open(gpu.node.c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) { chosen = &gpu; break; }
    }
    if (!chosen) {
        std::cerr << "FAIL: cannot open Intel render node: " << std::strerror(errno) << "\n";
        return 2;
    }

    std::vector<GemAllocation> objects;
    objects.reserve(count);
    bool ok = true;
    for (unsigned i = 0; i < count; ++i) {
        drm_i915_gem_create req{};
        req.size = object_size;
        if (drmIoctl(fd, DRM_IOCTL_I915_GEM_CREATE, &req) != 0) {
            std::cerr << "FAIL: GEM_CREATE object " << i << ": " << std::strerror(errno) << "\n";
            ok = false;
            break;
        }
        objects.push_back({req.handle, object_size});

        // Exercise a bounded data path without mapping or executing GPU commands.
        uint32_t source[64], target[64]{};
        for (uint32_t j = 0; j < 64; ++j)
            source[j] = 0x1D0F0000u ^ (i * 0x10001u) ^ (j * 0x10203u);
        drm_i915_gem_pwrite wr{};
        wr.handle = req.handle; wr.offset = 0; wr.size = sizeof(source);
        wr.data_ptr = reinterpret_cast<uint64_t>(source);
        if (drmIoctl(fd, DRM_IOCTL_I915_GEM_PWRITE, &wr) != 0) {
            std::cerr << "FAIL: GEM_PWRITE object " << i << ": " << std::strerror(errno) << "\n";
            ok = false;
            break;
        }
        drm_i915_gem_pread rd{};
        rd.handle = req.handle; rd.offset = 0; rd.size = sizeof(target);
        rd.data_ptr = reinterpret_cast<uint64_t>(target);
        if (drmIoctl(fd, DRM_IOCTL_I915_GEM_PREAD, &rd) != 0 ||
            std::memcmp(source, target, sizeof(source)) != 0) {
            std::cerr << "FAIL: GEM_PREAD/verify object " << i << "\n";
            ok = false;
            break;
        }
    }

    unsigned closed = 0;
    for (auto it = objects.rbegin(); it != objects.rend(); ++it) {
        drm_gem_close req{};
        req.handle = it->handle;
        if (drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &req) != 0) {
            std::cerr << "FAIL: GEM_CLOSE handle " << it->handle << ": " << std::strerror(errno) << "\n";
            ok = false;
        } else {
            ++closed;
        }
    }
    ::close(fd);
    if (!ok || closed != objects.size()) {
        std::cerr << "FAIL: GEM lifecycle incomplete; created=" << objects.size()
                  << " closed=" << closed << "\n";
        return 4;
    }
    std::cout << "PASS: i915 GEM lifecycle: created, wrote, read, verified and closed "
              << objects.size() << " objects (" << object_size << " bytes each; "
              << (static_cast<uint64_t>(objects.size()) * object_size) << " bytes total).\n"
              << "NOTE: these are real DRM GEM objects; IDW does not own them and no GPU commands were submitted.\n";
    return 0;
}

static int kernel_status() {
    const int fd = ::open("/dev/idw", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        std::cerr << "IDW kernel control device unavailable: " << std::strerror(errno) << "\n"
                  << "Build/load the optional module with matching kernel headers to query it.\n";
        return 2;
    }
    struct idw_query q{};
    q.struct_size = sizeof(q);
    if (::ioctl(fd, IDW_IOCTL_QUERY, &q) != 0) {
        std::cerr << "IDW_QUERY failed: " << std::strerror(errno) << "\n";
        ::close(fd);
        return 3;
    }
    ::close(fd);
    std::cout << "IDW kernel interface\n"
              << "  ABI:       " << q.abi_major << "." << q.abi_minor << "\n"
              << "  State:     0x" << std::hex << q.state << "\n"
              << "  Features:  0x" << q.features << std::dec << "\n"
              << "  Miniport:  " << ((q.state & IDW_STATE_MINIPORT_PRESENT) ? "registered" : "none") << "\n"
              << "  GPU owner: " << ((q.state & IDW_STATE_DEVICE_OWNED) ? "IDW" : "not IDW (expected: i915)") << "\n";
    return 0;
}

static int idw_resource_test(unsigned count, uint64_t requested_size) {
    constexpr unsigned MAX_OBJECTS = 256;
    constexpr uint64_t MAX_SIZE = 256ULL * 1024 * 1024;
    if (!count || count > MAX_OBJECTS || !requested_size || requested_size > MAX_SIZE) {
        std::cerr << "Usage constraints: count=1.." << MAX_OBJECTS
                  << ", size=1.." << MAX_SIZE << " bytes\n";
        return 1;
    }
    int fd = ::open("/dev/idw", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::cerr << "Cannot open /dev/idw: " << std::strerror(errno)
                  << " (load idw_core.ko; device mode is 0600 by default)\n";
        return 2;
    }
    std::vector<uint32_t> ids;
    bool ok = true;
    for (unsigned i = 0; i < count; ++i) {
        idw_allocation_create req{};
        req.struct_size = sizeof(req);
        req.size = requested_size;
        if (::ioctl(fd, IDW_IOCTL_ALLOCATION_CREATE, &req) != 0) {
            std::cerr << "FAIL: IDW allocation create " << i << ": " << std::strerror(errno) << "\n";
            ok = false;
            break;
        }
        ids.push_back(req.allocation_id);
        std::cout << "  created IDW handle=" << req.allocation_id
                  << " size=" << req.size << " bytes\n";
    }
    if (!ids.empty()) {
        const auto page_size = static_cast<size_t>(::getpagesize());
        const off_t offset = static_cast<off_t>(ids.front()) *
                             static_cast<off_t>(page_size);
        void* mapping = ::mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, offset);
        if (mapping != MAP_FAILED) {
            std::cerr << "FAIL: metadata-only allocation unexpectedly allowed mmap\n";
            ::munmap(mapping, page_size);
            ok = false;
        } else if (errno != EINVAL) {
            std::cerr << "FAIL: metadata-only mmap returned unexpected error: "
                      << std::strerror(errno) << "\n";
            ok = false;
        }
    }
    unsigned destroyed = 0;
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
        idw_allocation_destroy req{};
        req.struct_size = sizeof(req);
        req.allocation_id = *it;
        if (::ioctl(fd, IDW_IOCTL_ALLOCATION_DESTROY, &req) != 0) {
            std::cerr << "FAIL: IDW allocation destroy " << *it << ": " << std::strerror(errno) << "\n";
            ok = false;
        } else {
            ++destroyed;
        }
    }
    if (!ok || destroyed != ids.size()) {
        std::cerr << "FAIL: IDW resource lifetime incomplete; created=" << ids.size()
                  << " destroyed=" << destroyed << "\n";
        ::close(fd);
        return 4;
    }
    idw_allocation_create replacement{};
    replacement.struct_size = sizeof(replacement);
    replacement.size = requested_size;
    if (::ioctl(fd, IDW_IOCTL_ALLOCATION_CREATE, &replacement) != 0) {
        std::cerr << "FAIL: create replacement IDW allocation: " << std::strerror(errno) << "\n";
        ::close(fd);
        return 4;
    }
    if (replacement.allocation_id == ids.front()) {
        std::cerr << "FAIL: IDW reused a destroyed allocation handle\n";
        idw_allocation_destroy cleanup{};
        cleanup.struct_size = sizeof(cleanup);
        cleanup.allocation_id = replacement.allocation_id;
        (void)::ioctl(fd, IDW_IOCTL_ALLOCATION_DESTROY, &cleanup);
        ::close(fd);
        return 4;
    }
    idw_allocation_destroy stale{};
    stale.struct_size = sizeof(stale);
    stale.allocation_id = ids.front();
    if (::ioctl(fd, IDW_IOCTL_ALLOCATION_DESTROY, &stale) == 0 || errno != ENOENT) {
        std::cerr << "FAIL: stale IDW allocation handle was not rejected\n";
        idw_allocation_destroy cleanup{};
        cleanup.struct_size = sizeof(cleanup);
        cleanup.allocation_id = replacement.allocation_id;
        (void)::ioctl(fd, IDW_IOCTL_ALLOCATION_DESTROY, &cleanup);
        ::close(fd);
        return 4;
    }
    idw_allocation_destroy cleanup{};
    cleanup.struct_size = sizeof(cleanup);
    cleanup.allocation_id = replacement.allocation_id;
    if (::ioctl(fd, IDW_IOCTL_ALLOCATION_DESTROY, &cleanup) != 0) {
        std::cerr << "FAIL: destroy replacement IDW allocation: " << std::strerror(errno) << "\n";
        ::close(fd);
        return 4;
    }

    idw_allocation_create backed{};
    backed.struct_size = sizeof(backed);
    backed.flags = IDW_ALLOCATION_CREATE_SYSTEM_BACKED;
    backed.size = std::min<uint64_t>(requested_size,
        static_cast<uint64_t>(::getpagesize()) * 4);
    if (::ioctl(fd, IDW_IOCTL_ALLOCATION_CREATE, &backed) != 0) {
        std::cerr << "FAIL: create system-backed IDW allocation: "
                  << std::strerror(errno) << "\n";
        ::close(fd);
        return 4;
    }
    const size_t backing_size = static_cast<size_t>(backed.size);
    const size_t page_size = static_cast<size_t>(::getpagesize());
    const off_t backing_offset = static_cast<off_t>(backed.allocation_id) *
                                 static_cast<off_t>(page_size);
    void* mapping = ::mmap(nullptr, backing_size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, backing_offset);
    if (mapping == MAP_FAILED) {
        std::cerr << "FAIL: mmap system-backed IDW allocation: "
                  << std::strerror(errno) << "\n";
        idw_allocation_destroy destroy_backing{};
        destroy_backing.struct_size = sizeof(destroy_backing);
        destroy_backing.allocation_id = backed.allocation_id;
        (void)::ioctl(fd, IDW_IOCTL_ALLOCATION_DESTROY, &destroy_backing);
        ::close(fd);
        return 4;
    }
    auto* mapped_bytes = static_cast<uint8_t*>(mapping);
    for (size_t i = 0; i < backing_size; ++i)
        mapped_bytes[i] = static_cast<uint8_t>((i * 37u) ^ 0x5au);
    idw_allocation_destroy destroy_backing{};
    destroy_backing.struct_size = sizeof(destroy_backing);
    destroy_backing.allocation_id = backed.allocation_id;
    if (::ioctl(fd, IDW_IOCTL_ALLOCATION_DESTROY, &destroy_backing) == 0 ||
        errno != EBUSY) {
        std::cerr << "FAIL: mapped system-backed allocation was not protected from destroy\n";
        ::munmap(mapping, backing_size);
        ::close(fd);
        return 4;
    }
    ::close(fd); // also guarantees per-file resources are reclaimed on abnormal exit
    bool backing_ok = true;
    for (size_t i = 0; i < backing_size; ++i) {
        if (mapped_bytes[i] != static_cast<uint8_t>((i * 37u) ^ 0x5au)) {
            backing_ok = false;
            break;
        }
    }
    if (::munmap(mapping, backing_size) != 0) {
        std::cerr << "FAIL: munmap system-backed IDW allocation: "
                  << std::strerror(errno) << "\n";
        return 4;
    }
    if (!backing_ok) {
        std::cerr << "FAIL: system-backed mapping did not survive fd close\n";
        return 4;
    }
    std::cout << "PASS: IDW metadata resource lifecycle: " << ids.size()
              << " handles created and destroyed; stale handles are rejected. "
              << "Mapped allocations reject destroy and survive fd close; no GPU memory was allocated.\n";
    return 0;
}

static int dmabuf_import_test() {
    auto gpus = discover();
    if (gpus.empty()) {
        std::cerr << "FAIL: no Intel DRM render node found\n";
        return 2;
    }
    int drm_fd = -1;
    const Gpu* chosen = nullptr;
    for (const auto& gpu : gpus) {
        drm_fd = ::open(gpu.node.c_str(), O_RDWR | O_CLOEXEC);
        if (drm_fd >= 0) { chosen = &gpu; break; }
    }
    if (!chosen) {
        std::cerr << "FAIL: cannot open Intel render node: " << std::strerror(errno) << "\n";
        return 2;
    }
    int idw_fd = ::open("/dev/idw", O_RDWR | O_CLOEXEC);
    if (idw_fd < 0) {
        std::cerr << "FAIL: cannot open /dev/idw: " << std::strerror(errno) << "\n";
        ::close(drm_fd);
        return 2;
    }

    drm_i915_gem_create create{};
    create.size = 4096;
    bool ok = drmIoctl(drm_fd, DRM_IOCTL_I915_GEM_CREATE, &create) == 0;
    if (!ok) std::cerr << "FAIL: GEM_CREATE: " << std::strerror(errno) << "\n";

    int prime_fd = -1;
    if (ok) {
        drm_prime_handle prime{};
        prime.handle = create.handle;
        prime.flags = DRM_CLOEXEC | DRM_RDWR;
        if (drmIoctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0) {
            std::cerr << "FAIL: PRIME_HANDLE_TO_FD: " << std::strerror(errno) << "\n";
            ok = false;
        } else prime_fd = prime.fd;
    }

    uint32_t idw_handle = 0;
    if (ok) {
        idw_allocation_import_dmabuf import{};
        import.struct_size = sizeof(import);
        import.dma_buf_fd = prime_fd;
        if (::ioctl(idw_fd, IDW_IOCTL_ALLOCATION_IMPORT_DMABUF, &import) != 0) {
            std::cerr << "FAIL: IDW dma-buf import: " << std::strerror(errno) << "\n";
            ok = false;
        } else {
            idw_handle = import.allocation_id;
            std::cout << "  IDW imported dma-buf as handle=" << idw_handle << "\n";
        }
    }

    // Close the userspace dma-buf descriptor first. IDW's dma_buf_get() reference
    // must keep the exporter object alive until the IDW handle is destroyed.
    if (prime_fd >= 0) { ::close(prime_fd); prime_fd = -1; }
    if (idw_handle) {
        idw_allocation_destroy destroy{};
        destroy.struct_size = sizeof(destroy);
        destroy.allocation_id = idw_handle;
        if (::ioctl(idw_fd, IDW_IOCTL_ALLOCATION_DESTROY, &destroy) != 0) {
            std::cerr << "FAIL: IDW imported allocation destroy: " << std::strerror(errno) << "\n";
            ok = false;
        }
    }
    if (create.handle) {
        drm_gem_close close_req{};
        close_req.handle = create.handle;
        if (drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &close_req) != 0) {
            std::cerr << "FAIL: GEM_CLOSE: " << std::strerror(errno) << "\n";
            ok = false;
        }
    }
    ::close(idw_fd);
    ::close(drm_fd);
    if (!ok || !idw_handle) return 4;
    std::cout << "PASS: i915 GEM object exported as dma-buf, imported by IDW, userspace dma-buf fd closed, IDW reference released, and GEM handle closed.\n"
              << "NOTE: i915 remains the exporter and GPU-memory owner; IDW retains a dma-buf reference only. No GPU commands were submitted.\n";
    return 0;
}


static int i915_submit_context_create(void *userdata, uint32_t *context_id) {
    auto *fd = static_cast<int *>(userdata);
    drm_i915_gem_context_create context{};
    if (drmIoctl(*fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &context) != 0)
        return -errno;
    *context_id = context.ctx_id;
    return 0;
}

static int i915_submit_gem_create(void *userdata, uint64_t size,
                                  uint32_t *handle) {
    auto *fd = static_cast<int *>(userdata);
    drm_i915_gem_create create{};
    create.size = size;
    if (drmIoctl(*fd, DRM_IOCTL_I915_GEM_CREATE, &create) != 0)
        return -errno;
    *handle = create.handle;
    return 0;
}

static int i915_submit_batch_upload(void *userdata, uint32_t handle,
                                    const void *bytes, uint32_t length) {
    auto *fd = static_cast<int *>(userdata);
    drm_i915_gem_pwrite write{};
    write.handle = handle;
    write.offset = 0;
    write.size = length;
    write.data_ptr = reinterpret_cast<uint64_t>(bytes);
    return drmIoctl(*fd, DRM_IOCTL_I915_GEM_PWRITE, &write) == 0 ? 0 : -errno;
}

static int i915_submit_exec(void *userdata, uint32_t context_id,
                            idw::HaswellEngine engine, uint32_t handle,
                            uint32_t batch_length, uint64_t *gpu_va) {
    if (engine != idw::HaswellEngine::Render)
        return -EINVAL;
    auto *fd = static_cast<int *>(userdata);
    drm_i915_gem_exec_object2 object{};
    object.handle = handle;
    drm_i915_gem_execbuffer2 exec{};
    exec.buffers_ptr = reinterpret_cast<uint64_t>(&object);
    exec.buffer_count = 1;
    exec.batch_start_offset = 0;
    exec.batch_len = batch_length;
    exec.flags = I915_EXEC_RENDER;
    exec.rsvd1 = context_id;
    if (drmIoctl(*fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &exec) != 0)
        return -errno;
    *gpu_va = object.offset;
    return 0;
}

static int i915_submit_wait(void *userdata, uint32_t handle,
                            int64_t timeout_ns) {
    auto *fd = static_cast<int *>(userdata);
    drm_i915_gem_wait wait{};
    wait.bo_handle = handle;
    wait.timeout_ns = timeout_ns;
    return drmIoctl(*fd, DRM_IOCTL_I915_GEM_WAIT, &wait) == 0 ? 0 : -errno;
}

static int i915_submit_gem_close(void *userdata, uint32_t handle) {
    auto *fd = static_cast<int *>(userdata);
    drm_gem_close close_request{};
    close_request.handle = handle;
    return drmIoctl(*fd, DRM_IOCTL_GEM_CLOSE, &close_request) == 0 ? 0 : -errno;
}

static int i915_submit_context_destroy(void *userdata, uint32_t context_id) {
    auto *fd = static_cast<int *>(userdata);
    drm_i915_gem_context_destroy destroy{};
    destroy.ctx_id = context_id;
    return drmIoctl(*fd, DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, &destroy) == 0 ?
           0 : -errno;
}

static int gpu_submit_test(unsigned count) {
    if (count == 0 || count > 1000) {
        std::cerr << "Usage: gpu-submit [count: 1..1000]\n";
        return 1;
    }
    auto gpus = discover();
    if (gpus.empty()) {
        std::cerr << "FAIL: no Intel DRM render node found\n";
        return 2;
    }

    int fd = -1;
    const Gpu *chosen = nullptr;
    for (const auto& gpu : gpus) {
        if (gpu.device != "0x0412")
            continue;
        fd = ::open(gpu.node.c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            chosen = &gpu;
            break;
        }
    }
    if (!chosen) {
        std::cerr << "REFUSED: gpu-submit requires Haswell 0x0412; no matching "
                     "render node could be opened\n";
        return 2;
    }
    if (chosen->driver != "i915") {
        std::cerr << "REFUSED: expected i915 ownership for the assisted "
                     "execbuffer path; found " << chosen->driver << "\n";
        ::close(fd);
        return 3;
    }

    const uint32_t batch[2] = {
        idw::kHaswellMiBatchBufferEnd, idw::kHaswellMiNoop
    };
    const idw::GpuSubmissionOps ops = {
        i915_submit_context_create,
        i915_submit_gem_create,
        i915_submit_batch_upload,
        i915_submit_exec,
        i915_submit_wait,
        i915_submit_gem_close,
        i915_submit_context_destroy
    };
    idw::GpuSubmissionResult result{};
    const int ret = idw::submit_haswell_noop_batches(
        ops, &fd, &batch, sizeof(batch), count,
        idw::kDefaultGpuWaitTimeoutNs, result);
    const int close_ret = ::close(fd) == 0 ? 0 : -errno;
    if (ret || close_ret) {
        const int error = ret ? ret : close_ret;
        std::cerr << "FAIL: gpu-submit completed "
                  << result.completed_batches << "/" << count
                  << " batches at "
                  << idw::gpu_submission_stage_name(result.failed_stage)
                  << ": " << std::strerror(-error) << "\n";
        if (result.cleanup_error)
            std::cerr << "FAIL: submission cleanup also failed: "
                      << std::strerror(-result.cleanup_error) << "\n";
        return 4;
    }

    std::cout << "PASS: i915-assisted Haswell render submission completed "
              << result.completed_batches << "/" << count << " batch(es).\n"
              << "Context: " << result.context_id << " (i915-managed GPU VM).\n"
              << "Returned batch GPU VA: 0x" << std::hex << result.last_gpu_va
              << std::dec << " (4 KiB aligned).\n"
              << "Completion evidence: GEM_WAIT returned success for every batch; "
                 "each GEM handle and the context were closed.\n"
              << "Batch: Haswell MI_BATCH_BUFFER_END followed by MI_NOOP "
                 "padding; no workload.\n"
              << "Ownership: i915 performed address translation, execution, "
                 "scheduling and completion. This is not IDW-owned execution.\n";
    return 0;
}


/* Bind an IDW-imported dma-buf-backed GEM object into an i915 context VM.
 * i915 owns the actual PPGTT mapping. The batch is deliberately a no-op: this
 * proves VM residency/lifetime integration without issuing writes to the target.
 */
static int gpu_resource_test() {
    auto gpus = discover();
    if (gpus.empty()) { std::cerr << "FAIL: no Intel DRM render node found\n"; return 2; }
    int drm_fd = -1;
    const Gpu* chosen = nullptr;
    for (const auto& gpu : gpus) {
        drm_fd = ::open(gpu.node.c_str(), O_RDWR | O_CLOEXEC);
        if (drm_fd >= 0) { chosen = &gpu; break; }
    }
    if (!chosen) { std::cerr << "FAIL: cannot open Intel render node: " << std::strerror(errno) << "\n"; return 2; }
    if (chosen->driver != "i915") {
        std::cerr << "REFUSED: this legacy execbuffer test requires i915; found " << chosen->driver << "\n";
        ::close(drm_fd); return 3;
    }
    int idw_fd = ::open("/dev/idw", O_RDWR | O_CLOEXEC);
    if (idw_fd < 0) {
        std::cerr << "FAIL: cannot open /dev/idw: " << std::strerror(errno)
                  << " (load idw_core.ko and check permissions)\n";
        ::close(drm_fd); return 2;
    }

    uint32_t resource_handle = 0, batch_handle = 0, allocation_id = 0;
    int prime_fd = -1;
    uint32_t context_id = 0;
    bool context_created = false;
    bool ok = true;
    auto fail = [&](const char* what) {
        std::cerr << "FAIL: " << what << ": " << std::strerror(errno) << "\n";
        ok = false;
    };

    drm_i915_gem_create resource{};
    resource.size = 4096;
    if (drmIoctl(drm_fd, DRM_IOCTL_I915_GEM_CREATE, &resource) != 0) fail("resource GEM_CREATE");
    else resource_handle = resource.handle;

    if (ok) {
        drm_prime_handle prime{};
        prime.handle = resource_handle;
        prime.flags = DRM_CLOEXEC | DRM_RDWR;
        if (drmIoctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0) fail("PRIME_HANDLE_TO_FD");
        else prime_fd = prime.fd;
    }
    if (ok) {
        idw_allocation_import_dmabuf import{};
        import.struct_size = sizeof(import);
        import.dma_buf_fd = prime_fd;
        if (::ioctl(idw_fd, IDW_IOCTL_ALLOCATION_IMPORT_DMABUF, &import) != 0) fail("IDW dma-buf import");
        else allocation_id = import.allocation_id;
    }
    if (prime_fd >= 0) { ::close(prime_fd); prime_fd = -1; }

    if (ok) {
        drm_i915_gem_context_create context{};
        if (drmIoctl(drm_fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &context) != 0) fail("context create");
        else { context_id = context.ctx_id; context_created = true; }
    }
    if (ok) {
        drm_i915_gem_create batch{};
        batch.size = 4096;
        if (drmIoctl(drm_fd, DRM_IOCTL_I915_GEM_CREATE, &batch) != 0) fail("batch GEM_CREATE");
        else batch_handle = batch.handle;
    }
    if (ok) {
        const uint32_t words[2] = {
            idw::kHaswellMiBatchBufferEnd, idw::kHaswellMiNoop
        };
        drm_i915_gem_pwrite write{};
        write.handle = batch_handle;
        write.size = sizeof(words);
        write.data_ptr = reinterpret_cast<uint64_t>(words);
        if (drmIoctl(drm_fd, DRM_IOCTL_I915_GEM_PWRITE, &write) != 0) fail("batch upload");
    }

    uint64_t resource_vm_offset = 0;
    if (ok) {
        drm_i915_gem_exec_object2 objects[2]{};
        objects[0].handle = batch_handle;
        objects[1].handle = resource_handle;
        drm_i915_gem_execbuffer2 exec{};
        exec.buffers_ptr = reinterpret_cast<uint64_t>(objects);
        exec.buffer_count = 2;
        exec.batch_len = idw::kHaswellNoopBatchLength;
        exec.flags = I915_EXEC_RENDER;
        exec.rsvd1 = context_id;
        if (drmIoctl(drm_fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &exec) != 0) fail("resource-backed execbuffer2");
        else {
            resource_vm_offset = objects[1].offset;
            drm_i915_gem_wait wait{};
            wait.bo_handle = batch_handle;
            wait.timeout_ns = 5LL * 1000 * 1000 * 1000;
            if (drmIoctl(drm_fd, DRM_IOCTL_I915_GEM_WAIT, &wait) != 0) fail("GPU completion wait");
        }
    }

    if (allocation_id) {
        idw_allocation_destroy destroy{};
        destroy.struct_size = sizeof(destroy);
        destroy.allocation_id = allocation_id;
        if (::ioctl(idw_fd, IDW_IOCTL_ALLOCATION_DESTROY, &destroy) != 0) fail("IDW allocation destroy");
    }
    if (batch_handle) {
        drm_gem_close close_req{}; close_req.handle = batch_handle;
        if (drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &close_req) != 0) fail("batch GEM_CLOSE");
    }
    if (resource_handle) {
        drm_gem_close close_req{}; close_req.handle = resource_handle;
        if (drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &close_req) != 0) fail("resource GEM_CLOSE");
    }
    if (context_created) {
        drm_i915_gem_context_destroy destroy{}; destroy.ctx_id = context_id;
        if (drmIoctl(drm_fd, DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, &destroy) != 0) fail("context destroy");
    }
    ::close(idw_fd);
    ::close(drm_fd);
    if (!ok) return 4;
    std::cout << "PASS: IDW retained an i915 GEM dma-buf; the same GEM resource was included in a real i915 context execbuffer and bound by i915 into that context VM.\n"
              << "Context: " << context_id << "\n"
              << "Returned resource VM offset: 0x" << std::hex << resource_vm_offset << std::dec << "\n"
              << "Completion: batch fence waited successfully.\n"
              << "Boundary: IDW retains object lifetime; i915 owns the PPGTT mapping, scheduler and fence. The no-op batch does not read or write resource contents.\n";
    return 0;
}

/* Exercise real context GPU-VA placement and GEM backing advice through i915.
 * This validates the ownership boundary only; IDW does not manage PPGTT.
 */
static int gpu_memory_test() {
    constexpr uint64_t PAGE_SIZE = 4096;
    constexpr uint64_t RESOURCE_GPUVA = 0x10000000;
    constexpr uint64_t BATCH_GPUVA = 0x10100000;
    constexpr size_t DATA_SIZE = 64;

    auto gpus = discover();
    if (gpus.empty()) {
        std::cerr << "FAIL: no Intel DRM render node found\n";
        return 2;
    }

    int fd = -1;
    const Gpu* chosen = nullptr;
    for (const auto& gpu : gpus) {
        fd = ::open(gpu.node.c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) { chosen = &gpu; break; }
    }
    if (!chosen) {
        std::cerr << "FAIL: cannot open Intel render node: " << std::strerror(errno) << "\n";
        return 2;
    }
    if (chosen->device != "0x0412" || chosen->driver != "i915") {
        std::cerr << "REFUSED: gpu-memory-test requires Haswell 0x0412 owned by i915; found "
                  << chosen->device << " with " << chosen->driver << "\n";
        ::close(fd);
        return 3;
    }

    idw::I915GemResourceBackend gem_resources(fd);
    uint32_t resource_handle = 0;
    uint32_t batch_handle = 0;
    uint32_t context_id = 0;
    bool context_created = false;
    bool ok = true;
    const uint32_t data[DATA_SIZE / sizeof(uint32_t)] = {
        0x49445730u, 0x47505556u, 0x4d454d31u, 0x00000001u,
        0x10203040u, 0x50607080u, 0x90abcdefu, 0xfedcba09u,
        0x13579bdfu, 0x2468ace0u, 0x55aa55aau, 0xaa55aa55u,
        0x31415926u, 0x27182818u, 0xdeadbeefu, 0xc001d00du
    };
    auto fail = [&](const char* operation) {
        std::cerr << "FAIL: " << operation << ": " << std::strerror(errno) << "\n";
        ok = false;
    };
    auto fail_backend = [&](const char* operation, int error) {
        std::cerr << "FAIL: " << operation << ": "
                  << std::strerror(-error) << "\n";
        ok = false;
    };

    int create_ret = gem_resources.create(PAGE_SIZE, resource_handle);
    if (create_ret)
        fail_backend("resource GEM_CREATE", create_ret);
    if (ok) {
        const int ret = gem_resources.create(PAGE_SIZE, batch_handle);
        if (ret)
            fail_backend("batch GEM_CREATE", ret);
    }

    auto mark_willneed = [&](uint32_t handle, const char* operation) {
        idw::I915ResourceSnapshot state{};
        const int ret = gem_resources.advise_willneed(handle, &state);
        if (ret) {
            fail_backend(operation, ret);
            return false;
        }
        if (state.backing == idw::I915BackingState::Purged) {
            std::cerr << "FAIL: " << operation << ": backing is not retained\n";
            ok = false;
            return false;
        }
        return true;
    };
    if (ok) mark_willneed(resource_handle, "resource WILLNEED");

    if (ok) {
        drm_i915_gem_pwrite write{};
        write.handle = resource_handle;
        write.size = sizeof(data);
        write.data_ptr = reinterpret_cast<uint64_t>(data);
        if (drmIoctl(fd, DRM_IOCTL_I915_GEM_PWRITE, &write) != 0)
            fail("resource upload");
    }

    if (ok) {
        const uint32_t batch_words[2] = {
            idw::kHaswellMiBatchBufferEnd, idw::kHaswellMiNoop
        };
        drm_i915_gem_pwrite write{};
        write.handle = batch_handle;
        write.size = sizeof(batch_words);
        write.data_ptr = reinterpret_cast<uint64_t>(batch_words);
        if (drmIoctl(fd, DRM_IOCTL_I915_GEM_PWRITE, &write) != 0)
            fail("batch upload");
    }

    uint64_t resource_gpuva = 0;
    uint64_t batch_gpuva = 0;
    bool resource_busy_observed = false;
    auto create_context = [&]() {
        drm_i915_gem_context_create context{};
        if (drmIoctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &context) != 0) {
            fail("context create");
            return false;
        }
        context_id = context.ctx_id;
        context_created = true;
        return true;
    };
    auto destroy_context = [&](const char* operation) {
        if (!context_created) return true;
        drm_i915_gem_context_destroy destroy{};
        destroy.ctx_id = context_id;
        if (drmIoctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, &destroy) != 0) {
            fail(operation);
            return false;
        }
        context_created = false;
        gem_resources.release_context(context_id);
        return true;
    };
    auto bind_and_submit = [&]() {
        if (!create_context()) return false;
        drm_i915_gem_exec_object2 objects[2]{};
        objects[0].handle = resource_handle;
        objects[0].offset = RESOURCE_GPUVA;
        objects[0].flags = EXEC_OBJECT_PINNED;
        objects[1].handle = batch_handle;
        objects[1].offset = BATCH_GPUVA;
        objects[1].flags = EXEC_OBJECT_PINNED;

        drm_i915_gem_execbuffer2 exec{};
        exec.buffers_ptr = reinterpret_cast<uint64_t>(objects);
        exec.buffer_count = 2;
        exec.batch_len = idw::kHaswellNoopBatchLength;
        exec.flags = I915_EXEC_RENDER;
        exec.rsvd1 = context_id;
        if (drmIoctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &exec) != 0) {
            fail("pinned context execbuffer2");
            return false;
        } else if (objects[0].offset != RESOURCE_GPUVA ||
                   objects[1].offset != BATCH_GPUVA) {
            std::cerr << "FAIL: i915 did not preserve the requested GPU virtual addresses\n";
            ok = false;
            return false;
        } else {
            resource_gpuva = objects[0].offset;
            batch_gpuva = objects[1].offset;
            int ret = gem_resources.record_exec_binding(
                resource_handle, context_id, RESOURCE_GPUVA,
                objects[0].offset);
            if (!ret)
                ret = gem_resources.record_exec_binding(
                    batch_handle, context_id, BATCH_GPUVA,
                    objects[1].offset);
            if (ret) {
                fail_backend("track i915 context GPU addresses", ret);
                return false;
            }
            drm_i915_gem_wait wait{};
            wait.bo_handle = batch_handle;
            wait.timeout_ns = 5LL * 1000 * 1000 * 1000;
            if (drmIoctl(fd, DRM_IOCTL_I915_GEM_WAIT, &wait) != 0) {
                fail("GPU completion wait");
                return false;
            }
            idw::I915ResourceSnapshot activity{};
            ret = gem_resources.query_activity(resource_handle, &activity);
            if (ret) {
                fail_backend("resource GEM_BUSY query", ret);
                return false;
            }
            resource_busy_observed = activity.busy;
        }
        return true;
    };

    if (ok && bind_and_submit())
        destroy_context("context destroy before backing advice");

    bool backing_purged = false;
    if (ok) {
        idw::I915ResourceSnapshot state{};
        int ret = gem_resources.advise_discardable(resource_handle, &state);
        if (ret) {
            fail_backend("resource DONTNEED", ret);
        } else {
            backing_purged = state.backing == idw::I915BackingState::Purged;
            ret = gem_resources.advise_willneed(resource_handle, &state);
            if (ret)
                fail_backend("resource WILLNEED after DONTNEED", ret);
            else if (state.backing == idw::I915BackingState::Purged)
                backing_purged = true;
        }
    }

    if (ok && backing_purged) {
        const int ret = gem_resources.destroy(resource_handle);
        if (ret) {
            fail_backend("close purged resource", ret);
        } else {
            resource_handle = 0;
            const int create_ret = gem_resources.create(PAGE_SIZE,
                                                        resource_handle);
            if (create_ret) {
                fail_backend("recreate purged resource", create_ret);
            } else {
                if (mark_willneed(resource_handle, "replacement resource WILLNEED")) {
                    drm_i915_gem_pwrite write{};
                    write.handle = resource_handle;
                    write.size = sizeof(data);
                    write.data_ptr = reinterpret_cast<uint64_t>(data);
                    if (drmIoctl(fd, DRM_IOCTL_I915_GEM_PWRITE, &write) != 0)
                        fail("restore purged resource contents");
                }
            }
        }
    }

    if (ok) {
        uint32_t readback[DATA_SIZE / sizeof(uint32_t)]{};
        drm_i915_gem_pread read{};
        read.handle = resource_handle;
        read.size = sizeof(readback);
        read.data_ptr = reinterpret_cast<uint64_t>(readback);
        if (drmIoctl(fd, DRM_IOCTL_I915_GEM_PREAD, &read) != 0) {
            fail("resource readback");
        } else if (std::memcmp(data, readback, sizeof(data)) != 0) {
            std::cerr << "FAIL: resource contents did not survive backing advice/recreation\n";
            ok = false;
        }
    }

    if (ok && !bind_and_submit()) ok = false;
    if (context_created) destroy_context("context destroy");

    const int close_ret = gem_resources.close_all();
    if (close_ret)
        fail_backend("GEM_CLOSE tracked resources", close_ret);
    if (gem_resources.resource_count()) {
        const int retry_ret = gem_resources.close_all();
        if (retry_ret)
            fail_backend("retry GEM_CLOSE tracked resources", retry_ret);
    }
    ::close(fd);

    if (!ok) return 4;
    std::cout << "PASS: i915 bound Haswell context objects at GPU VAs 0x"
              << std::hex << resource_gpuva << " and 0x" << batch_gpuva << std::dec
              << ", released the context VM, processed DONTNEED/WILLNEED, and rebound at the same VAs with completed no-op submissions.\n"
              << "Backing purge observed: " << (backing_purged ? "yes" : "no") << "\n"
              << "i915 GEM_BUSY activity sample: " << (resource_busy_observed ? "busy" : "idle") << " after completion wait.\n"
              << "Boundary: i915 owns PPGTT mappings and residency/placement. IDW tracks only WILLNEED/DONTNEED backing-retention advice, discardability, the GEM_BUSY in-flight activity sample, and GPU addresses returned by i915 execbuffer. DONTNEED permits purge under memory pressure; if backing is purged, the test recreates the GEM object. GEM_BUSY is not a residency query. IDW does not program page tables.\n";
    return 0;
}

static void usage(const char* p) {
    std::cout << "IDW — Intel GPU inspection and DRM smoke test\n"
              << "Usage:\n  " << p << " info [--json]\n  " << p << " selftest\n  " << p << " gem-resource-test\n  " << p << " memory-test [count] [size-bytes]\n  " << p << " resource-test [count] [size-bytes]\n  " << p << " dmabuf-test\n  " << p << " gpu-submit [count]\n  " << p << " gpu-resource-test\n  " << p << " gpu-memory-test\n  " << p << " kernel\n  " << p << " --help\n";
}
int main(int argc,char** argv) {
    if(argc < 2 || std::string(argv[1])=="--help" || std::string(argv[1])=="-h") { usage(argv[0]); return 0; }
    const std::string cmd=argv[1];
    if(cmd=="info") {
        if (argc > 3 || (argc == 3 && std::string(argv[2]) != "--json")) {
            usage(argv[0]);
            return 1;
        }
        print_info(argc == 3);
        return 0;
    }
    if(cmd=="selftest") {
        if (argc != 2) { usage(argv[0]); return 1; }
        return gem_selftest();
    }
    if(cmd=="gem-resource-test") {
        if (argc != 2) { usage(argv[0]); return 1; }
        return gem_resource_backend_test();
    }
    if(cmd=="memory-test") {
        unsigned count = 8;
        uint64_t size = 65536;
        if (argc > 4 || (argc > 2 && !parse_count(argv[2], count)) ||
            (argc > 3 && !parse_u64_decimal(argv[3], size))) {
            usage(argv[0]);
            return 1;
        }
        return gem_memory_lifecycle_test(count, size);
    }
    if(cmd=="resource-test") {
        unsigned count = 8;
        uint64_t size = 65536;
        if (argc > 4 || (argc > 2 && !parse_count(argv[2], count)) ||
            (argc > 3 && !parse_u64_decimal(argv[3], size))) {
            usage(argv[0]);
            return 1;
        }
        return idw_resource_test(count, size);
    }
    if(cmd=="dmabuf-test") {
        if (argc != 2) { usage(argv[0]); return 1; }
        return dmabuf_import_test();
    }
    if(cmd=="gpu-submit") {
        unsigned count = 1;
        if (argc > 3 || (argc > 2 && !parse_count(argv[2], count))) {
            usage(argv[0]);
            return 1;
        }
        return gpu_submit_test(count);
    }
    if(cmd=="gpu-resource-test") {
        if (argc != 2) { usage(argv[0]); return 1; }
        return gpu_resource_test();
    }
    if(cmd=="gpu-memory-test") {
        if (argc != 2) { usage(argv[0]); return 1; }
        return gpu_memory_test();
    }
    if(cmd=="kernel") {
        if (argc != 2) { usage(argv[0]); return 1; }
        return kernel_status();
    }
    usage(argv[0]); return 1;
}
