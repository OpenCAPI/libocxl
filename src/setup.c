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
 * Enable messages from libocxl open calls.
 *
 * Subsequent messages
 *
 * Error messages, if enabled, are emitted by default on STDERR. This behaviour may be
 * overidden by ocxl_afu_set_error_message_handler().
 *
 * Tracing, if enabled, is always emitted on STDERR. It assists a developer by showing
 * detailed AFU information, as well as MMIO & IRQ interactions between the application
 * and the AFU. It does not show direct accesses to memory from the AFU.
 *
 * @param sources a bitwise OR of the message sources to enable (OCXL_ERRORS, OCXL_TRACING)
 * @see ocxl_afu_set_error_message_handler()
 */
void ocxl_enable_messages(uint64_t sources)
{
	verbose_errors = !!(sources & OCXL_ERRORS);
	tracing = !!(sources & OCXL_TRACING);
}

/**
 * Override the default handler for emitting error messages from open calls
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
 * @param handler the new error message handler
 */
void ocxl_set_error_message_handler(void (*handler)(ocxl_err error, const char *message))
{
	error_handler = handler;
}

/**
 * Convert an error value to a string
 * @param err the error value
 * @return a string value of the error
 */
const char *ocxl_err_to_string(ocxl_err err)
{
	switch (err) {
	case OCXL_OK:
		return "OK";

	case OCXL_NO_MEM:
		return "No memory";

	case OCXL_NO_CONTEXT:
		return "AFU context not available";

	case OCXL_NO_IRQ:
		return "AFU interrupt not available";

	case OCXL_INTERNAL_ERROR:
		return "Internal error";

	case OCXL_ALREADY_DONE:
		return "Already done";

	case OCXL_OUT_OF_BOUNDS:
		return "Out of bounds";

	case OCXL_NO_MORE_CONTEXTS:
		return "No more contexts";

	default:
		return "Unknown error";
	}
}

/**
 * @}
 */
