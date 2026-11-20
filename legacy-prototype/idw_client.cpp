#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "idw_uapi.h"

namespace {

constexpr char kClientStageMagic[8] = {'I', 'D', 'W', 'S', 'T', 'G', '0', '1'};

struct ClientStagedWddm {
    bool valid{false};
    std::uint32_t thunk_sig{0};
    idw_wddm_translate_req wddm_req{};
    std::vector<std::uint8_t> payload;
};

void print_debug_stats(int fd, const char *label) {
    idw_debug_stats dbg{};
    if (::ioctl(fd, IDW_IOC_QUERY_DEBUG, &dbg) < 0) {
        std::cerr << "  [" << label << "] QUERY_DEBUG failed: "
                  << std::strerror(errno) << "\n";
        return;
    }
    std::cout << "  [" << label << "] ppgtt_seq=" << dbg.ppgtt_tlb_flush_seq
              << " ggtt_seq=" << dbg.ggtt_tlb_flush_seq
              << " fence=" << dbg.completed_fence << "/" << dbg.submitted_fence << "\n";
}

bool load_client_staging(const char *path, ClientStagedWddm &out) {
    out = ClientStagedWddm{};
    if (!path || path[0] == '\0')
        return false;

    constexpr std::size_t kMinBytes =
        sizeof(kClientStageMagic) + sizeof(std::uint32_t) + sizeof(idw_wddm_translate_req);

    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        std::cerr << "  [stage] open failed for " << path << "\n";
        return false;
    }

    const auto sz = ifs.tellg();
    if (sz < 0 || static_cast<std::size_t>(sz) < kMinBytes) {
        std::cerr << "  [stage] file too small (" << sz << " bytes)\n";
        return false;
    }

