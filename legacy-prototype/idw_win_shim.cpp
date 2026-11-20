#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

#include "idw_uapi.h"

namespace {

constexpr char kStageMagic[8] = {'I', 'D', 'W', 'S', 'T', 'G', '0', '1'};

struct StagedRecord {
    bool loaded{false};
    std::uint32_t thunk_sig{0x5744444DU};
    idw_wddm_translate_req wddm_req{};
};

bool read_stage(const char *path, StagedRecord &out) {
    constexpr std::size_t kMinSz =
        sizeof(kStageMagic) + sizeof(std::uint32_t) + sizeof(idw_wddm_translate_req);
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs)
        return false;
    const auto sz = ifs.tellg();
    if (sz < 0 || static_cast<std::size_t>(sz) < kMinSz)
        return false;

    std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    if (!ifs.read(reinterpret_cast<char *>(buf.data()), sz))
        return false;
    if (std::memcmp(buf.data(), kStageMagic, sizeof(kStageMagic)) != 0)
        return false;

    std::size_t off = sizeof(kStageMagic);
    std::memcpy(&out.thunk_sig, buf.data() + off, sizeof(out.thunk_sig));
    off += sizeof(out.thunk_sig);
    std::memcpy(&out.wddm_req, buf.data() + off, sizeof(out.wddm_req));
    out.loaded = true;
    return true;
}

void inspect_pe_source(const char *pe_path) {
    if (!pe_path || pe_path[0] == '\0') {
        std::cout << "[Windows Driver Image Shim] source: <synthetic PE64>\n";
        return;
    }
    std::ifstream ifs(pe_path, std::ios::binary);
    std::uint8_t hdr[0x100]{};
    ifs.read(reinterpret_cast<char *>(hdr), sizeof(hdr));
    if (ifs.gcount() >= 0x44 && hdr[0] == 'M' && hdr[1] == 'Z') {
        const std::uint32_t pe_off =
            static_cast<std::uint32_t>(hdr[0x3c]) |
            (static_cast<std::uint32_t>(hdr[0x3d]) << 8) |
            (static_cast<std::uint32_t>(hdr[0x3e]) << 16) |
            (static_cast<std::uint32_t>(hdr[0x3f]) << 24);
        if (pe_off + 6 <= static_cast<std::size_t>(ifs.gcount()) &&
            hdr[pe_off] == 'P' && hdr[pe_off + 1] == 'E') {
            const std::uint16_t machine =
                static_cast<std::uint16_t>(hdr[pe_off + 4]) |
                (static_cast<std::uint16_t>(hdr[pe_off + 5]) << 8);
            std::cout << "[Windows Driver Image Shim] source: " << pe_path
                      << " machine: 0x" << std::hex << machine << std::dec << "\n";
            return;
        }
    }
    std::cout << "[Windows Driver Image Shim] source: " << pe_path << "\n";
}

} // namespace

int main(int argc, char **argv) {
    const char *stage_path = nullptr;
    const char *pe_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stage") == 0 && i + 1 < argc) {
            stage_path = argv[++i];
        } else if (std::strcmp(argv[i], "--pe") == 0 && i + 1 < argc) {
            pe_path = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cout << "Usage: idw_win_shim [--stage <file>] [--pe <path>]\n";
            return 0;
        }
    }

    inspect_pe_source(pe_path);

    StagedRecord rec{};
    if (stage_path && read_stage(stage_path, rec)) {
        std::cout << "[Loaded Staging Record] sig=0x" << std::hex << rec.thunk_sig
                  << std::dec << " ddi=" << rec.wddm_req.wddm_ddi_kind
                  << " irql=" << rec.wddm_req.irql_level
                  << " fence=" << rec.wddm_req.wddm_fence_val << "\n";
    } else {
        rec.wddm_req.wddm_ddi_kind = IDW_WDDM_DDI_SUBMIT_RENDER;
        rec.wddm_req.irql_level = 0U;
        rec.wddm_req.wddm_fence_val = 99ULL;
        rec.wddm_req.wddm_alloc_handle = 0x55AA0001ULL;
    }

    int fd = ::open("/dev/inteldriverwin_onlinux", O_RDWR);
    if (fd < 0) {
        std::cerr << "open /dev/inteldriverwin_onlinux failed: "
                  << std::strerror(errno) << "\n";
        return 1;
    }

    if (::ioctl(fd, IDW_IOC_WDDM_TRANSLATE, &rec.wddm_req) == 0) {
        std::cout << "[WDDM Translate Result] linux_fence=" << rec.wddm_req.linux_dma_fence
                  << " gem_handle=" << rec.wddm_req.drm_gem_handle
                  << " status=" << rec.wddm_req.nt_status << "\n";
    }

    std::vector<std::uint8_t> buf(sizeof(rec.thunk_sig) + sizeof(rec.wddm_req.wddm_fence_val));
    std::memcpy(buf.data(), &rec.thunk_sig, sizeof(rec.thunk_sig));
    std::memcpy(buf.data() + sizeof(rec.thunk_sig), &rec.wddm_req.wddm_fence_val,
                sizeof(rec.wddm_req.wddm_fence_val));

    idw_submit_req sub{};
    sub.cmd_buf_uaddr = reinterpret_cast<std::uint64_t>(buf.data());
    sub.cmd_buf_bytes = static_cast<std::uint32_t>(buf.size());
    sub.op_code = IDW_OP_WIN_ESCAPE;

    if (::ioctl(fd, IDW_IOC_SUBMIT, &sub) == 0) {
        std::cout << "[Shim Submission & Fence] fence_seq=" << sub.fence_seq
                  << " status=" << sub.nt_status << "\n";
    }

    ::close(fd);
    return 0;
}
