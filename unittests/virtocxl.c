/*
 * Copyright 2017 International Business Machines
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "libocxl_internal.h"
#include <fuse/cuse_lowlevel.h>
#include <fuse/fuse_lowlevel.h>
#include <linux/poll.h>
#include <misc/ocxl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>

typedef struct ocxl_kernel_event_header ocxl_kernel_event_header;
typedef struct ocxl_kernel_event_xsl_fault_error ocxl_kernel_event_xsl_fault_error;
#define KERNEL_EVENT_SIZE (sizeof(ocxl_kernel_event_header) + sizeof(ocxl_kernel_event_xsl_fault_error))

ocxl_kernel_event_xsl_fault_error translation_fault = { .addr = 0 };
bool afu_attached = false;
const char *sysfs_path = NULL;

static uint8_t version_major = 5;
static uint8_t version_minor = 10;
static size_t _global_mmio_size = 0;
static size_t _pp_mmio_size = 0;


static void afu_open(fuse_req_t req, struct fuse_file_info *fi)
{
	afu_attached = false;
	fuse_reply_open(req, fi);
}

static void afu_read(fuse_req_t req, size_t size, off_t off, __attribute__((unused)) struct fuse_file_info *fi)
{
	char buf[KERNEL_EVENT_SIZE];
	ocxl_kernel_event_header header = {
			.type = OCXL_AFU_EVENT_XSL_FAULT_ERROR,
			.flags = OCXL_KERNEL_EVENT_FLAG_LAST,
	};

	if (0 != off) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	if (translation_fault.addr == 0) {
		fuse_reply_err(req, EAGAIN);
		return;
	}

	if (size < KERNEL_EVENT_SIZE) {
		fuse_reply_buf(req, buf, 0);
		return;
	}

	memcpy(buf, &header, sizeof(header));
	memcpy(buf + sizeof(header), &translation_fault, sizeof(translation_fault));

	fuse_reply_buf(req, buf, size);

	translation_fault.addr = 0;
}

static void afu_ioctl(fuse_req_t req, int cmd, __attribute__((unused)) void *arg,
		__attribute__((unused)) struct fuse_file_info *fi, __attribute__((unused)) unsigned flags,
		__attribute__((unused)) const void *in_buf, __attribute__((unused)) size_t in_bufsz,
		__attribute__((unused)) size_t out_bufsz)
{
	struct ocxl_ioctl_metadata ret;

	switch (cmd) {
	case OCXL_IOCTL_ATTACH:
		afu_attached = true;
		fuse_reply_ioctl(req, 0, NULL, 0);
		break;

	case OCXL_IOCTL_GET_METADATA:
		memset(&ret, 0, sizeof(ret));

		ret.version = 1;

		ret.afu_version_major = version_major;
		ret.afu_version_minor = version_minor;
		ret.pasid = 1234;
		ret.pp_mmio_size = _pp_mmio_size;
		ret.global_mmio_size = _global_mmio_size;

		fuse_reply_ioctl(req, 0, &ret, sizeof(ret));

		break;

	default:
		fuse_reply_err(req, EINVAL);
	}
}

static void afu_poll (fuse_req_t req, __attribute__((unused)) struct fuse_file_info *fi,
		__attribute__((unused)) struct fuse_pollhandle *ph)
{
	if (translation_fault.addr != 0) {
		fuse_reply_poll(req, POLLIN | POLLRDNORM);
	} else if (!afu_attached) {
		fuse_reply_poll(req, POLLERR);
	}

	fuse_reply_poll(req, 0);
}

#define DEVICE_NAME_MAX 64
#define BUF_SIZE 1024

typedef struct afu_thread_info {
	char device_name[DEVICE_NAME_MAX];
	struct cuse_info cuse;
	struct cuse_lowlevel_ops afu_ops;
} afu_thread_info;

afu_thread_info afu_info;
struct fuse_session *afu_session = NULL;

static void *start_afu_thread(void *arg) {
	afu_thread_info *info = (afu_thread_info *)arg;

	char dev_name[PATH_MAX+9] = "DEVNAME=";
	const char *dev_info_argv[] = { dev_name };
	strncat(dev_name, info->device_name, sizeof(dev_name) - 9);

	memset(&info->cuse, 0, sizeof(info->cuse));
	info->cuse.dev_major = 0;
	info->cuse.dev_minor = 0;
	info->cuse.dev_info_argc = 1;
	info->cuse.dev_info_argv = dev_info_argv;
	info->cuse.flags = 0;

	char *argv[] = {
			"testobj/unittests",
			"-f",
	};

	struct fuse_args args = FUSE_ARGS_INIT(2, argv);

	afu_session = cuse_lowlevel_setup(args.argc, args.argv, &info->cuse, &info->afu_ops, NULL, NULL);
	(void)fuse_session_loop(afu_session);

	return NULL;
}

void stop_afu() {
	fuse_session_exit(afu_session);
}

void term_afu() {
	if (afu_session) {
		fuse_session_destroy(afu_session);
		afu_session = NULL;
	}
}

/**
 * Is the current AFU instance attached
 * @return true if the AFU is attached
 */
