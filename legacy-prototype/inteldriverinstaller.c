#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "idw_uapi.h"

#define IDW_STAGE_MAGIC "IDWSTG01"

static void print_usage(void)
{
    printf("Usage: inteldriverinstaller.exe [--scan <dir>] [--stage <file>] [--inspect <pe64>] [--help]\n");
}

static void inspect_pe64(const char *path)
{
    FILE *fp = fopen(path, "rb");
    uint8_t hdr[0x100];
    size_t n;
    uint32_t pe_off;

    if (!fp) {
        fprintf(stderr, "[PE64] unable to open %s\n", path);
        return;
    }
    n = fread(hdr, 1, sizeof(hdr), fp);
    fclose(fp);

    if (n < 0x44 || hdr[0] != 'M' || hdr[1] != 'Z') {
        printf("[PE64 Driver Image Inspected] source: %s (non-MZ or short)\n", path);
        return;
    }

    pe_off = (uint32_t)hdr[0x3c] | ((uint32_t)hdr[0x3d] << 8) |
             ((uint32_t)hdr[0x3e] << 16) | ((uint32_t)hdr[0x3f] << 24);
    if (pe_off + 6 <= n && hdr[pe_off] == 'P' && hdr[pe_off + 1] == 'E') {
        uint16_t machine = (uint16_t)hdr[pe_off + 4] | ((uint16_t)hdr[pe_off + 5] << 8);
        printf("[PE64 Driver Image Inspected] source: %s machine: 0x%04x\n", path, machine);
    } else {
        printf("[PE64 Driver Image Inspected] source: %s pe_off: 0x%x\n", path, pe_off);
    }
}

static int write_stage_file(const char *stage_path)
{
    FILE *fp = fopen(stage_path, "wb");
    uint32_t thunk_sig = 0x5744444DU; /* 'WDDM' */
    struct idw_wddm_translate_req req;

    if (!fp) {
        fprintf(stderr, "[Stage] failed to write %s\n", stage_path);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.wddm_ddi_kind = IDW_WDDM_DDI_SUBMIT_RENDER;
    req.irql_level = 0U; /* PASSIVE_LEVEL */
    req.wddm_fence_val = 42ULL;
    req.wddm_alloc_handle = 0xA110C001ULL;

    fwrite(IDW_STAGE_MAGIC, 1, 8, fp);
    fwrite(&thunk_sig, 1, sizeof(thunk_sig), fp);
    fwrite(&req, 1, sizeof(req), fp);
    fclose(fp);

    printf("[Stage] wrote %s (ddi=%u irql=%u fence=%llu)\n",
           stage_path, req.wddm_ddi_kind, req.irql_level, (unsigned long long)req.wddm_fence_val);
    return 0;
}

int main(int argc, char **argv)
{
    const char *scan_dir = NULL;
    const char *stage_path = NULL;
    const char *pe_path = NULL;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--scan") == 0 && i + 1 < argc) {
            scan_dir = argv[++i];
        } else if (strcmp(argv[i], "--stage") == 0 && i + 1 < argc) {
            stage_path = argv[++i];
        } else if (strcmp(argv[i], "--inspect") == 0 && i + 1 < argc) {
            pe_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
    }

    if (scan_dir) {
        printf("[Installer] scan target: %s\n", scan_dir);
    }
    if (pe_path) {
        inspect_pe64(pe_path);
    }
    if (stage_path) {
        if (write_stage_file(stage_path) != 0)
            return 1;
    }
    return 0;
}
