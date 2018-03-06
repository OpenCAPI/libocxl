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
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <unistd.h>
#include <misc/ocxl.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <glob.h>
#include <ctype.h>

/**
 * @defgroup ocxl_afu_getters OpenCAPI AFU Getters
 *
 * The AFU getter functions provide access to AFU metadata, such as the identifier,
 * paths, and MMIO sizes.
 *
 * These operate on any valid AFU handle, even if it has not been opened.
 *
 * @{
 */

/**
 * Get the PASID for the currently open context
 * @pre ocxl_afu_open() has been successfully called
 * @param afu the AFU instance to get the PASID of
 * @return the PASID
 * @retval UINT32_MAX if the context has not been attached
 */
uint32_t ocxl_afu_get_pasid(ocxl_afu_h afu)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	return my_afu->pasid;
}


/**
 * Get the identifier of the AFU
 *
 * The identifier contains the PCI physical function, AFU name & AFU Index
 *
 * @param afu The AFU to find the identifier of
 * @return the identifier of the AFU
 */
const ocxl_identifier *ocxl_afu_get_identifier(ocxl_afu_h afu)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	return &my_afu->identifier;
}

/**
 * Get the canonical device path of the AFU
 *
 * @param afu The AFU to get the device path of
 * @return the device path, or NULL if the device is invalid
 */
const char *ocxl_afu_get_device_path(ocxl_afu_h afu)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	return my_afu->device_path;
}

/**
 * Get the canonical sysfs path of the AFU
 *
 * @param afu The AFU to get the sysfs path of
 * @return the sysfs path, or NULL if the device is invalid
 */
const char *ocxl_afu_get_sysfs_path(ocxl_afu_h afu)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	return my_afu->sysfs_path;
}

/**
 * Get the version of the AFU
 *
 * @param afu The AFU to get the sysfs path of
 * @param[out] major the major version number
 * @param[out] minor the minor version number
 */
void ocxl_afu_get_version(ocxl_afu_h afu, uint8_t *major, uint8_t *minor)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	*major = my_afu->version_major;
	*minor = my_afu->version_minor;
}

/**
 * @}
 *
 * @defgroup ocxl_afu_messages OpenCAPI AFU messages
 *
 * These functions control messages from libocxl, such as error messages and tracing
 *
 * @{
 */

/**
 * Enable messages from libocxl
 *
 * Error messages, if enabled, are emitted by default on STDERR. This behaviour may be
 * overidden by ocxl_afu_set_error_message_handler().
 *
 * Tracing, if enabled, is always emitted on STDERR. It assists a developer by showing
 * detailed AFU information, as well as MMIO & IRQ interactions between the application
 * and the AFU. It does not show direct accesses to memory from the AFU.
 *
 * @param afu the AFU to enable message on
 * @param sources a bitwise OR of the message sources to enable (OCXL_ERRORS, OCXL_TRACING)
 * @see ocxl_afu_set_error_message_handler()
 */
void ocxl_afu_enable_messages(ocxl_afu_h afu, uint64_t sources)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	my_afu->verbose_errors = !!(sources & OCXL_ERRORS);
	my_afu->tracing = !!(sources & OCXL_TRACING);
}

/**
 * Override the default handler for emitting error messages for an AFU
 *
 * The default error handler emits messages on STDERR, to override this behavior,
 * pass a callback to this function.
 *
 * The callback is responsible for prefixing and line termination.
 *
 * Typical use cases would be redirecting error messages to the application's own
 * logging/reporting mechanisms, and adding additional application-specific context
 * to the error messages.
 *
 * @param afu the AFU to override the error handler for
 * @param handler the new error message handler
 */
void ocxl_afu_set_error_message_handler(ocxl_afu_h afu, void (*handler)(ocxl_afu_h afu, ocxl_err error,
                                        const char *message))
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	my_afu->error_handler = handler;
}