bool afu_is_attached() {
	return afu_attached;
}

/**
 * Create a new virtual OCXL device.
 *
 * @param afu_name the name of the AFU
 * @param global_mmio_size the size of the global MMIO area
 * @param per_pasid_mmio_size the size of the per-PASID MMIO area
 *
 * @return the thread for the device, or 0 on error
 */
pthread_t create_ocxl_device(const char *afu_name, size_t global_mmio_size, size_t per_pasid_mmio_size) {
	char sysfs_base[PATH_MAX - 20];
	char tmp[PATH_MAX];
	char buf[BUF_SIZE];

	_global_mmio_size = global_mmio_size;
	_pp_mmio_size = per_pasid_mmio_size;

	snprintf(afu_info.device_name, sizeof(afu_info.device_name), "ocxl-test/%s.0001:00:00.1.0", afu_name);
	snprintf(sysfs_base, sizeof(sysfs_base), "%s/%s.0001:00:00.1.0", SYS_PATH, afu_name);
	struct stat sysfs_stat;

	if (stat(sysfs_base, &sysfs_stat)) {
		if (mkdir(sysfs_base, 0775)) {
			fprintf(stderr, "Could not mkdir '%s': %d: %s\n",
					sysfs_base, errno, strerror(errno));
		}
	}

	// Create global MMIO area file
	snprintf(tmp, sizeof(tmp), "%s/global_mmio_area", sysfs_base);
	int fd = creat(tmp, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
	if (fd < 0) {
		fprintf(stderr, "Could not create global_mmio_area file '%s': %d: %s\n",
				tmp, errno, strerror(errno));
		return 0;
	}

	memset(buf, 0, sizeof(buf));

	for (size_t offset = 0; offset < global_mmio_size; offset += sizeof(buf)) {
		int written = write(fd, buf, sizeof(buf));
		if (written < 0) {
			fprintf(stderr, "Could not write global_mmio_area file '%s': %d: %s\n",
					tmp, errno, strerror(errno));
			return 0;
		}
	}
	close(fd);

	memset(&afu_info.afu_ops, 0, sizeof(afu_info.afu_ops));
	afu_info.afu_ops.open = afu_open;
	afu_info.afu_ops.read = afu_read;
	afu_info.afu_ops.ioctl = afu_ioctl;
	afu_info.afu_ops.poll = afu_poll;

	pthread_t afu_thread = 1;
	if (pthread_create(&afu_thread, NULL, start_afu_thread, &afu_info)) {
		fprintf(stderr, "Could not create AFU thread\n");
		return 0;
	}

	return afu_thread;
}

#ifdef _ARCH_PPC64
/**
 * Force a translation fault, this should cause afu_poll() to register an event, and afu_read() to return the event.
 *
 * @param addr the address of the fault
 * @param dsisr the value of the PPC64 specific DSISR register
 * @param count the number of times the translation fault has triggered an error
 */
void force_translation_fault(void *addr, uint64_t dsisr, uint64_t count) {
	translation_fault.addr = (uint64_t)addr;
	translation_fault.dsisr = dsisr;
	translation_fault.count = count;
}
#else
/**
 * Force a translation fault, this should cause afu_poll() to register an event, and afu_read() to return the event.
 *
 * @param addr the address of the fault
 * @param count the number of times the translation fault has triggered an error
 */
void force_translation_fault(void *addr, uint64_t count) {
	translation_fault.addr = (__u64)addr;
	translation_fault.count = count;
}
#endif
