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
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/// The base sysfs path for OCXL devices
char sys_path[PATH_MAX] = "/sys/class/ocxl";

/// The base device path for OCXL devices
char dev_path[PATH_MAX] = "/dev/ocxl";

/// The device path for the usrirq device
char irq_path[PATH_MAX] = "/dev/usrirq";

/// Whether the user wants verbose error messages
bool verbose_errors = false;

/// The filehandle to print verbose error messages to, defaults to stderr
FILE *errmsg_filehandle = NULL;
pthread_mutex_t errmsg_mutex;

/**
 * Output a debug message
 * @param file the source filename
 * @param line the source line
 * @param function the function name
 * @param format a printf format & args
 */
void debug(const char *file, int line, const char *function, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);

	fprintf(stderr, "%s:%d\t%s():\t\t", file, line, function);
	vfprintf(stderr, format, ap);
	fprintf(stderr, "\n");

	va_end(ap);
}

/**
 * Output a warning message
 * @param format a printf format & args
 */
void errmsg(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);

	if (verbose_errors) {
		int rc;
		while ((rc = pthread_mutex_lock(&errmsg_mutex)) == EAGAIN) {
		}

		if (rc == 0) {
			fprintf(errmsg_filehandle, "ERROR: ");
			vfprintf(errmsg_filehandle, format, ap);
			fprintf(errmsg_filehandle, "\n");
			pthread_mutex_unlock(&errmsg_mutex);
		}
	}

	va_end(ap);
}

#define INT_LEN 20

/**
 * Read an unsigned int from sysfs
 *
 * @param path the to read from
 * @param val[out] the value that was read
 * @return true if an error occurred
 */
static bool read_sysfs_uint(const char *path, uint64_t * val)
{
	int fd, len;
	char buf[INT_LEN + 1];

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		errmsg("Could not open '%s': %d: %s", path, errno, strerror(errno));
		return true;
	}
	len = read(fd, buf, sizeof(buf));
	close(fd);

	if (len == -1) {
		errmsg("Could not read '%s': %d: %s", path, errno, strerror(errno));
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
bool ocxl_afu_mmio_sizes(ocxl_afu * afu)
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
