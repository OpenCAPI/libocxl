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

#include "libocxl.h"
#include <stdbool.h>
#include "uthash.h"
#include "linux/usrirq.h"

#define DEBUG(__dbg_tx, __dbg_format, __dbg_args...) \
do {\
        debug(__FILE__, __LINE__, __FUNCTION__, PSTR(__dbg_format), ## __dbg_args); \
} while (0)

void debug(const char *file, int line, const char *function, const char *format, ...);
void errmsg(const char *format, ...);

extern char sys_path[PATH_MAX];
extern char dev_path[PATH_MAX];
extern char irq_path[PATH_MAX];
extern bool verbose_errors;
extern FILE *errmsg_filehandle;

/**
 * @internal
 * Represents an MMIO area from an AFU
 */
typedef struct ocxl_mmio_area {
	char *start; /**< The first addressable byte of the area */
	size_t length; /**< The size of the area in bytes */
	ocxl_endian endianess; /**< The endianess of the MMIO area */
} ocxl_mmio_area;

/**
 * @internal
 * AFU IRQ information
 */
typedef struct ocxl_irq {
	ocxl_irq_h handle; /**< The 64 bit handle for the IRQ */
	struct usrirq_event event; /**< The event descriptor */
	void *addr; /**< The mmaped address of the IRQ page */
	void *info; /**< Additional info to pass to the user */
	UT_hash_handle hh; /**< Required by UTHASH to make this a hash of ocxl_irqs */
} ocxl_irq;

/**
 * @internal
 * Represents an AFU
 */
typedef struct ocxl_afu {
	ocxl_identifier identifier; /**< The physical function, name and index of the AFU */
	char device_path[PATH_MAX + 1];
	char sysfs_path[PATH_MAX + 1];
	int fd;	/**< A file descriptor for operating on the AFU */
	int irq_fd; /**< A file descriptor for AFU IRQs */
	int global_mmio_fd; /**< A file descriptor for accessing the AFU global MMIO area */
	ocxl_mmio_area global_mmio;
	ocxl_mmio_area per_pasid_mmio;
	size_t page_size;
	ocxl_irq *irqs;

#ifdef _ARCH_PPC64
	uint64_t ppc64_amr;
#endif
} ocxl_afu;

bool ocxl_afu_mmio_sizes(ocxl_afu * afu);

#endif				/* _LIBOCXL_INTERNAL_H */
