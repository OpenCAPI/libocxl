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
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/eventfd.h>
#include <linux/usrirq.h>
#include <misc/ocxl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#define MAX_EVENT_SIZE	(16*sizeof(uint64_t))

/**
 * Fixme: Dummy struct, until it's added to the kernel header
 */
typedef struct ocxl_kernel_event_header {
	__u16 type; /* The type of event */
	__u16 size; /* The size of the following data structure */
} ocxl_kernel_event_header;

#define OCXL_KERNEL_EVENT_TYPE_TRANSLATION_FAULT 0

/**
 * Fixme: Dummy struct, until it's added to the kernel header
 */
typedef struct ocxl_kernel_event_translation_fault {
	__u64 addr;
	__u64 dsisr;
} ocxl_kernel_event_translation_fault;

/**
 * @defgroup ocxl_irq OpenCAPI IRQ Functions
 *
 * These functions allow the allocation and handling of AFU IRQs. IRQs can be
 * handled either via requesting an array of triggered IRQ handles (via ocxl_afu_check),
 * or by issuing callbacks via ocxl_afu_handle_callbacks().
 *
 * Each IRQ has an opaque pointer attached, which is passed to the callback, or can
 * be queried with ocxl_afu_irq_get_info(). This pointer can be used by the caller to
 * save identifying information against the IRQ.
 *
 * @{
 */

/**
 * Get an IRQ from it's handle
 *
 * @param afu the AFU the IRQ belongs to
 * @param handle the IRQ handle
 * @return a pointer to the IRQ struct
 * @retval NULL if the IRQ was not allocated
 */
static ocxl_irq *get_irq(ocxl_afu * afu, ocxl_irq_h handle)
{
	ocxl_irq *ret;

	HASH_FIND_INT(afu->irqs, &handle, ret);

	return ret;
}

/**
 * Deallocate a single IRQ
 */
static void irq_free(ocxl_afu * afu, ocxl_irq * irq)
{
	irq->handle = 0;

	if (irq->addr) {
		if (munmap(irq->addr, afu->page_size)) {
			errmsg("Could not unmap IRQ page for AFU '%s': %d: '%s'",
			       afu->identifier.afu_name, errno, strerror(errno));
		}
		irq->addr = NULL;
	}

	if (irq->event.irq_offset) {
		int rc = ioctl(afu->irq_fd, USRIRQ_FREE, &irq->event.irq_offset);
		if (rc) {
			errmsg("Could not free IRQ in kernel: %d", rc);
		}
	}
	irq->event.irq_offset = 0;

	if (irq->event.eventfd >= 0) {
		close(irq->event.eventfd);
		irq->event.eventfd = -1;
	}

	irq->info = NULL;

	HASH_DEL(afu->irqs, irq);
}

/**
 * Allocate a single IRQ
 *
 * @param afu the AFU to operate on
 * @param irq the IRQ struct to populate
 * @param info user information to associate with the handle (may be NULL)
 *
 * @retval OCXL_OK if the IRQ was allocated
 * @retval OCXL_INTERNAL_ERROR if an error occurred allocating the IRQ
 * @retval OCXL_NO_MEM if an out of memory error occurred
 */
static ocxl_err irq_allocate(ocxl_afu * afu, ocxl_irq * irq, void *info)
{
	ocxl_err ret = OCXL_INTERNAL_ERROR;
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	irq->handle = 0;
	irq->event.irq_offset = 0;
	irq->event.eventfd = -1;
	irq->addr = NULL;
	irq->info = info;

	int fd = eventfd(0, EFD_CLOEXEC);
	if (fd < 0) {
		errmsg("Could not open eventfd for AFU '%s': %d: '%s'",
		       afu->identifier.afu_name, errno, strerror(errno));
		goto errend;
	}
	irq->event.eventfd = fd;

	int rc = ioctl(afu->irq_fd, USRIRQ_ALLOC, &irq->event.irq_offset);
	if (rc) {
		errmsg("Could not allocate IRQ in kernel: %d", rc);
		goto errend;
	}

	rc = ioctl(my_afu->irq_fd, USRIRQ_SET_EVENTFD, &irq->event);
	if (rc) {
		errmsg("Could not set event descriptor in kernel: %d", rc);
		goto errend;
	}

	irq->addr = mmap(NULL, afu->page_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	                 my_afu->irq_fd, irq->event.irq_offset);
	if (irq->addr == MAP_FAILED) {
		errmsg("mmap for IRQ for AFU '%s': %d: '%s'", afu->identifier.afu_name, errno, strerror(errno));
		goto errend;
	}

	irq->handle = (ocxl_irq_h) irq->addr;

	HASH_ADD_INT(afu->irqs, handle, irq);

	return OCXL_OK;

errend:
	irq_free(my_afu, irq);
	return ret;
}

