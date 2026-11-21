/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IDW_DMA_TEST_UAPI_H
#define IDW_DMA_TEST_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * Test-only interface exposed by the QEMU EDU DMA module. It is not a stable
 * IDW UAPI and must not be used by the production driver.
 */
#define IDW_DMA_TEST_DEVICE "/dev/idw-edu-dma"
#define IDW_DMA_TEST_IOCTL_MAGIC 0xd9
#define IDW_DMA_TEST_MAX_SEGMENTS 64u
#define IDW_DMA_TEST_PT_PGOFF_BASE (1ULL << 32)

enum idw_dma_test_direction {
	IDW_DMA_TEST_BIDIRECTIONAL = 1,
	IDW_DMA_TEST_TO_DEVICE = 2,
	IDW_DMA_TEST_FROM_DEVICE = 3,
};

struct idw_dma_test_alloc {
	__u32 size;
	__u32 direction;
	__u64 bytes;
	__u64 cookie;
	__u64 device_cookie;
	__u64 generation;
	__u32 page_count;
	__u32 reserved;
};

struct idw_dma_test_free {
	__u32 size;
	__u32 reserved;
	__u64 cookie;
};

struct idw_dma_test_segment {
	__u64 dma_address;
	__u64 bytes;
	__u64 device_cookie;
	__u64 generation;
	__u64 mapping_cookie;
};

struct idw_dma_test_segments {
	__u32 size;
	__u32 capacity;
	__u64 cookie;
	__u64 offset;
	__u64 bytes;
	__u64 segments;
	__u32 count;
	__u32 reserved;
};

struct idw_dma_test_pt {
	__u32 size;
	__u32 reserved;
	__u64 cookie;
	__u64 dma_address;
	__u64 device_cookie;
	__u64 generation;
	__u64 bytes;
};

struct idw_dma_test_sync {
	__u32 size;
	__u32 direction;
	__u64 cookie;
	__u64 offset;
	__u64 bytes;
};

#define IDW_DMA_TEST_IOCTL_ALLOC \
	_IOWR(IDW_DMA_TEST_IOCTL_MAGIC, 0x00, struct idw_dma_test_alloc)
#define IDW_DMA_TEST_IOCTL_FREE \
	_IOWR(IDW_DMA_TEST_IOCTL_MAGIC, 0x01, struct idw_dma_test_free)
#define IDW_DMA_TEST_IOCTL_SEGMENTS \
	_IOWR(IDW_DMA_TEST_IOCTL_MAGIC, 0x02, struct idw_dma_test_segments)
#define IDW_DMA_TEST_IOCTL_ALLOC_PT \
	_IOWR(IDW_DMA_TEST_IOCTL_MAGIC, 0x03, struct idw_dma_test_pt)
#define IDW_DMA_TEST_IOCTL_FREE_PT \
	_IOWR(IDW_DMA_TEST_IOCTL_MAGIC, 0x04, struct idw_dma_test_pt)
#define IDW_DMA_TEST_IOCTL_SYNC \
	_IOWR(IDW_DMA_TEST_IOCTL_MAGIC, 0x05, struct idw_dma_test_sync)
#define IDW_DMA_TEST_IOCTL_SYNC_PT \
	_IOWR(IDW_DMA_TEST_IOCTL_MAGIC, 0x06, struct idw_dma_test_pt)

#endif