    std::vector<std::uint8_t> raw(static_cast<std::size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    if (!ifs.read(reinterpret_cast<char *>(raw.data()), sz)) {
        std::cerr << "  [stage] read failed for " << path << "\n";
        return false;
    }

    if (std::memcmp(raw.data(), kClientStageMagic, sizeof(kClientStageMagic)) != 0) {
        std::cerr << "  [stage] bad magic header (expected IDWSTG01)\n";
        return false;
    }

    std::size_t off = sizeof(kClientStageMagic);
    std::memcpy(&out.thunk_sig, raw.data() + off, sizeof(out.thunk_sig));
    off += sizeof(out.thunk_sig);

    std::memcpy(&out.wddm_req, raw.data() + off, sizeof(out.wddm_req));

    if (out.wddm_req.irql_level != 0U && out.wddm_req.irql_level != 2U) {
        std::cerr << "  [stage] invalid irql_level=" << out.wddm_req.irql_level << "\n";
        return false;
    }
    if (out.wddm_req.wddm_ddi_kind < IDW_WDDM_DDI_SUBMIT_RENDER ||
        out.wddm_req.wddm_ddi_kind > IDW_WDDM_DDI_CREATE_ALLOC) {
        std::cerr << "  [stage] invalid wddm_ddi_kind=" << out.wddm_req.wddm_ddi_kind << "\n";
        return false;
    }

    out.payload.insert(out.payload.end(),
                       reinterpret_cast<const std::uint8_t *>(&out.thunk_sig),
                       reinterpret_cast<const std::uint8_t *>(&out.thunk_sig) + sizeof(out.thunk_sig));
    out.payload.insert(out.payload.end(),
                       reinterpret_cast<const std::uint8_t *>(&out.wddm_req.wddm_fence_val),
                       reinterpret_cast<const std::uint8_t *>(&out.wddm_req.wddm_fence_val) +
                           sizeof(out.wddm_req.wddm_fence_val));
    if (out.payload.size() > IDW_MAX_PAYLOAD) {
        out.payload.resize(IDW_MAX_PAYLOAD);
    }

    out.valid = true;
    return true;
}

void run_gtt_map_unmap_test(int fd) {
    std::cout << "[GTT Map / Unmap Test (PPGTT + GGTT)]\n";
    print_debug_stats(fd, "initial");

    idw_gtt_map_req ppgtt_map{};
    ppgtt_map.requested_gpu_va = 0ULL;
    ppgtt_map.size_bytes = 4096ULL;
    ppgtt_map.domain = IDW_GTT_DOMAIN_PPGTT;
    ppgtt_map.flags = IDW_GTT_FLAG_READ | IDW_GTT_FLAG_WRITE;

    if (::ioctl(fd, IDW_IOC_GTT_MAP, &ppgtt_map) < 0) {
        std::cerr << "  PPGTT map failed: rc=-1 errno=" << errno << "\n";
        return;
    }

    std::cout << "  PPGTT map:         gpu_va=0x" << std::hex << ppgtt_map.mapped_gpu_va
              << " iova=0x" << ppgtt_map.mapped_dma_iova
              << std::dec << " tlb_seq=" << ppgtt_map.tlb_flush_seq << "\n";
    print_debug_stats(fd, "after PPGTT map");

    idw_gtt_unmap_req ppgtt_unmap{};
    ppgtt_unmap.mapped_gpu_va = ppgtt_map.mapped_gpu_va;
    ppgtt_unmap.domain = IDW_GTT_DOMAIN_PPGTT;
    if (::ioctl(fd, IDW_IOC_GTT_UNMAP, &ppgtt_unmap) == 0) {
        std::cout << "  PPGTT unmap:       tlb_seq=" << ppgtt_unmap.tlb_flush_seq << "\n";
    }
    print_debug_stats(fd, "after PPGTT unmaps");

    idw_gtt_map_req ggtt_map{};
    ggtt_map.requested_gpu_va = 0ULL;
    ggtt_map.size_bytes = 4096ULL;
    ggtt_map.domain = IDW_GTT_DOMAIN_GGTT;
    ggtt_map.flags = IDW_GTT_FLAG_READ | IDW_GTT_FLAG_WRITE;

    if (::ioctl(fd, IDW_IOC_GTT_MAP, &ggtt_map) < 0) {
        std::cerr << "  GGTT map failed:      rc=-1 errno=" << errno
                  << " (" << std::strerror(errno) << ")\n";
        return;
    }

    std::cout << "  GGTT map:          gpu_va=0x" << std::hex << ggtt_map.mapped_gpu_va
              << " iova=0x" << ggtt_map.mapped_dma_iova
              << std::dec << " tlb_seq=" << ggtt_map.tlb_flush_seq
              << " status=" << ggtt_map.nt_status << "\n";
    print_debug_stats(fd, "after GGTT map");

    idw_gtt_unmap_req ggtt_unmap{};
    ggtt_unmap.mapped_gpu_va = ggtt_map.mapped_gpu_va;
    ggtt_unmap.domain = IDW_GTT_DOMAIN_GGTT;

    if (::ioctl(fd, IDW_IOC_GTT_UNMAP, &ggtt_unmap) < 0) {
        std::cerr << "  GGTT unmap failed:    rc=-1 errno=" << errno
                  << " (" << std::strerror(errno) << ")\n";
    } else {
        std::cout << "  GGTT unmap:        tlb_seq=" << ggtt_unmap.tlb_flush_seq
                  << " status=" << ggtt_unmap.nt_status << "\n";
    }
    print_debug_stats(fd, "after GGTT unmap");
}

void run_child_worker(int child_id, const char *stage_path) {
    int cfd = ::open("/dev/inteldriverwin_onlinux", O_RDWR);
    if (cfd < 0) {
        std::cerr << "  [child " << child_id << "] open failed: "
                  << std::strerror(errno) << "\n";
        _exit(1);
    }

    idw_gtt_map_req ppgtt_map{};
    ppgtt_map.requested_gpu_va = 0ULL;
    ppgtt_map.size_bytes = 4096ULL;
    ppgtt_map.domain = IDW_GTT_DOMAIN_PPGTT;
    ppgtt_map.flags = IDW_GTT_FLAG_READ | IDW_GTT_FLAG_WRITE;

    if (::ioctl(cfd, IDW_IOC_GTT_MAP, &ppgtt_map) < 0) {
        std::cerr << "  [child " << child_id << "] PPGTT map failed: errno="
                  << errno << "\n";
        ::close(cfd);
        _exit(1);
    }

    idw_gtt_map_req ggtt_map{};
    if (child_id == 1) {
        ggtt_map.requested_gpu_va = 0ULL;
        ggtt_map.size_bytes = 4096ULL;
        ggtt_map.domain = IDW_GTT_DOMAIN_GGTT;
        ggtt_map.flags = IDW_GTT_FLAG_READ | IDW_GTT_FLAG_WRITE;
        (void)::ioctl(cfd, IDW_IOC_GTT_MAP, &ggtt_map);

        ClientStagedWddm staged{};
        if (stage_path && load_client_staging(stage_path, staged)) {
            if (::ioctl(cfd, IDW_IOC_WDDM_TRANSLATE, &staged.wddm_req) == 0) {
                std::cout << "  [child 1 stage] translate ok: linux_fence="
                          << staged.wddm_req.linux_dma_fence
                          << " gem_handle=" << staged.wddm_req.drm_gem_handle
                          << " nt_status=" << staged.wddm_req.nt_status << "\n";
            }

            idw_submit_req sub{};
            sub.cmd_buf_uaddr = reinterpret_cast<std::uint64_t>(staged.payload.data());
            sub.cmd_buf_bytes = static_cast<std::uint32_t>(staged.payload.size());
            sub.op_code = IDW_OP_WIN_ESCAPE;
            if (::ioctl(cfd, IDW_IOC_SUBMIT, &sub) == 0) {
                std::cout << "  [child 1 stage] submit ok: fence_seq="
                          << sub.fence_seq << " nt_status=" << sub.nt_status << "\n";
            }
        } else {
            std::vector<std::uint8_t> pkt = {'M', 'U', 'L', 'T', 'I', '_', 'P', '1'};
            idw_submit_req sub{};
            sub.cmd_buf_uaddr = reinterpret_cast<std::uint64_t>(pkt.data());
            sub.cmd_buf_bytes = static_cast<std::uint32_t>(pkt.size());
            sub.op_code = IDW_OP_SUBMIT_CMD;
            (void)::ioctl(cfd, IDW_IOC_SUBMIT, &sub);
        }
    }

    ::usleep(6000);

    idw_debug_stats dbg{};
    if (::ioctl(cfd, IDW_IOC_QUERY_DEBUG, &dbg) == 0) {
        std::cout << "  [child " << child_id << "] ppgtt_va=0x" << std::hex
                  << ppgtt_map.mapped_gpu_va
                  << " ppgtt_iova=0x" << ppgtt_map.mapped_dma_iova
                  << std::dec
                  << " | ppgtt_seq=" << dbg.ppgtt_tlb_flush_seq
                  << " ggtt_seq=" << dbg.ggtt_tlb_flush_seq
                  << " fence=" << dbg.completed_fence << "/" << dbg.submitted_fence
                  << "\n";
    }

    idw_gtt_unmap_req ppgtt_unmap{};
    ppgtt_unmap.mapped_gpu_va = ppgtt_map.mapped_gpu_va;
    ppgtt_unmap.domain = IDW_GTT_DOMAIN_PPGTT;
    (void)::ioctl(cfd, IDW_IOC_GTT_UNMAP, &ppgtt_unmap);

    if (child_id == 1 && ggtt_map.mapped_gpu_va != 0ULL) {
        idw_gtt_unmap_req ggtt_unmap{};
        ggtt_unmap.mapped_gpu_va = ggtt_map.mapped_gpu_va;
        ggtt_unmap.domain = IDW_GTT_DOMAIN_GGTT;
        (void)::ioctl(cfd, IDW_IOC_GTT_UNMAP, &ggtt_unmap);
    }

    ::close(cfd);
    _exit(0);
}

void run_multiprocess_isolation_test(const char *stage_path) {
    std::cout << "[Multi-Process PPGTT Isolation, Shared GGTT & Staged WDDM Test]\n";

    const pid_t p1 = ::fork();
    if (p1 < 0)
        return;
    if (p1 == 0)
        run_child_worker(1, stage_path);

    ::usleep(3000);

    const pid_t p2 = ::fork();
    if (p2 < 0) {
        int st1 = 0;
        ::waitpid(p1, &st1, 0);
        return;
    }
    if (p2 == 0)
        run_child_worker(2, stage_path);

    int status1 = 0, status2 = 0;
    ::waitpid(p1, &status1, 0);
    ::waitpid(p2, &status2, 0);

    std::cout << "  children exited:   p1=" << WEXITSTATUS(status1)
              << " p2=" << WEXITSTATUS(status2) << "\n";
}

} // namespace

int main(int argc, char **argv) {
    const char *stage_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stage") == 0 && i + 1 < argc) {
            stage_path = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cout << "Usage: idw_client [--stage <path>]\n";
            return 0;
        }
    }

    int fd = ::open("/dev/inteldriverwin_onlinux", O_RDWR);
    if (fd < 0) {
        std::cerr << "open /dev/inteldriverwin_onlinux failed: "
                  << std::strerror(errno) << "\n";
        return 1;
    }

    run_gtt_map_unmap_test(fd);
    ::close(fd);

    run_multiprocess_isolation_test(stage_path);
    return 0;
}
