/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static void stop_guest(const char *reason)
{
	fprintf(stderr, "IDW_QEMU_DMA: FAIL: %s (errno=%d)\n", reason, errno);
	fflush(NULL);
	reboot(RB_POWER_OFF);
	_exit(1);
}

static int load_module(const char *path)
{
	struct stat st;
	char *image;
	size_t offset = 0;
	int fd;
	int ret;

	fd = open(path, O_RDONLY);
	if (fd < 0 || fstat(fd, &st) || st.st_size <= 0) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	image = malloc((size_t)st.st_size);
	if (!image) {
		close(fd);
		errno = ENOMEM;
		return -1;
	}
	while (offset < (size_t)st.st_size) {
		ssize_t count = read(fd, image + offset,
				     (size_t)st.st_size - offset);
		if (count <= 0) {
			free(image);
			close(fd);
			if (!count)
				errno = EIO;
			return -1;
		}
		offset += (size_t)count;
	}
	close(fd);
	ret = (int)syscall(SYS_init_module, image, (size_t)st.st_size, "");
	free(image);
	return ret;
}

static void signal_qemu_exit(unsigned char code)
{
	int port;

	port = open("/dev/port", O_WRONLY);
	if (port < 0 || lseek(port, 0xf4, SEEK_SET) != 0xf4 ||
	    write(port, &code, sizeof(code)) != sizeof(code))
		stop_guest("write QEMU isa-debug-exit port");
	close(port);
}

int main(void)
{
	int console;

	mkdir("/dev", 0755);
	mkdir("/proc", 0555);
	mkdir("/sys", 0555);
	if (mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, NULL) &&
	    errno != EBUSY)
		stop_guest("mount devtmpfs");
	if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV, NULL) &&
	    errno != EBUSY)
		stop_guest("mount proc");
	if (mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV, NULL) &&
	    errno != EBUSY)
		stop_guest("mount sysfs");

	console = open("/dev/console", O_RDWR);
	if (console >= 0) {
		dup2(console, STDIN_FILENO);
		dup2(console, STDOUT_FILENO);
		dup2(console, STDERR_FILENO);
		if (console > STDERR_FILENO)
			close(console);
	}

	puts("IDW_QEMU_DMA: loading IDW core and QEMU EDU test driver");
	if (load_module("/modules/idw_core.ko"))
		stop_guest("load idw_core.ko");
	if (load_module("/modules/idw_edu_dma_test.ko"))
		stop_guest("load idw_edu_dma_test.ko or bind EDU device");

	if (syscall(SYS_delete_module, "idw_edu_dma_test", 0))
		stop_guest("unload test driver and run PCI remove");

	puts("IDW_QEMU_DMA: PASS: probe, 32 DMA cycles, cleanup and remove completed");
	fflush(stdout);
	signal_qemu_exit(0x10);
	return 1;
}