/**
 * @}
 *
 * @defgroup ocxl_afu OpenCAPI AFU Management
 *
 * These functions provide access to open and close the AFU.
 *
 * A typical workflow involves the following:
 * - ocxl_afu_open_from_dev(), ocxl_afu_open() - Open the device by device or name
 * - ocxl_afu_attach() - Attach the device to the process's address space
 * - ocxl_global_mmio_map() - Map the AFU Global MMIO space
 * - ocxl_mmio_map() - Map the Per-PASID MMIO space
 *
 * Subsequently, you will need to write information to the AFU's MMIO space (see ocxl_mmio)
 * and also configure and handle interrupts (see ocxl_irq)
 *
 * Finally, to free the AFU handle, you can use ocxl_afu_close().
 *
 * @{
 */

/**
 * Initialize a new AFU structure
 * @internal
 * @param afu a pointer to the structure to initialize
 */
static void afu_init(ocxl_afu * afu)
{
	memset((char *)afu->identifier.afu_name, '\0', sizeof(afu->identifier.afu_name));
	afu->device_path = NULL;
	afu->sysfs_path = NULL;
	afu->version_major = 0;
	afu->version_minor = 0;
	afu->fd = -1;
	afu->fd_info.type = EPOLL_SOURCE_OCXL;
	afu->fd_info.irq = NULL;
	afu->epoll_fd = -1;
	afu->epoll_events = NULL;
	afu->epoll_event_count = 0;
	afu->global_mmio_fd = -1;

	afu->global_mmio.start = NULL;
	afu->global_mmio.length = 0;
	afu->global_mmio.type = OCXL_GLOBAL_MMIO;

	afu->per_pasid_mmio.start = NULL;
	afu->per_pasid_mmio.length = 0;
	afu->per_pasid_mmio.type = OCXL_PER_PASID_MMIO;

	afu->page_size = sysconf(_SC_PAGESIZE);

	afu->irqs = NULL;
	afu->irq_count = 0;
	afu->irq_max_count = 0;

	afu->mmios = NULL;
	afu->mmio_count = 0;
	afu->mmio_max_count = 0;

	afu->pasid = UINT32_MAX;

	afu->verbose_errors = false;
	afu->error_handler = ocxl_default_afu_error_handler;

	afu->tracing = false;

#ifdef _ARCH_PPC64
	afu->ppc64_amr = 0;
#endif
}

/**
 * Allocate an AFU handle (should be freed with ocxl_afu_close)
 *
 * @param[out] afu_out a pointer to an AFU handle to set
 * @retval OCXL_OK if the AFU was allocated
 * @retval OCXL_NO_MEM if memory could not be allocated
 */
static ocxl_err ocxl_afu_alloc(ocxl_afu_h * afu_out)
{
	ocxl_afu *afu = malloc(sizeof(ocxl_afu));
	if (afu == NULL) {
		ocxl_err rc = OCXL_NO_MEM;
		errmsg(NULL, rc, "Could not allocate %d bytes for AFU", sizeof(ocxl_afu));
		return rc;
	}

	afu_init(afu);

	*afu_out = (ocxl_afu_h) afu;

	return OCXL_OK;
}

static bool device_matches(int dirfd, char *dev_name, dev_t dev)
{
	struct stat sb;

	if (fstatat(dirfd, dev_name, &sb, 0) == -1) {
		return false;
	}

	if (!S_ISCHR(sb.st_mode)) {
		return false;
	}

	return dev == sb.st_rdev;
}

/**
 * Find the matching device for a given device major & minor, populate the AFU accordingly
 * @param dev the device number
 * @param afu the afu to set the name & device paths
 *
 * @retval true if the device was found
 */