/**
 * Allocate an IRQ for an open AFU
 *
 * @param afu the AFU to allocate IRQs for
 * @param info user information to associate with the handle (may be NULL)
 * @param[out] irq_handle the handle of the allocated IRQ
 * @retval OCXL_OK if the IRQs have been allocated
 * @retval OCXL_NO_MEM if a memory allocation error occurred
 */
ocxl_err ocxl_afu_irq_alloc(ocxl_afu_h afu, void *info, ocxl_irq_h * irq_handle)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	ocxl_irq *irq = malloc(sizeof(*irq));
	if (irq == NULL) {
		errmsg("Could not allocated %d bytes for IRQ for AFU '%s'", sizeof(*irq), my_afu->identifier.afu_name);
		return OCXL_NO_MEM;
	}

	ocxl_err rc = irq_allocate(my_afu, irq, info);
	if (rc != OCXL_OK) {
		errmsg("Could not allocate IRQ for AFU '%s'", my_afu->identifier.afu_name);
		return rc;
	}

	*irq_handle = irq->handle;

	return OCXL_OK;
}

/**
 * Deallocate an IRQ for an open AFU
 *
 * @param afu the AFU to free the IRQ on
 * @param irq the IRQ handle to free
 * @retval OCXL_OK if the free was successful
 * @retval OCXL_ALREADY_DONE if the IRQ is not allocated
 * @post the IRQ is set to INVALID_IRQ
 */
ocxl_err ocxl_afu_irq_free(ocxl_afu_h afu, ocxl_irq_h * irq)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;
	ocxl_irq *my_irq = get_irq(afu, *irq);

	if (my_irq == NULL) {
		return OCXL_ALREADY_DONE;
	}

	irq_free(my_afu, my_irq);
	free(my_irq);

	*irq = OCXL_INVALID_IRQ;

	return OCXL_OK;
}

/**
 * @internal
 * Read an event from the main AFU descriptor
 *
 * @param afu the AFU to read the event from
 * @param max_supported_event the id of the maximum supported kernel event, events with an ID higher than this will be ignored
 * @param[out] event the event information
 * @retval OCXL_EVENT_ACTION_SUCCESS if the event should be handled
 * @retval OCXL_EVENT_ACTION_FAIL if the event read failed (fatal)
 * @retval OCXL_EVENT_ACTION_NONE if there was no event to read
 * @retval OCXL_EVENT_ACTION_IGNORE if the read was successful but should be ignored
 */
static ocxl_event_action read_afu_event(ocxl_afu *afu, uint16_t max_supported_event, ocxl_event *event)
{
	ocxl_kernel_event_header header;
	char buf[MAX_EVENT_SIZE];
	int ret;

	if ((ret = read(afu->fd, &header, sizeof(header))) < 0) {
		if (ret == EAGAIN || ret == EWOULDBLOCK) {
			return OCXL_EVENT_ACTION_NONE;
		}

		errmsg("read of event header from fd %d for AFU '%s' failed: %d: %s",
		       afu->fd, afu->identifier.afu_name, errno, strerror(errno));
		return OCXL_EVENT_ACTION_FAIL;
	}

	if (header.size > MAX_EVENT_SIZE) {
		errmsg("Event size %d exceeds maximum expected %d",
		       header.size, MAX_EVENT_SIZE);
		return OCXL_EVENT_ACTION_FAIL;
	}

	if (header.type > max_supported_event) {
		// Drain the unused data
		if (read(afu->fd, &buf, header.size) < 0) {
			errmsg("read of event data from fd %d for AFU '%s' failed: %d: %s",
			       afu->fd, afu->identifier.afu_name,
			       errno, strerror(errno));
		}

		// Squelch events we don't know how to handle
		return OCXL_EVENT_ACTION_IGNORE;
	}

	switch (header.type) {
	case OCXL_KERNEL_EVENT_TYPE_TRANSLATION_FAULT:
		if (read(afu->fd, buf, header.size) < 0) {
			errmsg("read of translation fault data from fd %d for AFU '%s' failed: %d: %s",
			       afu->fd, afu->identifier.afu_name,
			       errno, strerror(errno));
			return 0;
		}

		ocxl_kernel_event_translation_fault *translation_fault = (ocxl_kernel_event_translation_fault *)buf;

		event->type = OCXL_EVENT_TRANSLATION_FAULT;
		event->translation_fault.addr = (void *)translation_fault->addr;
#ifdef __ARCH_PPC64
		event->translation_fault.dsisr = translation_fault->dsisr;
#endif
		return OCXL_EVENT_ACTION_SUCCESS;
		break;
	default:
		errmsg("Unknown event %d, max_supported_event %d",
		       header.type, max_supported_event);
		return OCXL_EVENT_ACTION_FAIL;
		break;
	}
}


