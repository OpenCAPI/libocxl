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
#include <linux/usrirq.h>
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
 * @return the device path
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
 * @return the sysfs path
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
 * Get the file descriptor of an opened AFU
 * @param afu The AFU to get the descriptor of
 * @return the file descriptor
 */
int ocxl_afu_get_fd(ocxl_afu_h afu)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	return my_afu->fd;
}

/**
 * Get the size of the global MMIO region for an AFU
 *
 * @param afu the AFU to get the MMIO size of
 * @return the size of the global MMIO region
 */
size_t ocxl_afu_get_global_mmio_size(ocxl_afu_h afu)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	return my_afu->global_mmio.length;
}

/**
 * Get the size of the per-PASID MMIO region for an AFU
 *
 * @param afu the AFU to get the MMIO size of
 * @return the size of the per-PASID MMIO region
 */
size_t ocxl_afu_get_mmio_size(ocxl_afu_h afu)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	return my_afu->per_pasid_mmio.length;
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
 * The ocxl_afu_use() and ocxl_afu_use_from_dev() calls provide wrappers around
 * these calls.
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
	memset(afu->device_path, '\0', sizeof(afu->device_path));
	memset(afu->sysfs_path, '\0', sizeof(afu->sysfs_path));
	afu->version_major = 0;
	afu->version_minor = 0;
	afu->fd = -1;
	afu->fd_info.type = EPOLL_SOURCE_AFU;
	afu->fd_info.irq = NULL;
	afu->irq_fd = -1;
	afu->epoll_fd = -1;
	afu->epoll_events = NULL;
	afu->epoll_event_count = 0;
	afu->global_mmio_fd = -1;

	afu->global_mmio.endianess = OCXL_MMIO_HOST_ENDIAN;
	afu->global_mmio.start = NULL;
	afu->global_mmio.length = 0;

	afu->per_pasid_mmio.endianess = OCXL_MMIO_HOST_ENDIAN;
	afu->per_pasid_mmio.start = NULL;
	afu->per_pasid_mmio.length = 0;

	afu->page_size = sysconf(_SC_PAGESIZE);

	afu->irqs = NULL;
	afu->irq_count = 0;
	afu->irq_size = 0;

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
		errmsg("Could not allocate %d bytes for AFU", sizeof(ocxl_afu));
		return OCXL_NO_MEM;
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
 * Read a buffer from sysfs
 *
 * @param path the to read from
 * @param[out] buf the buffer to populate
 * @param size the size of the buffer
 * @retval -1 if an error occurred
 * @return the amount of buffer that was populated
 *
 */
static int read_sysfs_buf(const char *path, char *buf, size_t size)
{
	int fd, len;

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		errmsg("Could not open '%s': %d: %s", path, errno, strerror(errno));
		return -1;
	}
	len = read(fd, buf, size);
	close(fd);

	if (len == -1) {
		errmsg("Could not read '%s': %d: %s", path, errno, strerror(errno));
		return -1;
	}

	return len;
}

#define INT_LEN 20

/**
 * Read an unsigned int from sysfs
 *
 * @param path the to read from
 * @param[out] val the value that was read
 * @return true if an error occurred
 */
static bool read_sysfs_uint(const char *path, uint64_t * val)
{
	int len;
	char buf[INT_LEN + 1];

	len = read_sysfs_buf(path, buf, sizeof(buf));

	if (len == -1) {
		return true;
	}

	buf[len - 1] = '\0';

	if (!isdigit(buf[0])) {
		errmsg("Contents of '%s' ('%s') does not represent a number", path, buf);
		return true;
	}

	*val = strtoull(buf, NULL, 10);

	return false;
}

/**
 * Populate the AFU MMIO sizes
 *
 * @param afu the afu to get the sizes for
 * @return true if there was an error getting the sizes
 */
static bool mmio_sizes(ocxl_afu * afu)
{
	char path[PATH_MAX + 1];

	uint64_t val;
	int pathlen = snprintf(path, sizeof(path), "%s/pp_mmio_size", afu->sysfs_path);
	if (pathlen >= sizeof(path)) {
		errmsg("Path truncated constructing pp_mmio_size path, base='%s'", afu->sysfs_path);
		return true;
	}
	if (read_sysfs_uint(path, &val)) {
		return true;
	}

	afu->per_pasid_mmio.length = val;

	pathlen = snprintf(path, sizeof(path), "%s/global_mmio_size", afu->sysfs_path);
	if (pathlen >= sizeof(path)) {
		errmsg("Path truncated constructing global_mmio_size path, base='%s'", afu->sysfs_path);
		return true;
	}

	if (read_sysfs_uint(path, &val)) {
		return true;
	}

	afu->global_mmio.length = val;

	return false;
}