static bool populate_metadata(dev_t dev, ocxl_afu * afu)
{
	DIR *dev_dir;
	struct dirent *dev_ent;

	dev_dir = opendir(DEVICE_PATH);

	if (dev_dir == NULL) {
		return false;
	}

	int fd = dirfd(dev_dir);
	do {
		if (!(dev_ent = readdir(dev_dir))) {
			closedir(dev_dir);
			return false;
		}
	} while (!device_matches(fd, dev_ent->d_name, dev));

	char *physical_function = strchr(dev_ent->d_name, '.');
	if (physical_function == NULL) {
		errmsg(NULL, OCXL_INTERNAL_ERROR, "Could not extract physical function from device name '%s', missing initial '.'",
		       dev_ent->d_name);
		return false;
	}
	int afu_name_len = physical_function - dev_ent->d_name;
	if (afu_name_len > AFU_NAME_MAX) {
		errmsg(NULL, OCXL_INTERNAL_ERROR,"AFU name '%-.*s' exceeds maximum length of %d", afu_name_len, dev_ent->d_name);
		return false;
	}

	physical_function++;
	uint16_t domain;
	uint8_t bus, device, function;
	int found = sscanf(physical_function, "%hu:%hhu:%hhu.%hhu.%hhu",
	                   &domain, &bus, &device, &function, &afu->identifier.afu_index);

	if (found != 5) {
		errmsg(NULL, OCXL_INTERNAL_ERROR, "Could not parse physical function '%s', only got %d components", physical_function,
		       found);
		return false;
	}

	memcpy((char *)afu->identifier.afu_name, dev_ent->d_name, afu_name_len);
	((char *)afu->identifier.afu_name)[afu_name_len] = '\0';

	size_t dev_path_len = strlen(DEVICE_PATH) + 1 + strlen(dev_ent->d_name) + 1;
	afu->device_path = malloc(dev_path_len);
	if (NULL == afu->device_path) {
		errmsg(NULL, OCXL_INTERNAL_ERROR, "Could not allocate %llu bytes for device path", dev_path_len);
		return false;
	}
	(void)snprintf(afu->device_path, dev_path_len, "%s/%s", DEVICE_PATH, dev_ent->d_name);

	size_t sysfs_path_len = strlen(SYS_PATH) + 1 + strlen(dev_ent->d_name) + 1;
	afu->sysfs_path = malloc(sysfs_path_len);
	if (NULL == afu->sysfs_path) {
		errmsg(NULL, OCXL_INTERNAL_ERROR, "Could not allocate %llu bytes for sysfs path", sysfs_path_len);
		return false;
	}
	(void)snprintf(afu->sysfs_path, sysfs_path_len, "%s/%s", SYS_PATH, dev_ent->d_name);

	return true;
}

/**
 * Output tracing information for AFU metadata
 *
 * @param afu the AFU to display metadata for
 */
static void trace_metadata(ocxl_afu *afu)
{
	TRACE(afu, "device path=\"%s\"", afu->device_path);
	TRACE(afu, "sysfs path=\"%s\"", afu->sysfs_path);
	TRACE(afu, "AFU Name=\"%s\"", afu->identifier.afu_name);
	TRACE(afu, "AFU Index=%u", afu->identifier.afu_index);
	TRACE(afu, "AFU Version=%u:%u", afu->version_major, afu->version_minor);
	TRACE(afu, "Global MMIO size=%llu", afu->global_mmio.length);
	TRACE(afu, "Per PASID MMIO size=%llu", afu->per_pasid_mmio.length);
	TRACE(afu, "Page Size=%llu", afu->page_size);
	TRACE(afu, "PASID=%lu", afu->pasid);
}

/**
 * Open a context on a closed AFU
 *
 * An AFU can have many contexts, the device can be opened once for each
 * context that is available. A seperate afu handle is required for each context.
 *
 * @param afu the AFU instance we want to open
 * @retval OCXL_OK on success
 * @retval OCXL_NO_DEV if the AFU is invalid
 * @retval OCXL_ALREADY_DONE if the AFU is already open
 * @retval OCXL_NO_MORE_CONTEXTS if maximum number of AFU contexts has been reached
 */
