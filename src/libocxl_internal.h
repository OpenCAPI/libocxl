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

#ifndef _LIBOCXL_INTERNAL_H
#define _LIBOCXL_INTERNAL_H

#include <misc/ocxl.h>
#include "libocxl.h"
#include <stdbool.h>
#include <pthread.h>

/* LibOCXL is only tested with the following compilers:
 * GCC 6
 * GCC 7
 *
 * Use of other compilers may result in the MMIO functions emitting instructions
 * that do not result in a 32/64 bit transfer across the OpenCAPI link.
 *
 * The above compilers have been manually verified to produce the following
 * instructions in the mmio_*_native functions:
 *   lwz/ld
 *   stw/std
 */
#if !defined(__GNUC__) || __GNUC__ < 4
#error LibOCXL is only tested with GCC 4, 5, 6 & 7, remove this error at your own peril
#endif

#define LIKELY(condition) __builtin_expect((condition), 1)
#define UNLIKELY(condition) __builtin_expect((condition), 0)

#define DEBUG(__dbg_format, __dbg_args...) \
do {\
        trace_message("Debug", __FILE__, __LINE__, __FUNCTION__, __dbg_format, ## __dbg_args); \
} while (0)

#define TRACE(afu, __trc_format, __trc_args...) \
do {\
        if (UNLIKELY(afu->tracing)) { \
        	trace_message("Trace", __FILE__, __LINE__, __FUNCTION__, __trc_format, ## __trc_args); \
        } \
} while (0)

#define TRACE_OPEN(__trc_format, __trc_args...) \
do {\
        if (UNLIKELY(tracing)) { \
        	trace_message("Trace", __FILE__, __LINE__, __FUNCTION__, __trc_format, ## __trc_args); \
        } \
} while (0)

extern bool verbose_errors;
extern bool verbose_errors_all;
extern bool tracing;
extern bool tracing_all;
extern void (*error_handler)(ocxl_err error, const char *message);
extern const char *libocxl_info;

typedef struct ocxl_afu ocxl_afu;

void trace_message(const char *label, const char *file, int line, const char *function, const char *format, ...);
void errmsg(ocxl_afu *afu, ocxl_err error, const char *format, ...);
void ocxl_default_error_handler(ocxl_err error, const char *message);
void ocxl_default_afu_error_handler(ocxl_afu_h afu, ocxl_err error, const char *message);
ocxl_err grow_buffer(ocxl_afu *afu, void **buffer, uint16_t *count, size_t size, size_t initial_count);
ocxl_err global_mmio_open(ocxl_afu *afu);

extern const char *sys_path;
#define SYS_PATH_DEFAULT "/sys/class/ocxl"
#define SYS_PATH ((UNLIKELY(sys_path != NULL)) ? sys_path : SYS_PATH_DEFAULT)

extern const char *dev_path;
#define DEV_PATH_DEFAULT "/dev/ocxl"
#define DEVICE_PATH ((UNLIKELY(dev_path != NULL)) ? dev_path : DEV_PATH_DEFAULT)

#define INITIAL_IRQ_COUNT 64
#define INITIAL_MMIO_COUNT 4

/**
 * @internal
 * Represents an MMIO area from an AFU
 */
typedef struct ocxl_mmio_area {
	char *start; /**< The first addressable byte of the area */
	size_t length; /**< The size of the area in bytes */
	ocxl_mmio_type type; /**< The type of the area */
	ocxl_afu *afu; /**< The AFU this MMIO area belongs to */
} ocxl_mmio_area;


struct ocxl_irq;
typedef struct ocxl_irq ocxl_irq;

/**
 * @internal
 *
 * The type of action to be taken upon return from the ocxl_read_afu_event() function
 */
typedef enum ocxl_event_action {
	OCXL_EVENT_ACTION_SUCCESS,	/**< The event read was successful and should be handled */
	OCXL_EVENT_ACTION_FAIL,		/**< The event read failed */
	OCXL_EVENT_ACTION_NONE,		/**< There was no event to read */
	OCXL_EVENT_ACTION_IGNORE,	/**< The event read was successful, but should be ignored */
} ocxl_event_action;


/**
 * @internal
 *
 * Metadata for determining which source triggered an epoll fd
 */
typedef struct epoll_fd_source {
	enum {
		EPOLL_SOURCE_OCXL, /**< Source is the OpenCAPI infrastructure */
		EPOLL_SOURCE_IRQ, /**< Source is an AFU generated IRQ */
	} type;
	union {
		ocxl_irq *irq;
	};
} epoll_fd_source;

/**
 * @internal
 *
 * AFU IRQ information
 */
struct ocxl_irq {
	struct ocxl_ioctl_irq_fd event; /**< The event descriptor */
	uint16_t irq_number; /**< The 0 indexed IRQ number */
	void *addr; /**< The mmapped address of the IRQ page */
	void *info; /**< Additional info to pass to the user */
	epoll_fd_source fd_info; /**< Epoll information for this IRQ */
};



/**
 * @internal
 *
 * Represents an AFU
 */
struct ocxl_afu {
	ocxl_identifier identifier; /**< The physical function, name and index of the AFU */
	char *device_path;
	char *sysfs_path;
	uint8_t version_major;
	uint8_t version_minor;
	int fd;	/**< A file descriptor for operating on the AFU */
	epoll_fd_source fd_info; /**< Epoll information for the main AFU fd */
	int epoll_fd; /**< A file descriptor for AFU IRQs wrapped with epoll */
	struct epoll_event *epoll_events; /**< buffer for epoll return */
	size_t epoll_event_count; /**< number of elements available in the epoll_events buffer */
	int global_mmio_fd; /**< A file descriptor for accessing the AFU global MMIO area */
	ocxl_mmio_area global_mmio;
	ocxl_mmio_area per_pasid_mmio;
	size_t page_size;
	ocxl_irq *irqs;
	uint16_t irq_count; /**< The number of valid IRQs */
	uint16_t irq_max_count; /**< The maximum number of IRQs available */

	ocxl_mmio_area *mmios;
	uint16_t mmio_count; /**< The number of valid MMIO regions */
	uint16_t mmio_max_count; /**< The maximum number of MMIO regions available */

	uint32_t pasid;

	bool verbose_errors;
	void (*error_handler)(ocxl_afu_h afu, ocxl_err error, const char *message);

	bool tracing;
	pthread_mutex_t trace_mutex;

	bool attached;

#ifdef _ARCH_PPC64
	uint64_t ppc64_amr;
#endif
};

void irq_dealloc(ocxl_afu *afu, ocxl_irq *irq);
void libocxl_init();

#endif				/* _LIBOCXL_INTERNAL_H */