/**
 * Populate the AFU version
 *
 * @param afu the AFU to get the version for
 * @return true if there was an error getting the sizes
 */
static bool afu_version(ocxl_afu * afu)
{
	char path[PATH_MAX + 1];

	int pathlen = snprintf(path, sizeof(path), "%s/afu_version", afu->sysfs_path);
	if (pathlen >= sizeof(path)) {
		errmsg("Path truncated constructing afu_version path, base='%s'", afu->sysfs_path);
		return true;
	}

#define AFU_VERSION_SIZE (3+1+3+1)
	char buf[AFU_VERSION_SIZE+1];
	int len;
	if ((len = read_sysfs_buf(path, buf, sizeof(buf))) == -1) {
		return true;
	}
	buf[len] = 0;

	sscanf(buf, "%hhu:%hhu", &afu->version_major, &afu->version_minor);

	return false;
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

	dev_dir = opendir(dev_path);

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
		errmsg("Could not extract physical function from device name '%s', missing initial '.'",
		       dev_ent->d_name);
		return false;
	}
	int afu_name_len = physical_function - dev_ent->d_name;
	if (afu_name_len > AFU_NAME_MAX) {
		errmsg("AFU name '%-.*s' exceeds maximum length of %d", afu_name_len, dev_ent->d_name);
		return false;
	}

	physical_function++;
	uint16_t domain;
	uint8_t bus, device, function;
	int found = sscanf(physical_function, "%hu:%hhu:%hhu.%hhu.%hhu",
	                   &domain, &bus, &device, &function, &afu->identifier.afu_index);

	if (found != 5) {
		errmsg("Could not parse physical function '%s', only got %d components", physical_function, found);
		return false;
	}

	memcpy((char *)afu->identifier.afu_name, dev_ent->d_name, afu_name_len);
	((char *)afu->identifier.afu_name)[afu_name_len] = '\0';

	int length = snprintf(afu->device_path, sizeof(afu->device_path), "%s/%s", dev_path, dev_ent->d_name);
	if (length >= sizeof(afu->device_path)) {
		errmsg("device path truncated for AFU '%s'", afu->identifier.afu_name);
		return false;
	}

	length = snprintf(afu->sysfs_path, sizeof(afu->sysfs_path), "%s/%s", sys_path, dev_ent->d_name);
	if (length >= sizeof(afu->device_path)) {
		errmsg("sysfs path truncated for AFU '%s'", afu->identifier.afu_name);
		return false;
	}

	if (afu_version(afu)) {
		errmsg("Could not fetch AFU version information for afu '%s'", afu->identifier.afu_name);
		return false;
	}

	if (mmio_sizes(afu)) {
		errmsg("Could not fetch MMIO sizes for afu '%s'", afu->identifier.afu_name);
		return false;
	}

	return true;
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

	int fd = open(afu->device_path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		if (errno == ENOSPC) {
			errmsg("Could not open AFU device '%s', the maximum number of contexts has been reached: Error %d: %s",
			       afu->device_path, errno, strerror(errno));
			return OCXL_NO_MORE_CONTEXTS;
		}
		errmsg("Could not open AFU device '%s': Error %d: %s", afu->device_path, errno, strerror(errno));
		return OCXL_NO_DEV;
	}

	afu->fd = fd;

	fd = open(irq_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		errmsg("Could not open user IRQ device '%s' for AFU '%s': Error %d: %s",
		       irq_path, afu->device_path, errno, strerror(errno));
		close(afu->fd);
		return OCXL_NO_DEV;
	}
	afu->irq_fd = fd;

	fd = epoll_create1(EPOLL_CLOEXEC);
	if (fd < 0) {
		errmsg("Could not create epoll descriptor. Error %d: %s",
		       errno, strerror(errno));
		return OCXL_NO_DEV;
	}
	afu->epoll_fd = fd;

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.ptr = &afu->fd_info; // Already set up in afu_init
	if (epoll_ctl(afu->epoll_fd, EPOLL_CTL_ADD, afu->fd, &ev) == -1) {
		errmsg("Could not add device fd %d to epoll fd %d for AFU '%s': %d: '%s'",
		       afu->fd, afu->epoll_fd, afu->identifier.afu_name,
		       errno, strerror(errno));
		return OCXL_NO_DEV;
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
		errmsg("Could not stat AFU device '%s': Error %d: %s", path, errno, strerror(errno));
		*afu = OCXL_INVALID_AFU;
		return OCXL_NO_DEV;
	}

	if (!populate_metadata(dev_stats.st_rdev, my_afu)) {
		errmsg("Could not find OCXL device for '%s', major=%d, minor=%d, device expected in '%s'",
		       path, major(dev_stats.st_rdev), minor(dev_stats.st_rdev), dev_path);
		*afu = OCXL_INVALID_AFU;
		return OCXL_NO_DEV;
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
 * Allocate an array of fixed length strings
 * @param count the number of strings
 * @param length the length of each string (including space for a NULL terminator)
 * @return the array, or NULL if the array could not be allocated
 */
static char **allocate_string_array(size_t count, size_t length)
{
	size_t buf_len = count * sizeof(char *) + count * length;
	char *buf =  malloc(buf_len);
	if (buf == NULL) {
		errmsg("Could not allocate %d bytes for string array", buf_len);
		return NULL;
	}

	char **pointers = (char **)buf;
	char *strings = buf + count * sizeof(char *);

	for (int i = 0; i < count; i++) {
		char * string = strings + (i * length);
		pointers[i] = string;
		string[0] = '\0';
	}

	return pointers;
}

/**
 * Get a list of the physical functions for an AFU
 * @param name the name of the AFU
 * @param[out] physical_functions an array we will allocate containing the physical functions
 * @param[out] count the number of physical functions found
 * @retval OCXL_OK if we have found matching physical functions
 * @retval OCXL_NO_MEM if an out of memory error occurred
 * @retval OCXL_NO_DEV if no valid device was found
 */
static ocxl_err list_physical_functions(const char *name, char ***physical_functions, size_t *count)
{
	char pattern[PATH_MAX];
	char **funcs = NULL;
	ocxl_err ret = OCXL_INTERNAL_ERROR;

	snprintf(pattern, sizeof(pattern), "%s/%s.*.0",
	         dev_path, name);

	glob_t glob_data;
	int rc = glob(pattern, GLOB_ERR, NULL, &glob_data);
	switch (rc) {
	case 0:
		break;
	case GLOB_NOSPACE:
		errmsg("No memory for glob while listing AFUs");
		ret = OCXL_NO_MEM;
		goto end;
	case GLOB_NOMATCH:
		errmsg("No OCXL devices found in '%s' for pattern '%s'", dev_path, pattern);
		ret = OCXL_NO_DEV;
		goto end;
	default:
		errmsg("Glob error %d while listing AFUs", rc);
		goto end;
	}

#define PHYS_FUNC_LEN (4+1+2+12+1+1 + 1) // 0001:00:00.1
	funcs = allocate_string_array(glob_data.gl_pathc, PHYS_FUNC_LEN);
	if (funcs == NULL) {
		errmsg("Could not allocate output physical function array");
		ret = OCXL_NO_MEM;
		goto end;
	}

	for (size_t func = 0; func < glob_data.gl_pathc; func++) {
		char *dev_name = glob_data.gl_pathv[func];

		char *physical_function = strchr(dev_name, '.');
		if (physical_function == NULL) {
			errmsg("Could not identify physical function in device path '%s'",
			       glob_data.gl_pathv[func]);
			goto end;
		}
		physical_function++;

		char *end = strrchr(dev_name, '.');
		if (end == NULL) {
			errmsg("Could not identify end of physical function in device path '%s'",
			       glob_data.gl_pathv[func]);
			goto end;
		}

		size_t len = end - physical_function;
		if (len >= PHYS_FUNC_LEN) {
			errmsg("Excessively long physical function length detected in device path '%s'");
			goto end;
		}
		memcpy(funcs[func], physical_function, len);
		funcs[func][len] = '\0';
	}

	*count = glob_data.gl_pathc;
	*physical_functions = funcs;

	ret = OCXL_OK;

end:
	if (ret != OCXL_OK) {
		if (funcs != NULL) {
			free(funcs);
		}
		*count = 0;
		*physical_functions = NULL;
	}

	globfree(&glob_data);
	return ret;
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
		         dev_path, name,
		         physical_function ? physical_function : "*");
	} else {
		snprintf(pattern, sizeof(pattern), "%s/%s.%s.%d",
		         dev_path, name,
		         physical_function ? physical_function : "*",
		         afu_index);
	}

	int rc = glob(pattern, GLOB_ERR, NULL, &glob_data);
	switch (rc) {
	case 0:
		break;
	case GLOB_NOSPACE:
		errmsg("No memory for glob while listing AFUs");
		ret = OCXL_NO_MEM;
		goto end;
	case GLOB_NOMATCH:
		errmsg("No OCXL devices found in '%s' with pattern '%s'", dev_path, pattern);
		ret = OCXL_NO_DEV;
		goto end;
	default:
		errmsg("Glob error %d while listing AFUs", rc);
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
 * Open an AFU with a specified name on a specific card/afu index
 *
 * @param name the name of the AFU
 * @param card_index the card index (in order of discovery)
 * @param afu_index the AFU index (or -1 for any)
 * @param[out] afu the AFU handle which we will allocate. This should be freed with ocxl_afu_close
 * @retval OCXL_OK if we have successfully fetched the AFU
 * @retval OCXL_NO_MEM if an out of memory error occurred
 * @retval OCXL_NO_DEV if no valid device was found
 * @retval OCXL_NO_MORE_CONTEXTS if maximum number of AFU contexts has been reached on all matching AFUs
 */
ocxl_err ocxl_afu_open_by_id(const char *name, uint8_t card_index, int16_t afu_index, ocxl_afu_h * afu)
{
	char **funcs = NULL;
	size_t func_count;

	ocxl_err ret = list_physical_functions(name, &funcs, &func_count);
	if (ret != OCXL_OK) {
		errmsg("Could not retrieve list of physical functions for AFU '%s'", name);
		return ret;
	}

	if (card_index >= func_count) {
		errmsg("Requested card index %d exceeds maximum detected index of %d for AFU '%s'",
		       card_index, func_count, name);
		ret = OCXL_NO_DEV;
		goto end;
	}

	ret = ocxl_afu_open_specific(name, funcs[card_index], afu_index, afu);

end:
	if (funcs) {
		free(funcs);
	}

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
		return OCXL_INTERNAL_ERROR;
	}

	return OCXL_OK;
}