static ocxl_err afu_open(ocxl_afu *afu)
{
	if (afu->fd != -1) {
		return OCXL_ALREADY_DONE;
	}

	ocxl_err rc;

	int fd = open(afu->device_path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		if (errno == ENOSPC) {
			rc = OCXL_NO_MORE_CONTEXTS;
			errmsg(afu, rc, "Could not open AFU device '%s', the maximum number of contexts has been reached: Error %d: %s",
			       afu->device_path, errno, strerror(errno));
			return rc;
		}

		rc = OCXL_NO_DEV;
		errmsg(afu, rc, "Could not open AFU device '%s': Error %d: %s", afu->device_path, errno, strerror(errno));
		return rc;
	}

	afu->fd = fd;

	rc = global_mmio_open(afu);
	if (rc != OCXL_OK) {
		errmsg(afu, rc, "Could not open global MMIO descriptor");
		return rc;
	}

	fd = epoll_create1(EPOLL_CLOEXEC);
	if (fd < 0) {
		ocxl_err rc = OCXL_NO_DEV;
		errmsg(afu, rc, "Could not create epoll descriptor. Error %d: %s",
		       errno, strerror(errno));
		return rc;
	}
	afu->epoll_fd = fd;

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.ptr = &afu->fd_info; // Already set up in afu_init
	if (epoll_ctl(afu->epoll_fd, EPOLL_CTL_ADD, afu->fd, &ev) == -1) {
		ocxl_err rc = OCXL_NO_DEV;
		errmsg(afu, rc, "Could not add device fd %d to epoll fd %d for AFU '%s': %d: '%s'",
		       afu->fd, afu->epoll_fd, afu->identifier.afu_name,
		       errno, strerror(errno));
		return rc;
	}

	struct ocxl_ioctl_metadata metadata;
	if (ioctl(afu->fd, OCXL_IOCTL_GET_METADATA, &metadata)) {
		ocxl_err rc = OCXL_NO_DEV;
		errmsg(afu, rc, "OCXL_IOCTL_GET_METADATA failed %d:%s", errno, strerror(errno));
		return rc;
	}

	if (metadata.version >= 0) {
		afu->version_major = metadata.afu_version_major;
		afu->version_minor = metadata.afu_version_minor;
		afu->per_pasid_mmio.length = metadata.pp_mmio_size;
		afu->global_mmio.length = metadata.global_mmio_size;
		afu->pasid = metadata.pasid;
	}

	if (afu->tracing) {
		trace_metadata(afu);
	}

	return OCXL_OK;
}

/**
 * Get an AFU at the specified device path
 *
 * @param path the path of the AFU
 * @param[out] afu the afu handle
 * @retval OCXL_OK if we have successfully fetched the AFU
 * @retval OCXL_NO_MEM if an out of memory error occurred
 * @retval OCXL_NO_DEV if the device is invalid
 */
static ocxl_err get_afu_by_path(const char *path, ocxl_afu_h * afu)
{
	ocxl_afu_h afu_h;
	ocxl_err rc = ocxl_afu_alloc(&afu_h);
	if (rc != OCXL_OK) {
		*afu = OCXL_INVALID_AFU;
		return rc;
	}

	ocxl_afu *my_afu = (ocxl_afu *) afu_h;

	struct stat dev_stats;
	if (stat(path, &dev_stats)) {
		ocxl_err rc = OCXL_NO_DEV;
		errmsg(NULL, rc, "Could not stat AFU device '%s': Error %d: %s", path, errno, strerror(errno));
		*afu = OCXL_INVALID_AFU;
		return rc;
	}

	if (!populate_metadata(dev_stats.st_rdev, my_afu)) {
		ocxl_err rc = OCXL_NO_DEV;
		errmsg(NULL, rc, "Could not find OCXL device for '%s', major=%d, minor=%d, device expected in '%s'",
		       path, major(dev_stats.st_rdev), minor(dev_stats.st_rdev), DEVICE_PATH);
		*afu = OCXL_INVALID_AFU;
		return rc;
	}

	*afu = afu_h;

	return OCXL_OK;
}

