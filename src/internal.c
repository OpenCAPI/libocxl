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

/// True if we want tracing
bool tracing = false;

pthread_mutex_t errmsg_mutex;

/**
 * Output a trace message
 * @param file the source filename
 * @param line the source line
 * @param function the function name
 * @param label the label for the message
 * @param format a printf format & args
 */
__attribute__ ((format (printf, 5, 6)))
void trace_message(const char *label, const char *file, int line, const char *function, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);

	fprintf(stderr, "%s: %s:%d\t%s():\t\t", label, file, line, function);
	vfprintf(stderr, format, ap);
	fprintf(stderr, "\n");

	va_end(ap);
}

/**
 * Output a warning message
 * @param format a printf format & args
 */
__attribute__ ((format (printf, 1, 2)))
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