#ifdef _ARCH_PPC64
/**
 * Open and attach a context on an AFU
 *
 * Opens an AFU, attaches the context to the current process and memory maps the MMIO regions
 *
 * An AFU can have many contexts, the device can be opened once for each
 * context that is available.
 *
 * @see ocxl_afu_open, ocxl_afu_attach, ocxl_global_mmio_map, ocxl_mmio_map
 *
 * @param afu a pointer to the AFU handle we want to open
 * @param amr the value of the PPC64 specific PSL AMR register, may be 0 if this should be left alone
 * @param global_endianess	The endianess of the global MMIO area
 * @param per_pasid_endianess	The endianess of the per-PASID MMIO area
 * @post on success, the AFU will be opened, the context attached, and any MMIO areas available will be mapped
 * @retval OCXL_OK on success
 * @retval OCXL_NO_MEM if we could not allocate memory
 * @retval OCXL_NO_DEV if the AFU is invalid
 * @retval OCXL_NO_MORE_CONTEXTS if maximum number of AFU contexts has been reached
 */
static ocxl_err afu_use(ocxl_afu_h afu, uint64_t amr, ocxl_endian global_endianess, ocxl_endian per_pasid_endianess)
{
	ocxl_err ret;

	ret = afu_open(afu);
	if (ret != OCXL_OK) {
		return ret;
	}

	if (amr) {
		ret = ocxl_afu_set_ppc64_amr(afu, amr);
		if (ret != OCXL_OK) {
			ocxl_afu_close(afu);
			return ret;
		}
	}

	ret = ocxl_afu_attach(afu);
	if (ret != OCXL_OK) {
		ocxl_afu_close(afu);
		return ret;
	}

	ret = ocxl_global_mmio_map(afu, global_endianess);
	if (ret != OCXL_OK) {
		ocxl_afu_close(afu);
		return ret;
	}

	ret = ocxl_mmio_map(afu, per_pasid_endianess);
	if (ret != OCXL_OK) {
		ocxl_afu_close(afu);
		return ret;
	}

	return OCXL_OK;
}