/**
 * Check for pending IRQs
 *
 * @param afu the AFU holding the interrupts
 * @param timeout how long to wait for interrupts to arrive
 * @param[out] events the triggered events (caller allocated)
 * @param event_count the number of events that can fit into the events array
 * @param max_supported_event the id of the maximum supported kernel event, events with an ID higher than this will be ignored
 * @return the number of events triggered, if this is the same as event_count, you should call ocxl_afu_event_check again
 */
static uint16_t __ocxl_afu_event_check(ocxl_afu_h afu, struct timeval * timeout, ocxl_event *events,
                                       uint16_t event_count, uint16_t max_supported_event)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	fd_set event_fds;
	int maxfd;

	FD_ZERO(&event_fds);
	FD_SET(my_afu->fd, &event_fds);
	maxfd = my_afu->fd;

	for (ocxl_irq * irq = my_afu->irqs; irq != NULL; irq = irq->hh.next) {
		if (irq->event.eventfd >= 0) {
			FD_SET(irq->event.eventfd, &event_fds);
			if (maxfd < irq->event.eventfd) {
				maxfd = irq->event.eventfd;
			}
		}
	}

	int ready = select(maxfd + 1, &event_fds, NULL, NULL, timeout);
	uint16_t triggered = 0;
	if (ready) {
		// Handle kernel events
		if (FD_ISSET(my_afu->fd, &event_fds)) {
			ocxl_event_action ret;
			while ((ret = read_afu_event(my_afu, max_supported_event, &events[triggered])),
			       ret == OCXL_EVENT_ACTION_SUCCESS || ret == OCXL_EVENT_ACTION_IGNORE) {
				if (ret == OCXL_EVENT_ACTION_SUCCESS) {
					triggered++;
				}
			}
		}

		// Handle AFU IRQs
		uint64_t count;
		for (ocxl_irq * irq = my_afu->irqs; irq != NULL && triggered < event_count; irq = irq->hh.next) {
			if (irq->event.eventfd >= 0) {
				if (FD_ISSET(irq->event.eventfd, &event_fds)) {
					if (read(irq->event.eventfd, &count, sizeof(count)) < 0) {
						errmsg("read of eventfd %d for AFU '%s' failed: %d: %s",
						       irq->event.eventfd, my_afu->identifier.afu_name,
						       errno, strerror(errno));
					}
					events[triggered].type = OCXL_EVENT_IRQ;
					events[triggered].irq.handle = irq->handle;
					events[triggered].irq.info = irq->info;
					events[triggered++].irq.count = count;
				}
			}
		}
	}

	return triggered;
}

/**
 * Check for pending IRQs
 *
 * @fn ocxl_afu_event_check(ocxl_afu_h afu, struct timeval * timeout, ocxl_event *events, uint16_t event_count)
 * @param afu the AFU holding the interrupts
 * @param timeout how long to wait for interrupts to arrive
 * @param[out] events the triggered events (caller allocated)
 * @param event_count the number of triggered events
 * @return the number of events triggered, if this is the same as event_count, you should call ocxl_afu_event_check again
 */
// No function implementation for ocxl_event_check, we set up a versioned symbol alias for it instead

/**
 * @}
 */


uint16_t ocxl_afu_event_check_0(ocxl_afu_h afu, struct timeval * timeout, ocxl_event *events,
                                uint16_t event_count)
{
	return __ocxl_afu_event_check(afu, timeout, events, event_count, 0);
}


__asm__(".symver ocxl_afu_event_check_0,ocxl_afu_event_check@LIBOCXL_0");
