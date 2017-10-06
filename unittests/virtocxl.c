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

#include <fuse/cuse_lowlevel.h>
#include <fuse/fuse_lowlevel.h>
#include <linux/poll.h>
#include "libocxl_internal.h"
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

static void afu_open(fuse_req_t req, struct fuse_file_info *fi)
{
	afu_attached = false;
	fuse_reply_open(req, fi);
}

static void afu_read(fuse_req_t req, size_t size, off_t off, struct fuse_file_info *fi)
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

static void afu_ioctl(fuse_req_t req, int cmd, void *arg,
			  struct fuse_file_info *fi, unsigned flags,
			  const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	switch (cmd) {
	case OCXL_IOCTL_ATTACH:
		afu_attached = true;
		fuse_reply_ioctl(req, 0, NULL, 0);
		break;

	default:
		fuse_reply_err(req, EINVAL);
	}
}

static void afu_poll (fuse_req_t req, struct fuse_file_info *fi, struct fuse_pollhandle *ph)
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
	info->cuse.flags = CUSE_UNRESTRICTED_IOCTL;

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
 * Create a new virtual OCXL device
 * @param afu_name the name of the AFU
 * @param global_mmio_size the size of the global MMIO area
 * @param per_pasid_mmio_size the size of the per-PASID MMIO area
 * @return the thread for the device, or 0 on error
 */