/**
 * Open and attach a context on an AFU, given the path to the AFU device
 *
 * Opens an AFU, attaches the context to the current process and memory maps the MMIO regions
 *
 * An AFU can have many contexts, the device can be opened once for each
 * context that is available.
 *
 * @see ocxl_afu_open, ocxl_afu_attach, ocxl_global_mmio_map, ocxl_mmio_map
 *
 * @param path the path of the AFU device
 * @param[out] afu a pointer to the AFU handle we want to open
 * @param amr the value of the PPC64 specific PSL AMR register, may be 0 if this should be left alone
 * @param global_endianess	The endianess of the global MMIO area
 * @param per_pasid_endianess	The endianess of the per-PASID MMIO area
 * @post on success, the AFU will be opened, the context attached, and any MMIO areas available will be mapped
 * @retval OCXL_OK on success
 * @retval OCXL_NO_MEM if we could not allocate memory
 * @retval OCXL_NO_DEV if the AFU is invalid
 * @retval OCXL_NO_MORE_CONTEXTS if maximum number of AFU contexts has been reached
 */
ocxl_err ocxl_afu_use_from_dev(const char *path, ocxl_afu_h * afu, uint64_t amr,
                               ocxl_endian global_endianess, ocxl_endian per_pasid_endianess)
{
	ocxl_err rc = get_afu_by_path(path, afu);

	if (rc != OCXL_OK) {
		return rc;
	}

	rc = afu_use(*afu, amr, global_endianess, per_pasid_endianess);
	if (rc != OCXL_OK) {
		ocxl_afu_close(*afu);
		*afu = OCXL_INVALID_AFU;
		return rc;
	}

	return OCXL_OK;
}

