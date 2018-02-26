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
#include <string.h>
#include <stdlib.h>

/**
 * @defgroup ocxl_setup OpenCAPI Library Setup Functions
 *
 * @{
 */

#ifdef TEST_ENVIRONMENT
/**
 * Set the directory used for the ocxl sysfs dir
 *
 * Defaults to /sys/class/ocxl
 *
 * @param path the new path to use for sysfs
 */
__attribute__ ((used)) static void ocxl_set_sys_path(const char *path)
{
	sys_path = path;
}

/**
 * Set the directory used for the ocxl dev dir
 *
 * Defaults to /dev/ocxl
 *
 * @param path the new path to use for sysfs
 */
__attribute__ ((used)) static void ocxl_set_dev_path(const char *path)
{
	dev_path = path;
}

#endif // TEST_ENVIRONMENT


/**
 * Indicate that we want verbose error messages
 *
 * Error messages will be printed to stderr, or a filehandle specified by ocxl_set_errmsg_filehandle
 *
 * @see ocxl_set_errmsg_filehandle
 *
 * @param verbose true if we want verbose errors
 */
void ocxl_want_verbose_errors(int verbose)
{
	verbose_errors = ! !verbose;

	if (verbose_errors && errmsg_filehandle == NULL) {
		errmsg_filehandle = stderr;
	}
}

/**
 * Indicate that we want tracing messages
 *
 * @param want_tracing true if we want tracing
 */
void ocxl_want_tracing(int want_tracing)
{
	tracing = ! !want_tracing;
}


/**
 * Set which filehandle to use for verbose errors
 *
 * If not called, defaults to stderr.
 *
 * @param handle the filehandle to use
 */
void ocxl_set_errmsg_filehandle(FILE * handle)
{
	errmsg_filehandle = handle;
}

/**
 * @}
 */