pthread_t create_ocxl_device(const char *afu_name, size_t global_mmio_size, size_t per_pasid_mmio_size) {
	char sysfs_base[PATH_MAX];
	char tmp[PATH_MAX];
	char buf[BUF_SIZE];


	snprintf(afu_info.device_name, sizeof(afu_info.device_name), "ocxl-test/%s.0001:00:00.1.0", afu_name);
	snprintf(sysfs_base, sizeof(sysfs_base), "%s/%s.0001:00:00.1.0", sys_path, afu_name);
	struct stat sysfs_stat;

	if (stat(sysfs_base, &sysfs_stat)) {
		if (mkdir(sysfs_base, 0775)) {
			fprintf(stderr, "Could not mkdir '%s': %d: %s\n",
					sysfs_base, errno, strerror(errno));
		}
	}

	// Create global MMIO size file
	snprintf(tmp, sizeof(tmp), "%s/global_mmio_size", sysfs_base);
	int len = snprintf(buf, sizeof(buf), "%ld\n", global_mmio_size);
	int fd = creat(tmp, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
	if (fd < 0) {
		fprintf(stderr, "Could not create global_mmio_size file '%s': %d: %s\n",
				tmp, errno, strerror(errno));
		return 0;
	}
	int written = write(fd, buf, len);
	if (written < 0) {
		fprintf(stderr, "Could not write global_mmio_size file '%s': %d: %s\n",
				tmp, errno, strerror(errno));
		return 0;
	}
	close(fd);

	// Create AFU version file
	snprintf(tmp, sizeof(tmp), "%s/afu_version", sysfs_base);
	len = snprintf(buf, sizeof(buf), "5:10\n");
	fd = creat(tmp, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
	if (fd < 0) {
		fprintf(stderr, "Could not create afu_version file '%s': %d: %s\n",
				tmp, errno, strerror(errno));
		return 0;
	}

	written = write(fd, buf, len);
	if (written < 0) {
		fprintf(stderr, "Could not write pp_mmio_size file '%s': %d: %s\n",
				tmp, errno, strerror(errno));
		return 0;
	}
	close(fd);

	// Create global MMIO area file
	snprintf(tmp, sizeof(tmp), "%s/global_mmio_area", sysfs_base);
	fd = creat(tmp, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
	if (fd < 0) {
		fprintf(stderr, "Could not create global_mmio_area file '%s': %d: %s\n",
				tmp, errno, strerror(errno));
		return 0;
	}

	memset(buf, 0, sizeof(buf));

	for (size_t offset = 0; offset < global_mmio_size; offset += sizeof(buf)) {
		written = write(fd, buf, sizeof(buf));
		if (written < 0) {
			fprintf(stderr, "Could not write global_mmio_area file '%s': %d: %s\n",
					tmp, errno, strerror(errno));
			return 0;
		}
	}
	close(fd);

	// Create per-PASID MMIO size file
	snprintf(tmp, sizeof(tmp), "%s/pp_mmio_size", sysfs_base);
	len = snprintf(buf, sizeof(buf), "%ld\n", per_pasid_mmio_size);
	fd = creat(tmp, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
	if (fd < 0) {
		fprintf(stderr, "Could not create pp_mmio_size file '%s': %d: %s\n",
				tmp, errno, strerror(errno));
		return 0;
	}
	written = write(fd, buf, len);
	if (written < 0) {
		fprintf(stderr, "Could not write pp_mmio_size file '%s': %d: %s\n",
				tmp, errno, strerror(errno));
		return 0;
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

static void usrirq_open(fuse_req_t req, struct fuse_file_info *fi)
{
	fuse_reply_open(req, fi);
}

static void usrirq_ioctl(fuse_req_t req, int cmd, void *arg,
			  struct fuse_file_info *fi, unsigned flags,
			  const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	switch (cmd) {
	case USRIRQ_ALLOC:
		fuse_reply_ioctl(req, 0, NULL, 0);
		break;

	case USRIRQ_SET_EVENTFD:
		fuse_reply_ioctl(req, 0, NULL, 0);
		break;

	default:
		fuse_reply_err(req, EINVAL);
	}
}

typedef struct usrirq_thread_info {
	char device_name[DEVICE_NAME_MAX];
	struct cuse_info cuse;
	struct cuse_lowlevel_ops usrirq_ops;
} usrirq_thread_info;

usrirq_thread_info usrirq_info;
struct fuse_session *usrirq_session = NULL;

static void *start_usrirq_thread(void *arg) {
	usrirq_thread_info *info = (usrirq_thread_info *)arg;

	char dev_name[PATH_MAX+9] = "DEVNAME=";
	const char *dev_info_argv[] = { dev_name };
	strncat(dev_name, info->device_name, sizeof(dev_name) - 9);

	memset(&info->cuse, 0, sizeof(info->cuse));
	info->cuse.dev_major = 0;
	info->cuse.dev_minor = 0;
	info->cuse.dev_info_argc = 1;
	info->cuse.dev_info_argv = dev_info_argv;
	info->cuse.flags = CUSE_UNRESTRICTED_IOCTL;

	char *argv[] = {
			"testobj/unittests",
			"-f",
	};

	struct fuse_args args = FUSE_ARGS_INIT(2, argv);

	usrirq_session = cuse_lowlevel_setup(args.argc, args.argv, &info->cuse, &info->usrirq_ops, NULL, NULL);
	(void)fuse_session_loop(usrirq_session);

	return NULL;
}

void stop_usrirq() {
	fuse_session_exit(usrirq_session);
}

void term_usrirq() {
	if (usrirq_session) {
		fuse_session_destroy(usrirq_session);
		usrirq_session = NULL;
	}
}

/**
 * Create a new virtual usrirq device
 * @param dev_name the nme of the usrirq device
 * @return the thread for the device, or 0 on error
 */
pthread_t create_usrirq_device(const char *dev_name) {
	snprintf(usrirq_info.device_name, sizeof(afu_info.device_name), dev_name);

	memset(&usrirq_info.usrirq_ops, 0, sizeof(usrirq_info.usrirq_ops));
	usrirq_info.usrirq_ops.open = usrirq_open;
	usrirq_info.usrirq_ops.ioctl = usrirq_ioctl;

	pthread_t usrirq_thread = 1;
	if (pthread_create(&usrirq_thread, NULL, start_usrirq_thread, &usrirq_info)) {
		fprintf(stderr, "Could not create usrirq thread\n");
		return 0;
	}

	return usrirq_thread;
}

#ifdef _ARCH_PPC64
/**
 * Force a translation fault, this should cause afu_poll() to register an event, and afu_read() to return the event
 * @param addr the address of the fault
 * @param dsisr the value of the PPC64 specific DSISR register
 * @param count the number of times the translation fault has triggered an error
 */
void force_translation_fault(void *addr, uint64_t dsisr, uint64_t count) {
	translation_fault.addr = addr;
	translation_fault.dsisr = dsisr;
	translation_fault.count = count;
}
#else
/**
 * Force a translation fault, this should cause afu_poll() to register an event, and afu_read() to return the event
 * @param addr the address of the fault
 * @param count the number of times the translation fault has triggered an error
 */
void force_translation_fault(void *addr, uint64_t count) {
	translation_fault.addr = (__u64)addr;
	translation_fault.count = count;
}
#endif
