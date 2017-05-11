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

#ifndef _LIBOCXL_H
#define _LIBOCXL_H

#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <sys/select.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * Defines the endianess of an AFU MMIO area
 */
typedef enum {
	OCXL_MMIO_BIG_ENDIAN = 0,		/**< AFU data is big-endian */
	OCXL_MMIO_LITTLE_ENDIAN = 1,	/**< AFU data is little-endian */
	OCXL_MMIO_HOST_ENDIAN = 2,		/**< AFU data is the same endianess as the host */
} ocxl_endian;

#define AFU_NAME_MAX	24 /**< The maximum length of an AFU name */

/**
 * AFU identification information
 */
typedef struct ocxl_identifier {
	uint16_t domain;	/**< The domain from the physical function */
	uint8_t bus;		/**< The bus from the physical function */
	uint8_t device;		/**< The domain from the physical function */
	uint8_t function;	/**< The function from the physical function */
	uint8_t afu_index;	/**< The AFU Index */
	const char afu_name[AFU_NAME_MAX + 1];	/**< The name of the AFU */
} ocxl_identifier;

/**
 * A handle for an AFU
 */
typedef void *ocxl_afu_h;
#define OCXL_INVALID_AFU NULL /**< An invalid AFU handle */

/**
 * A handle for an IRQ on an AFU
 */
typedef uint64_t ocxl_irq_h;
#define OCXL_INVALID_IRQ 0 /**< An invalid IRQ handle */

/**
 * Potential return values from ocxl_* functions
 */
typedef enum {
	OCXL_OK = 0,				/**< The call succeeded */
	OCXL_NO_MEM = 1,			/**< An out of memory error occurred */
	OCXL_NO_DEV = 2,			/**< The OpenCAPI device is not available */
	OCXL_NO_CONTEXT = 3,		/**< The call requires an open context on the AFU */
	OCXL_NO_IRQ = 4,			/**< no further interrupts are available, or the interrupt is invalid */
	OCXL_INTERNAL_ERROR = 5,	/**< an internal error has occurred */
	OCXL_ALREADY_DONE = 6,		/**< The action requested has already been performed */
	OCXL_OUT_OF_BOUNDS = 7,		/**< The action requested falls outside the permitted area */
} ocxl_err;

/* setup.c */
void ocxl_set_sys_path(const char *path);
void ocxl_set_dev_path(const char *path);
void ocxl_set_irq_path(const char *path);
void ocxl_want_verbose_errors(int verbose);
void ocxl_set_errmsg_filehandle(FILE * handle);

/* afu.c */
/* AFU getters */
const ocxl_identifier *ocxl_afu_get_identifier(ocxl_afu_h afu);
const char *ocxl_afu_get_device_path(ocxl_afu_h afu);
const char *ocxl_afu_get_sysfs_path(ocxl_afu_h afu);
int ocxl_afu_get_fd(ocxl_afu_h afu);
size_t ocxl_afu_get_global_mmio_size(ocxl_afu_h afu);
size_t ocxl_afu_get_mmio_size(ocxl_afu_h afu);

/* AFU operations */
ocxl_err ocxl_afu_open_from_dev(const char *path, ocxl_afu_h * afu);
ocxl_err ocxl_afu_close(ocxl_afu_h afu);
void ocxl_afu_free(ocxl_afu_h * afu);
ocxl_err ocxl_afu_open(ocxl_afu_h afu);
ocxl_err ocxl_afu_attach(ocxl_afu_h afu);

/* High level wrappers */
#ifdef _ARCH_PPC64
ocxl_err ocxl_afu_use(ocxl_afu_h afu, uint64_t amr, ocxl_endian global_endianess,
                      ocxl_endian per_pasid_endianess);
ocxl_err ocxl_afu_use_from_dev(const char *path, ocxl_afu_h * afu, uint64_t amr,
                               ocxl_endian global_endianess, ocxl_endian per_pasid_endianess);
#endif

/* irq.c */
/* AFU IRQ functions */
ocxl_err ocxl_afu_irq_alloc(ocxl_afu_h afu, void *info, ocxl_irq_h * irq_handle);
ocxl_err ocxl_afu_irq_free(ocxl_afu_h afu, ocxl_irq_h * irq);
ocxl_err ocxl_afu_irq_set_info(ocxl_afu_h afu, ocxl_irq_h irq, void *info);
ocxl_err ocxl_afu_irq_get_info(ocxl_afu_h afu, ocxl_irq_h irq, void **info);
uint16_t ocxl_afu_irq_check(ocxl_afu_h afu, struct timeval *timeout,
                            const ocxl_irq_h ** triggered_irqs);

/* AFU IRQ Callbacks */
ocxl_err ocxl_afu_irq_attach_callback(ocxl_afu_h afu, ocxl_irq_h irq,
                                      void (*callback)(ocxl_afu_h afu, ocxl_irq_h irq, void *info));
ocxl_err ocxl_afu_irq_detach_callback(ocxl_afu_h afu, ocxl_irq_h irq);
uint16_t ocxl_afu_irq_handle_callbacks(ocxl_afu_h afu, struct timeval *timeout);

/* Platform specific: PPC64 */
#ifdef _ARCH_PPC64
ocxl_err ocxl_afu_set_ppc64_amr(ocxl_afu_h afu, uint64_t amr);
#endif

/* mmio.c */
ocxl_err ocxl_global_mmio_map(ocxl_afu_h afu, ocxl_endian endian);
ocxl_err ocxl_mmio_map(ocxl_afu_h afu, ocxl_endian endian);

ocxl_err ocxl_global_mmio_read32(ocxl_afu_h afu, size_t offset, uint32_t * out);
ocxl_err ocxl_global_mmio_read64(ocxl_afu_h afu, size_t offset, uint64_t * out);
ocxl_err ocxl_global_mmio_write32(ocxl_afu_h afu, size_t offset, uint32_t val);
ocxl_err ocxl_global_mmio_write64(ocxl_afu_h afu, size_t offset, uint64_t val);

ocxl_err ocxl_mmio_read32(ocxl_afu_h afu, size_t offset, uint32_t * out);
ocxl_err ocxl_mmio_read64(ocxl_afu_h afu, size_t offset, uint64_t * out);
ocxl_err ocxl_mmio_write32(ocxl_afu_h afu, size_t offset, uint32_t val);
ocxl_err ocxl_mmio_write64(ocxl_afu_h afu, size_t offset, uint64_t val);

/* MMIO low level API */
void ocxl_global_mmio_unmap(ocxl_afu_h afu);
void ocxl_mmio_unmap(ocxl_afu_h afu);

#ifdef __cplusplus
}
#endif
#endif	/* _LIBOCXL_H */

