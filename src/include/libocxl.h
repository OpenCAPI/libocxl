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

/**
 * @file libocxl.h
 * @brief library functions to implement userspace drivers for OpenCAPI accelerators
 *
 * Define LIBOCXL_LIVE_DANGEROUSLY before including this header to suppress optional
 * compiler warnings, such as warnings on unused return values.
 */

#ifndef _LIBOCXL_H
#define _LIBOCXL_H

#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <sys/select.h>
#include <sys/mman.h>  // Required for PROT_* for MMIO map calls
#include <endian.h> // Required for htobe32 & friends in MMIO access wrappers

#ifdef __cplusplus
extern "C" {
#endif

#define OCXL_NO_MESSAGES 0			/**< No messages requested */
#define OCXL_ERRORS		(1 << 0)	/**< Error messages requested */
#define OCXL_TRACING	(1 << 1)	/**< Tracing requested */


/**
 * Defines the endianness of an AFU MMIO area
 */
typedef enum {
	OCXL_MMIO_BIG_ENDIAN = 0,		/**< AFU data is big-endian */
	OCXL_MMIO_LITTLE_ENDIAN = 1,	/**< AFU data is little-endian */
	OCXL_MMIO_HOST_ENDIAN = 2,		/**< AFU data is the same endianness as the host */
} ocxl_endian;

/**
 * Defines the type of an MMIO area
 */
typedef enum {
	OCXL_GLOBAL_MMIO,
	OCXL_PER_PASID_MMIO
} ocxl_mmio_type;

#define AFU_NAME_MAX	24 /**< The maximum length of an AFU name */

/**
 * AFU identification information
 */
typedef struct ocxl_identifier {
	uint8_t afu_index;	/**< The AFU Index */
	const char afu_name[AFU_NAME_MAX + 1];	/**< The name of the AFU */
} ocxl_identifier;

/**
 * A handle for an AFU
 */
typedef struct ocxl_afu *ocxl_afu_h;

#define OCXL_INVALID_AFU NULL /**< An invalid AFU handle */

/**
 * A handle for an IRQ on an AFU
 */
typedef uint16_t ocxl_irq_h;

/**
 * A handle for an MMIO region on an AFU
 */
typedef struct ocxl_mmio_area *ocxl_mmio_h;


/**
 * Potential return values from ocxl_* functions
 */
typedef enum {
	OCXL_OK = 0,				/**< The call succeeded */
	OCXL_NO_MEM = -1,			/**< An out of memory error occurred */
	OCXL_NO_DEV = -2,			/**< The OpenCAPI device is not available */
	OCXL_NO_CONTEXT = -3,		/**< The call requires an open context on the AFU */
	OCXL_NO_IRQ = -4,			/**< no further interrupts are available, or the interrupt is invalid */
	OCXL_INTERNAL_ERROR = -5,	/**< an internal error has occurred */
	OCXL_ALREADY_DONE = -6,		/**< The action requested has already been performed */
	OCXL_OUT_OF_BOUNDS = -7,	/**< The action requested falls outside the permitted area */
	OCXL_NO_MORE_CONTEXTS = -8, /**< No more contexts can be opened on the AFU */
	OCXL_INVALID_ARGS = -9,		/**< One or more arguments are invalid */
	/* Adding something? Update setup.c: ocxl_err_to_string too */
} ocxl_err;

/**
 * OCXL Event types
 */
typedef enum {
	OCXL_EVENT_IRQ = 0,					/**< An AFU IRQ */
	OCXL_EVENT_TRANSLATION_FAULT = 1,	/**< A memory translation fault occurred on the AFU */
} ocxl_event_type;


/**
 * The data for a triggered IRQ event
 */
typedef struct {
	uint16_t irq; /**< The IRQ number of the AFU */
	uint64_t handle; /**< The 64 bit handle of the triggered IRQ */
	void *info; /**< An opaque pointer associated with the IRQ */
	uint64_t count; /**< The number of times the interrupt has been triggered since last checked */
} ocxl_event_irq;

/**
 * The data for a triggered translation fault error event
 */
typedef struct {
	void *addr; /**< The address that triggered the fault */
#ifdef _ARCH_PPC64
	uint64_t dsisr; /**< The value of the PPC64 specific DSISR (Data storage interrupt status register) */
#endif
	uint64_t count; /**< The number of times this address has triggered the fault */
} ocxl_event_translation_fault;


#ifndef _DOXYGEN_
#if defined(LIBOCXL_LIVE_DANGEROUSLY)
#define LIBOCXL_WARN_UNUSED
#elif defined(__clang__) && __clang_major__ < 3 || __clang_major__ == 3 && __clang_minor__ <= 8
#define LIBOCXL_WARN_UNUSED
#else
#define LIBOCXL_WARN_UNUSED __attribute__ ((warn_unused_result))
#endif
#endif /* _DOXYGEN_ */

/**
 * An OCXL event
 *
 * This may be an AFU interrupt, or a translation error, as determined by ocxl_event.type.
 *
 * Once the type in known, the appropriate member of the anonymous union may be accessed.
 */
typedef struct ocxl_event {
	ocxl_event_type type;
	union {
		ocxl_event_irq irq; /**< Usable only when the type is OCXL_EVENT_IRQ */
		ocxl_event_translation_fault translation_fault; /**< Usable only when the type is OCXL_OCXL_EVENT_TRANSLATION_FAULT */
		uint64_t padding[16];
	};
} ocxl_event;

#define OCXL_ATTACH_FLAGS_NONE (0)

/* setup.c */
void ocxl_enable_messages(uint64_t sources);
void ocxl_set_error_message_handler(void (*handler)(ocxl_err error, const char *message));
const char *ocxl_err_to_string(ocxl_err err) LIBOCXL_WARN_UNUSED;
const char *ocxl_info() LIBOCXL_WARN_UNUSED;

/* afu.c */
/* AFU getters */
const ocxl_identifier *ocxl_afu_get_identifier(ocxl_afu_h afu) LIBOCXL_WARN_UNUSED;
const char *ocxl_afu_get_device_path(ocxl_afu_h afu) LIBOCXL_WARN_UNUSED;
const char *ocxl_afu_get_sysfs_path(ocxl_afu_h afu) LIBOCXL_WARN_UNUSED;
void ocxl_afu_get_version(ocxl_afu_h afu, uint8_t *major, uint8_t *minor);
uint32_t ocxl_afu_get_pasid(ocxl_afu_h afu) LIBOCXL_WARN_UNUSED;

/* AFU operations */
ocxl_err ocxl_afu_open_specific(const char *name, const char *physical_function, int16_t afu_index,
                                ocxl_afu_h *afu) LIBOCXL_WARN_UNUSED;
ocxl_err ocxl_afu_open_from_dev(const char *path, ocxl_afu_h *afu) LIBOCXL_WARN_UNUSED;
ocxl_err ocxl_afu_open(const char *name, ocxl_afu_h *afu) LIBOCXL_WARN_UNUSED;
void ocxl_afu_enable_messages(ocxl_afu_h afu, uint64_t sources);
void ocxl_afu_set_error_message_handler(ocxl_afu_h afu, void (*handler)(ocxl_afu_h afu, ocxl_err error,
                                        const char *message));
ocxl_err ocxl_afu_close(ocxl_afu_h afu);
ocxl_err ocxl_afu_attach(ocxl_afu_h afu, uint64_t flags) LIBOCXL_WARN_UNUSED;

/* irq.c */
/* AFU IRQ functions */
ocxl_err ocxl_irq_alloc(ocxl_afu_h afu, void *info, ocxl_irq_h *irq_handle) LIBOCXL_WARN_UNUSED;
uint64_t ocxl_irq_get_handle(ocxl_afu_h afu, ocxl_irq_h irq) LIBOCXL_WARN_UNUSED;
int ocxl_afu_get_event_fd(ocxl_afu_h afu) LIBOCXL_WARN_UNUSED;
int ocxl_irq_get_fd(ocxl_afu_h afu, ocxl_irq_h irq) LIBOCXL_WARN_UNUSED;
int ocxl_afu_event_check_versioned(ocxl_afu_h afu, int timeout, ocxl_event *events, uint16_t event_count,
                                   uint16_t event_api_version) LIBOCXL_WARN_UNUSED;
#ifdef _ARCH_PPC64
ocxl_err ocxl_afu_get_p9_thread_id(ocxl_afu_h afu, uint16_t *thread_id);
#endif

/**
 * @addtogroup ocxl_irq
 * @{
 */

#if defined(_ARCH_PPC64) && !defined(__STRICT_ANSI__)
/**
 * A wrapper around the the Power 9 wait instruction
 *
 * The notify/wait mechanism provide a low-latency way for an AFU to signal to the
 * calling thread that a condition has been met (eg. work has been completed).
 *
 * This function will cause the thread to wait until woken by the AFU via as_notify.
 * As the thread may be woken for reasons other than as_notify, a condition variable
 * must be set by the AFU before issuing the notify.
 *
 * In order to successfully wake a waiting thread, the AFU must know the address of
 * the condition variable, and the thread ID of the application (via ocxl_afu_get_p9_thread_id()).
 *
 * A typical usage will be:
 * @code
 *   while (!condition_variable_is_set()) {
 *   	ocxl_wait();
 *   } // Pause execution until the condition variable is set
 *
 *   clear_condition_variable();
 *
 *   // Execution continues here
 * @endcode
 *
 * @see ocxl_afu_get_p9_thread_id()
 */
#ifndef _DOXYGEN_
static
#endif
inline void ocxl_wait()
{
	/* This would be just 'asm volatile ("wait")', but clang produces invalid machine code */
	asm volatile (".long 0x7c00003c");
}
#elif defined(_ARCH_PPC64) && !defined(LIBOCXL_SUPPRESS_INACCESSIBLE_WARNINGS)
#warning LibOCXL ocxl_wait() will not be available while ANSI C mode is enabled
#endif

/**
 * Check for pending IRQs and other events.
 *
 * Waits for the AFU to report an event or IRQs. On return, events will be populated
 * with the reported number of events. Each event may be either an AFU event, or an IRQ,
 * which can be determined by checking the value of events[i].type:
 *   Value							| Action
 *   ------------------------------ | -------
 *   OCXL_EVENT_IRQ					| An IRQ was triggered, and events[i].irq is populated with the IRQ information identifying which IRQ was triggered
 *   OCXL_EVENT_TRANSLATION_FAULT	| An OpenCAPI translation fault error has been issued, that is, the system has been unable to resolve an effective address. Events[i].translation_fault will be populated with the details of the error
 *
 * @param afu the AFU holding the interrupts
 * @param timeout how long to wait (in milliseconds) for interrupts to arrive, set to -1 to wait indefinitely, or 0 to return immediately if no events are available
 * @param[out] events the triggered events (caller allocated)
 * @param event_count the number of triggered events
 *
 * @return the number of events triggered, if this is the same as event_count, you should call ocxl_afu_event_check again
 * @retval -1 if an error occurred
 */
LIBOCXL_WARN_UNUSED
#ifndef _DOXYGEN_
static
#endif
inline int ocxl_afu_event_check(ocxl_afu_h afu, int timeout, ocxl_event *events, uint16_t event_count)
{
	uint16_t event_api_version = 0;
	return ocxl_afu_event_check_versioned(afu, timeout, events, event_count, event_api_version);
}

/**
 * @}
 */

/* Platform specific: PPC64 */
#ifdef _ARCH_PPC64
ocxl_err ocxl_afu_set_ppc64_amr(ocxl_afu_h afu, uint64_t amr);
#endif

/* mmio.c */
ocxl_err ocxl_mmio_map_advanced(ocxl_afu_h afu, ocxl_mmio_type type, size_t size, int prot, uint64_t flags,
                                off_t offset, ocxl_mmio_h *region) LIBOCXL_WARN_UNUSED;
ocxl_err ocxl_mmio_map(ocxl_afu_h afu, ocxl_mmio_type type, ocxl_mmio_h *region) LIBOCXL_WARN_UNUSED;
void ocxl_mmio_unmap(ocxl_mmio_h region);
int ocxl_mmio_get_fd(ocxl_afu_h afu, ocxl_mmio_type type) LIBOCXL_WARN_UNUSED;
size_t ocxl_mmio_size(ocxl_afu_h afu, ocxl_mmio_type type) LIBOCXL_WARN_UNUSED;
ocxl_err ocxl_mmio_get_info(ocxl_mmio_h region, void **address, size_t *size);
ocxl_err ocxl_mmio_read32(ocxl_mmio_h mmio, off_t offset, ocxl_endian endian, uint32_t *out);
ocxl_err ocxl_mmio_read64(ocxl_mmio_h mmio, off_t offset, ocxl_endian endian, uint64_t *out);
ocxl_err ocxl_mmio_write32(ocxl_mmio_h mmio, off_t offset, ocxl_endian endian, uint32_t value);
ocxl_err ocxl_mmio_write64(ocxl_mmio_h mmio, off_t offset, ocxl_endian endian, uint64_t value);

#ifdef __cplusplus
}
#endif
#endif	/* _LIBOCXL_H */

