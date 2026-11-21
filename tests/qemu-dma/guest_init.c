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
#include <sys/wait.h>
#include <unistd.h>

static void stop_guest(const char *reason)
{
	fprintf(stderr, "IDW_QEMU_DMA: FAIL: %s (errno=%d)\n", reason, errno);
	fflush(NULL);
	reboot(RB_POWER_OFF);
	_exit(1);
}

static int load_module(const char *path, const char *args)
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
	ret = (int)syscall(SYS_init_module, image, (size_t)st.st_size,
			   args ? args : "");
	free(image);
	return ret;
}

static int cmdline_has(const char *needle)
{
	char buffer[1024];
	int fd;
	ssize_t count;

	fd = open("/proc/cmdline", O_RDONLY);
	if (fd < 0)
		return 0;
	count = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if (count <= 0)
		return 0;
	buffer[count] = '\0';
	return strstr(buffer, needle) != NULL;
}

static int read_owner_count(void)
{
	char buffer[32];
	int fd;
	ssize_t count;

	fd = open("/sys/module/idw_edu_dma_test/parameters/owner_count",
		  O_RDONLY);
	if (fd < 0)
		return -1;
	count = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if (count <= 0)
		return -1;
	buffer[count] = '\0';
	return atoi(buffer);
}

static int run_program(const char *path)
{
        pid_t pid;
        int status;

        pid = fork();
        if (pid < 0)
                return -1;
        if (pid == 0) {
                execl(path, path, "--require-device", (char *)NULL);
                _exit(127);
        }
        if (waitpid(pid, &status, 0) != pid)
                return -1;
        if (!WIFEXITED(status))
                return -1;
        return WEXITSTATUS(status);
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
	int probe_failure_test;
	int i;
	int round;

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

	probe_failure_test = cmdline_has("idw_probe_failure_test=1");

	console = open("/dev/console", O_RDWR);
	if (console >= 0) {
		dup2(console, STDIN_FILENO);
		dup2(console, STDOUT_FILENO);
		dup2(console, STDERR_FILENO);
		if (console > STDERR_FILENO)
			close(console);
	}

	puts("IDW_QEMU_DMA: loading IDW core and QEMU EDU test driver");
	if (load_module("/modules/idw_core.ko", ""))
		stop_guest("load idw_core.ko");
	if (load_module("/modules/idw_edu_dma_test.ko",
			probe_failure_test ? "probe_failure_test=1" : ""))
		stop_guest("load idw_edu_dma_test.ko or bind EDU device");

	if (probe_failure_test) {
		for (round = 0; round < 2; round++) {
			if (round) {
				if (load_module("/modules/idw_edu_dma_test.ko",
						"probe_failure_test=1"))
					stop_guest("reload probe failure test driver");
			}
			for (i = 0; i < 100; i++) {
				if (read_owner_count() == 0)
					break;
				usleep(100000);
			}
			if (read_owner_count() != 0)
				stop_guest("probe failure owner did not recover");
			printf("IDW_QEMU_DMA: probe failure round %d owner recovered\n",
			       round + 1);
			if (syscall(SYS_delete_module, "idw_edu_dma_test", 0))
				stop_guest("unload probe failure test driver");
		}
		puts("IDW_QEMU_DMA: PASS: probe failure cleanup, quarantine, retry, repeated owner release completed");
		fflush(stdout);
		signal_qemu_exit(0x10);
		return 1;
	}

	puts("IDW_QEMU_DMA: running native DMA backend through MemoryManager");
	fflush(stdout);
	if (run_program("/native-dma-backend-test"))
		stop_guest("native DMA backend test");

	puts("IDW_QEMU_DMA: running native DMA fault injection and retry tests");
	fflush(stdout);
	if (run_program("/native-dma-fault-test"))
		stop_guest("native DMA fault test");

	if (syscall(SYS_delete_module, "idw_edu_dma_test", 0))
		stop_guest("unload test driver and run PCI remove");

	puts("IDW_QEMU_DMA: PASS: probe, page-table DMA, 32 DMA cycles, cleanup and remove completed");
	fflush(stdout);
	signal_qemu_exit(0x10);
	return 1;
}