/**
 * Open an AFU at a specified path
 *
 * @param path the path of the AFU
 * @param[out] afu the AFU handle which we will allocate. This should be freed with ocxl_afu_close
 * @retval OCXL_OK if we have successfully fetched the AFU
 * @retval OCXL_NO_MEM if an out of memory error occurred
 * @retval OCXL_NO_DEV if the device is invalid
 * @retval OCXL_NO_MORE_CONTEXTS if maximum number of AFU contexts has been reached
 */
ocxl_err ocxl_afu_open_from_dev(const char *path, ocxl_afu_h * afu)
{
	ocxl_err rc = get_afu_by_path(path, afu);

	if (rc != OCXL_OK) {
		*afu = OCXL_INVALID_AFU;
		return rc;
	}

	rc = afu_open((ocxl_afu *)*afu);
	if (rc != OCXL_OK) {
		ocxl_afu_close(*afu);
		*afu = OCXL_INVALID_AFU;
		return rc;
	}

	return OCXL_OK;
}

/**
 * Open an AFU with a specified name on a specific card/afu index
 *
 * @param name the name of the AFU
 * @param physical_function the PCI physical function of the card (as a string, or NULL for any)
 * @param afu_index the AFU index (or -1 for any)
 * @param[out] afu the AFU handle which we will allocate. This should be freed with ocxl_afu_close
 * @retval OCXL_OK if we have successfully fetched the AFU
 * @retval OCXL_NO_MEM if an out of memory error occurred
 * @retval OCXL_NO_DEV if no valid device was found
 * @retval OCXL_NO_MORE_CONTEXTS if maximum number of AFU contexts has been reached on all matching AFUs
 */
ocxl_err ocxl_afu_open_specific(const char *name, const char *physical_function, int16_t afu_index, ocxl_afu_h * afu)
{
	char pattern[PATH_MAX];
	glob_t glob_data;
	ocxl_err ret = OCXL_INTERNAL_ERROR;
	*afu = OCXL_INVALID_AFU;

	if (afu_index == -1) {
		snprintf(pattern, sizeof(pattern), "%s/%s.%s.*",
		         DEVICE_PATH, name,
		         physical_function ? physical_function : "*");
	} else {
		snprintf(pattern, sizeof(pattern), "%s/%s.%s.%d",
		         DEVICE_PATH, name,
		         physical_function ? physical_function : "*",
		         afu_index);
	}

	int rc = glob(pattern, GLOB_ERR, NULL, &glob_data);
	switch (rc) {
	case 0:
		break;
	case GLOB_NOSPACE:
		ret = OCXL_NO_MEM;
		errmsg(NULL, ret, "No memory for glob while listing AFUs");
		goto end;
	case GLOB_NOMATCH:
		ret = OCXL_NO_DEV;
		errmsg(NULL, ret, "No OCXL devices found in '%s' with pattern '%s'", DEVICE_PATH, pattern);
		goto end;
	default:
		errmsg(NULL, ret, "Glob error %d while listing AFUs", rc);
		goto end;
	}

	for (int dev = 0; dev < glob_data.gl_pathc; dev++) {
		const char *dev_path = glob_data.gl_pathv[dev];
		ret = ocxl_afu_open_from_dev(dev_path, afu);

		switch (ret) {
		case OCXL_OK:
			goto end;

		case OCXL_NO_MORE_CONTEXTS:
			continue;

		default:
			goto end;
		}
	}

end:
	globfree(&glob_data);
	return ret;
}

/**
 * Open an AFU with a specified name
 *
 * @param name the name of the AFU
 * @param[out] afu the AFU handle which we will allocate. This should be freed with ocxl_afu_close
 * @retval OCXL_OK if we have successfully fetched the AFU
 * @retval OCXL_NO_MEM if an out of memory error occurred
 * @retval OCXL_NO_DEV if no valid device was found
 * @retval OCXL_NO_MORE_CONTEXTS if maximum number of AFU contexts has been reached on all matching AFUs
 */
