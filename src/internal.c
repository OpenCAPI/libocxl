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
#include "libocxl_info.h"
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
#include <strings.h>
#include <stdlib.h>

/// The base sysfs path for OCXL devices
const char *sys_path = NULL;

/// The base device path for OCXL devices
const char *dev_path = NULL;

bool libocxl_inited = false;
pthread_mutex_t libocxl_inited_mutex = PTHREAD_MUTEX_INITIALIZER;

bool verbose_errors = false;
bool verbose_errors_all = false;
bool tracing = false;
bool tracing_all = false;

void ocxl_default_error_handler(ocxl_err error, const char *message);
void (*error_handler)(ocxl_err error, const char *message) = ocxl_default_error_handler;

pthread_mutex_t stderr_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Executed on first afu_open
 *  - Check the LIBOCXL_INFO environment variable and output the info string
 *  - Check the LIBOCXL_TRACE_ALL environment variable and enable tracing_all
 *  - Check the LIBOCXL_VERBOSE_ERRORS_ALL environment variable and enable verbose_errors_all
 */
void libocxl_init()
{
	const char *val;

	pthread_mutex_lock(&libocxl_inited_mutex);
	if (libocxl_inited) {
		pthread_mutex_unlock(&libocxl_inited_mutex);
		return;
	}

	val = getenv("LIBOCXL_INFO");
	if (val && (!strcasecmp(val, "yes") || !strcmp(val, "1"))) {
		fprintf(stderr, "%s\n", libocxl_info);
	}

	val = getenv("LIBOCXL_TRACE_ALL");
	if (val && (!strcasecmp(val, "yes") || !strcmp(val, "1"))) {
		tracing_all = true;
		tracing = true;
	}

	val = getenv("LIBOCXL_VERBOSE_ERRORS_ALL");
	if (val && (!strcasecmp(val, "yes") || !strcmp(val, "1"))) {
		verbose_errors_all = true;
		verbose_errors = true;
	}

	libocxl_inited = true;

	pthread_mutex_unlock(&libocxl_inited_mutex);
}


/**
 * Output a trace message
 * @param file the source filename
 * @param line the source line
 * @param function the function name
 * @param label the label for the message
 * @param format a printf format & args
 */
__attribute__ ((format (printf, 5, 6)))
void trace_message(const char *label, const char *file, int line,
                   const char *function, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);

	pthread_mutex_lock(&stderr_mutex);

	fprintf(stderr, "%s: %s:%d\t%s():\t\t", label, file, line, function);
	vfprintf(stderr, format, ap);
	fprintf(stderr, "\n");

	pthread_mutex_unlock(&stderr_mutex);

	va_end(ap);
}

#define MAX_MESSAGE_LENGTH 255

/**
 * Output an error message
 * @param afu the AFU the error message originated from, or NULL if there was none
 * @param error the error value associated with this message
 * @param format a printf format & args
 */
__attribute__ ((format (printf, 3, 4)))
void errmsg(ocxl_afu *afu, ocxl_err error, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);

	if (afu) {
		if (afu->verbose_errors) {
			char buf[MAX_MESSAGE_LENGTH];

			vsnprintf(buf, sizeof(buf), format, ap);
			buf[sizeof(buf) - 1] = '\0';

			ocxl_afu_h afu_handle = (ocxl_afu_h) afu;
			afu->error_handler(afu_handle, error, buf);
		}
	} else {
		if (verbose_errors) {
			char buf[MAX_MESSAGE_LENGTH];

			vsnprintf(buf, sizeof(buf), format, ap);
			buf[sizeof(buf) - 1] = '\0';

			error_handler(error, buf);
		}
	}

	va_end(ap);
}

/**
 * Print an error message on stderr
 * @param error the error value
 * @param message the message
 */
void ocxl_default_error_handler(ocxl_err error, const char *message)
{
	pthread_mutex_lock(&stderr_mutex);
	fprintf(stderr, "ERROR: %s: %s\n", ocxl_err_to_string(error), message);
	pthread_mutex_unlock(&stderr_mutex);
}

/**
 * Print an error message on stderr
 * @param afu the AFU the error message originated from
 * @param error the error value
 * @param message the message
 */
void ocxl_default_afu_error_handler(ocxl_afu_h afu, ocxl_err error, const char *message)
{
	const char *dev = ocxl_afu_get_device_path(afu);

	pthread_mutex_lock(&stderr_mutex);
	fprintf(stderr, "ERROR: %s\t%s: %s\n", dev ? dev : "No AFU", ocxl_err_to_string(error), message);
	pthread_mutex_unlock(&stderr_mutex);
}

/**
 * Grow a buffer geometrically
 *
 * @param afu the AFU that owns the buffer
 * @param buffer [in/out] the buffer to grow
 * @param [in/out] the number of elements in the buffer
 * @param size the size of a buffer element
 * @param initial_count the initial number of elements in the buffer
 */
ocxl_err grow_buffer(ocxl_afu *afu, void **buffer, uint16_t *count, size_t size, size_t initial_count)
{
	size_t new_count = (*count > 0) ? 2 * *count : initial_count;
	void *temp = realloc(*buffer, new_count * size);
	if (temp == NULL) {
		ocxl_err rc = OCXL_NO_MEM;
		errmsg(afu, rc, "Could not realloc buffer to %lu elements of %lu bytes (%lu bytes total): %d '%s'",
		       new_count, size, new_count * size, errno, strerror(errno));
		return rc;
	}

	*buffer = temp;
	*count = new_count;

	return OCXL_OK;
}