/**
 * Open and attach a context on an AFU, given the name of the AFU
 *
 * Opens an AFU, attaches the context to the current process and memory maps the MMIO regions
 *
 * An AFU can have many contexts, the device can be opened once for each
 * context that is available.
 *
 * @see afu_open, ocxl_afu_attach, ocxl_global_mmio_map, ocxl_mmio_map
 *
 * @param name the name of the AFU
 * @param[out] afu the AFU handle which we will allocate. This should be freed with ocxl_afu_close
 * @param[out] afu a pointer to the AFU handle we want to open
 * @param amr the value of the PPC64 specific PSL AMR register, may be 0 if this should be left alone
 * @param global_endianess	The endianess of the global MMIO area
 * @param per_pasid_endianess	The endianess of the per-PASID MMIO area
 * @post on success, the AFU will be opened, the context attached, and any MMIO areas available will be mapped
 * @retval OCXL_OK if we have successfully fetched the AFU
 * @retval OCXL_NO_MEM if an out of memory error occurred
 * @retval OCXL_NO_DEV if no valid device was found
 * @retval OCXL_NO_MORE_CONTEXTS if maximum number of AFU contexts has been reached on all matching AFUs
 */
ocxl_err ocxl_afu_use(const char *name, ocxl_afu_h * afu, uint64_t amr,
                      ocxl_endian global_endianess, ocxl_endian per_pasid_endianess)
{
	char pattern[PATH_MAX];
	snprintf(pattern, sizeof(pattern), "%s/%s.*", dev_path, name);
	glob_t glob_data;
	ocxl_err ret = OCXL_INTERNAL_ERROR;
	*afu = OCXL_INVALID_AFU;

	int rc = glob(pattern, GLOB_ERR, NULL, &glob_data);
	switch (rc) {
	case 0:
		break;
	case GLOB_NOSPACE:
		errmsg("No memory for glob while listing AFUs");
		ret = OCXL_NO_MEM;
		goto end;
	case GLOB_NOMATCH:
		errmsg("No OCXL devices found in '%s'", dev_path);
		ret = OCXL_NO_DEV;
		goto end;
	default:
		errmsg("Glob error %d while listing AFUs", rc);
		goto end;
	}

	for (int dev = 0; dev < glob_data.gl_pathc; dev++) {
		const char *dev_path = glob_data.gl_pathv[dev];
		ret = ocxl_afu_use_from_dev(dev_path, afu, amr, global_endianess, per_pasid_endianess);
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

#endif

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

	ocxl_mmio_unmap(afu);
	ocxl_global_mmio_unmap(afu);

	if (my_afu->irqs) {
		for (uint16_t irq = 0; irq < my_afu->irq_count; irq++) {
			irq_dealloc(my_afu, &my_afu->irqs[irq]);
		}

		free(my_afu->irqs);
		my_afu->irqs = NULL;
		my_afu->irq_count = 0;
		my_afu->irq_size = 0;
	}

	if (my_afu->epoll_events) {
		free(my_afu->epoll_events);
		my_afu->epoll_event_count = 0;
	}

	close(my_afu->fd);
	my_afu->fd = -1;

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