ocxl_err ocxl_afu_open(const char *name, ocxl_afu_h * afu)
{
	return ocxl_afu_open_specific(name, NULL, -1, afu);
}

/**
 * Attach the calling process's memory to an open AFU
 *
 * If specified, also sets the value of the PPC specific PSL AMR, and finally, starts the AFU context.
 *
 * @param afu the AFU to attach
 * @pre the AFU is opened
 * @retval OCXL_OK if the AFU was successful attached
 * @retval OCXL_NO_CONTEXT if the AFU was not opened
 * @retval OCXL_INTERNAL_ERROR if the AFU was unable to attach (check dmesg)
 */
ocxl_err ocxl_afu_attach(ocxl_afu_h afu)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	if (my_afu->fd == -1) {
		return OCXL_NO_CONTEXT;
	}

	struct ocxl_ioctl_attach attach_args;
	memset(&attach_args, '\0', sizeof(attach_args));
#ifdef _ARCH_PPC64
	attach_args.amr = my_afu->ppc64_amr;
#endif

	if (ioctl(my_afu->fd, OCXL_IOCTL_ATTACH, &attach_args)) {
		ocxl_err rc = OCXL_INTERNAL_ERROR;
		errmsg(my_afu, rc, "OCXL_IOCTL_ATTACH failed %d:%s", errno, strerror(errno));
		return rc;
	}

	return OCXL_OK;
}

/**
 * Close an AFU and detach it from the context
 *
 * @param afu a pointer to the AFU handle we want to close
 * @retval OCXL_OK if the AFU was freed
 * @retval OCXL_ALREADY_DONE if the AFU was not open
 * @post All resources associated with the handle are closed and freed, the handle is no longer usable
 */
ocxl_err ocxl_afu_close(ocxl_afu_h afu)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	if (my_afu->fd < 0) {
		return OCXL_ALREADY_DONE;
	}

	for (uint16_t mmio_idx = 0; mmio_idx < my_afu->mmio_count; mmio_idx++) {
		ocxl_mmio_unmap((ocxl_mmio_h)&my_afu->mmios[mmio_idx]);
	}

	if (my_afu->global_mmio_fd) {
		close(my_afu->global_mmio_fd);
		my_afu->global_mmio_fd = -1;
	}

	if (my_afu->irqs) {
		for (uint16_t irq = 0; irq < my_afu->irq_count; irq++) {
			irq_dealloc(my_afu, &my_afu->irqs[irq]);
		}

		free(my_afu->irqs);
		my_afu->irqs = NULL;
		my_afu->irq_count = 0;
		my_afu->irq_max_count = 0;
	}

	if (my_afu->epoll_events) {
		free(my_afu->epoll_events);
		my_afu->epoll_event_count = 0;
	}

	close(my_afu->epoll_fd);
	my_afu->epoll_fd = -1;

	close(my_afu->fd);
	my_afu->fd = -1;

	if (my_afu->device_path) {
		free(my_afu->device_path);
		my_afu->device_path = NULL;
	}

	if (my_afu->sysfs_path) {
		free(my_afu->sysfs_path);
		my_afu->sysfs_path = NULL;
	}

	free(my_afu);

	return OCXL_OK;
}

/**
 * @}
 *
 * @defgroup ocxl_ppc OpenCAPI PowerPC specific functions
 *
 * Platform specific AFU functions for PowerPC
 *
 * @{
 */

#ifdef _ARCH_PPC64
/**
 * Set the PPC64-specific PSL AMR register value for restricting access to the AFU
 *
 * This register is documented in the Power ISA, Book III.
 *
 * This function is not available on other platforms.
 *
 * If used, this function should be called before ocxl_afu_attach()
 *
 * @param afu the AFU handle
 * @param amr the AMR register value
 * @retval OCXL_OK if the value was accepted
 */
ocxl_err ocxl_afu_set_ppc64_amr(ocxl_afu_h afu, uint64_t amr)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	my_afu->ppc64_amr = amr;

	return OCXL_OK;
}
#endif

/**
 * @}
 */
