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
#include <misc/ocxl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/epoll.h>

#define MAX_EVENT_SIZE	(16*sizeof(uint64_t))

#define OCXL_KERNEL_EVENT_TYPE_TRANSLATION_FAULT 0

/**
 * Deallocate a single IRQ
 * @param afu the AFU the IRQ belongs to
 * @param irq the IRQ
 */
void irq_dealloc(ocxl_afu * afu, ocxl_irq * irq)
{
	if (irq->addr) {
		if (munmap(irq->addr, afu->page_size)) {
			errmsg("Could not unmap IRQ page for AFU '%s': %d: '%s'",
			       afu->identifier.afu_name, errno, strerror(errno));
		}
		irq->addr = NULL;
	}

	if (irq->event.irq_offset) {
		int rc = ioctl(afu->fd, OCXL_IOCTL_IRQ_FREE, &irq->event.irq_offset);
		if (rc) {
			errmsg("Could not free IRQ in kernel: %d", rc);
		}
		irq->event.irq_offset = 0;
	}

	if (irq->event.eventfd >= 0) {
		close(irq->event.eventfd);
		irq->event.eventfd = -1;
	}

	irq->info = NULL;
}

/**
 * @defgroup ocxl_irq OpenCAPI IRQ Functions
 *
 * These functions allow the allocation and handling of AFU IRQs. IRQs can be
 * handled either via requesting an array of triggered IRQ handles (via ocxl_afu_check),
 * or by issuing callbacks via ocxl_afu_handle_callbacks().
 *
 * Each IRQ has an opaque pointer attached, which is communicated to the caller via the event struct
 * passed back from ocxl_afu_event_check(). This pointer can be used by the caller to
 * save identifying information against the IRQ.
 *
 * @{
 */

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
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	irq->event.irq_offset = 0;
	irq->event.eventfd = -1;
	irq->event.reserved = 0;
	irq->irq_number = UINT16_MAX;
	irq->addr = NULL;
	irq->info = info;
	irq->fd_info.type = EPOLL_SOURCE_IRQ;
	irq->fd_info.irq = irq;

	int fd = eventfd(0, EFD_CLOEXEC);
	if (fd < 0) {
		errmsg("Could not open eventfd for AFU '%s': %d: '%s'",
		       afu->identifier.afu_name, errno, strerror(errno));
		goto errend;
	}
	irq->event.eventfd = fd;

	int rc = ioctl(afu->fd, OCXL_IOCTL_IRQ_ALLOC, &irq->event.irq_offset);
	if (rc) {
		errmsg("Could not allocate IRQ in kernel: %d", rc);
		goto errend;
	}

	rc = ioctl(my_afu->fd, OCXL_IOCTL_IRQ_SET_FD, &irq->event);
	if (rc) {
		errmsg("Could not set event descriptor in kernel: %d", rc);
		goto errend;
	}

	irq->addr = mmap(NULL, afu->page_size, PROT_WRITE, MAP_SHARED,
	                 my_afu->fd, irq->event.irq_offset);
	if (irq->addr == MAP_FAILED) {
		errmsg("mmap for IRQ for AFU '%s': %d: '%s'", afu->identifier.afu_name, errno, strerror(errno));
		goto errend;
	}

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.ptr = &irq->fd_info;
	if (epoll_ctl(my_afu->epoll_fd, EPOLL_CTL_ADD, irq->event.eventfd, &ev) == -1) {
		errmsg("Could not add IRQ fd %d to epoll fd %d for AFU '%s': %d: '%s'",
		       irq->event.eventfd, my_afu->epoll_fd, my_afu->identifier.afu_name,
		       errno, strerror(errno));
		goto errend;
	}

	return OCXL_OK;

errend:
	irq_dealloc(my_afu, irq);
	return OCXL_INTERNAL_ERROR;
}

/**
 * Allocate an IRQ for an open AFU
 *
 * @param afu the AFU to allocate IRQs for
 * @param info user information to associate with the handle (may be NULL)
 * @param[out] irq the 0 indexed IRQ number that was allocated. This will be monotonically incremented by each subsequent call.
 * @retval OCXL_OK if the IRQs have been allocated
 * @retval OCXL_NO_MEM if a memory allocation error occurred
 */
ocxl_err ocxl_afu_irq_alloc(ocxl_afu_h afu, void *info, ocxl_irq_h * irq)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	if (my_afu->irq_count == my_afu->irq_size) {
		size_t new_size = (my_afu->irq_size > 0) ? 2 * my_afu->irq_size : INITIAL_IRQ_COUNT;
		ocxl_irq *irqs = realloc(my_afu->irqs, new_size * sizeof(ocxl_irq));
		if (irqs == NULL) {
			errmsg("Could not realloc IRQs for afu '%s' to %d IRQs",
			       my_afu->identifier.afu_name, new_size);
			return OCXL_NO_MEM;
		}
		my_afu->irqs = irqs;
		my_afu->irq_size = new_size;
	}

	ocxl_err rc = irq_allocate(my_afu, &my_afu->irqs[my_afu->irq_count], info);
	if (rc != OCXL_OK) {
		errmsg("Could not allocate IRQ for AFU '%s'", my_afu->identifier.afu_name);
		return rc;
	}
	my_afu->irqs[my_afu->irq_count].irq_number = my_afu->irq_count;

	*irq = (ocxl_irq_h)my_afu->irq_count;
	my_afu->irq_count++;

	return OCXL_OK;
}

/**
 * Get the 64 bit IRQ ID for an IRQ
 *
 * This ID can be written to the AFU to allow the AFU to trigger the IRQ.
 *
 * @param afu the AFU the IRQ belongs to
 * @param irq the IRQ to get the handle of
 * @return the handle, or 0 if the handle is invalid
 */
uint64_t ocxl_afu_irq_get_id(ocxl_afu_h afu, ocxl_irq_h irq)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	if (irq > my_afu->irq_count) {
		return 0;
	}

	return (uint64_t)my_afu->irqs[irq].addr;
}

typedef struct ocxl_kernel_event_header ocxl_kernel_event_header;
typedef struct ocxl_kernel_event_xsl_fault_error ocxl_kernel_event_xsl_fault_error;

/**
 * @internal
 * Populate an XSL fault error event
 *
 * @param event the event to populate
 * @param body the event body from the kernel
 */
static void populate_xsl_fault_error(ocxl_event *event, void *body)
{
	ocxl_kernel_event_xsl_fault_error *err = body;

	event->type = OCXL_EVENT_TRANSLATION_FAULT;
	event->translation_fault.addr = (void *)err->addr;
#ifdef _ARCH_PPC64
	event->translation_fault.dsisr = err->dsisr;
	TRACE("Translation fault error received, addr=%p, dsisr=%llx, count=%llu",
	      event->translation_fault.addr, event->translation_fault.dsisr, err->count);
#else
	TRACE("Translation fault error received, addr=%p, count=%llu",
	      event->translation_fault.addr, err->count);
#endif
	event->translation_fault.count = err->count;
}

/**
 * @internal
 * Read events from the main AFU descriptor
 *
 * @param afu the AFU to read the event from
 * @param max_supported_event the id of the maximum supported kernel event, events with an ID higher than this will be ignored
 * @param[out] event event to populate
 * @param[out] last true if this was the last event to read from the kernel for now
 * @retval OCXL_EVENT_ACTION_SUCCESS if the event should be handled
 * @retval OCXL_EVENT_ACTION_FAIL if the event read failed (fatal)
 * @retval OCXL_EVENT_ACTION_NONE if there was no event to read
 * @retval OCXL_EVENT_ACTION_IGNORE if the read was successful but should be ignored
 */
static ocxl_event_action read_afu_event(ocxl_afu *afu, uint16_t max_supported_event,
                                        ocxl_event *event, bool *last) // hack to allow static symbol extraction
{
	size_t event_size = sizeof(ocxl_kernel_event_header);
	*last = true;

	switch (max_supported_event) {
	case 0:
		event_size += sizeof(ocxl_kernel_event_xsl_fault_error);
		break;
	default:
		errmsg("Unknown maximum supported event type %u", max_supported_event);
		return OCXL_EVENT_ACTION_FAIL;
	}

	char buf[event_size];

	int buf_used;
	if ((buf_used = read(afu->fd, buf, event_size)) < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return OCXL_EVENT_ACTION_NONE;
		}

		errmsg("read of event header from fd %d for AFU '%s' failed: %d: %s",
		       afu->fd, afu->identifier.afu_name, errno, strerror(errno));
		return OCXL_EVENT_ACTION_FAIL;
	} else if (buf_used < sizeof(ocxl_kernel_event_header)) {
		errmsg("short read of event header from fd %d for AFU '%s'",
		       afu->fd, afu->identifier.afu_name);
		return OCXL_EVENT_ACTION_FAIL;
	}

	ocxl_kernel_event_header *header = (ocxl_kernel_event_header *)buf;

	if (header->type > max_supported_event) {
		TRACE("Unknown event received from kernel of type %u", header->type);
		*last = !! (header->flags & OCXL_KERNEL_EVENT_FLAG_LAST);
		return OCXL_EVENT_ACTION_IGNORE;
	}

	switch (header->type) {
	case OCXL_AFU_EVENT_XSL_FAULT_ERROR:
		if (buf_used != sizeof(ocxl_kernel_event_header) + sizeof(ocxl_kernel_event_xsl_fault_error)) {
			errmsg("Incorrectly sized buffer received from kernel for XSL fault error, expected %d, got %d",
			       sizeof(ocxl_kernel_event_header) + sizeof(ocxl_kernel_event_xsl_fault_error),
			       buf_used);
			return OCXL_EVENT_ACTION_FAIL;
		}
		populate_xsl_fault_error(event, buf + sizeof(ocxl_kernel_event_header));
		break;

	default:
		errmsg("Unknown event %d, max_supported_event %d",
		       header->type, max_supported_event);
		return OCXL_EVENT_ACTION_FAIL;
	}

	*last = !! (header->flags & OCXL_KERNEL_EVENT_FLAG_LAST);
	return OCXL_EVENT_ACTION_SUCCESS;
}


/**
 * Check for pending IRQs and other events
 *
 * @param afu the AFU holding the interrupts
 * @param timeout how long to wait (in milliseconds) for interrupts to arrive, set to -1 to wait indefinitely, or 0 to return immediately if no events are available
 * @param[out] events the triggered events (caller allocated)
 * @param event_count the number of events that can fit into the events array
 * @param max_supported_event the id of the maximum supported kernel event, events with an ID higher than this will be ignored
 * @return the number of events triggered, if this is the same as event_count, you should call ocxl_afu_event_check again
 * @see ocxl_afu_event_check
 * @retval -1 if an error occurred
 */
int ocxl_afu_event_check_versioned(ocxl_afu_h afu, int timeout, ocxl_event *events, uint16_t event_count,
                                   uint16_t max_supported_event)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	TRACE("Waiting up to %dms for AFU events", timeout);

	if (event_count > my_afu->epoll_event_count) {
		free(my_afu->epoll_events);
		my_afu->epoll_events = NULL;
		my_afu->epoll_event_count = 0;

		struct epoll_event *events = malloc(event_count * sizeof(*events));
		if (events == NULL) {
			errmsg("Could not allocate space for %d events", event_count);
			return -1;
		}

		my_afu->epoll_events = events;
		my_afu->epoll_event_count = event_count;
	}

	int count;
	if ((count = epoll_wait(my_afu->epoll_fd, my_afu->epoll_events, event_count, timeout)) == -1) {
		errmsg("epoll_wait failed waiting for AFU events on AFU '%s': %d: '%s'",
		       my_afu->identifier.afu_name, errno, strerror(errno));
		return -1;
	}

	uint16_t triggered = 0;
	for (int event = 0; event < count; event++) {
		epoll_fd_source *info = (epoll_fd_source *)my_afu->epoll_events[event].data.ptr;
		ocxl_event_action ret;
		uint64_t count;
		bool last;

		switch (info->type) {
		case EPOLL_SOURCE_OCXL:
			while ((ret = read_afu_event(my_afu, max_supported_event, &events[triggered], &last)),
			       ret == OCXL_EVENT_ACTION_SUCCESS || ret == OCXL_EVENT_ACTION_IGNORE) {
				if (ret == OCXL_EVENT_ACTION_SUCCESS) {
					triggered++;
				}

				if (last) {
					break;
				}
			}

			if (ret == OCXL_EVENT_ACTION_FAIL) {
				return -1;
			}

			break;

		case EPOLL_SOURCE_IRQ:
			if (read(info->irq->event.eventfd, &count, sizeof(count)) < 0) {
				errmsg("read of eventfd %d for AFU '%s' IRQ %d failed: %d: %s",
				       info->irq->event.eventfd, my_afu->identifier.afu_name,
				       info->irq->irq_number, errno, strerror(errno));
				continue;
			}
			events[triggered].type = OCXL_EVENT_IRQ;
			events[triggered].irq.irq = info->irq->irq_number;
			events[triggered].irq.id = (uint64_t)info->irq->addr;
			events[triggered].irq.info = info->irq->info;
			events[triggered++].irq.count = count;

			TRACE("IRQ received, irq=%u id=%llx info=%p count=%llu",
			      info->irq->irq_number, (uint64_t)info->irq->addr, info->irq->info, count);

			break;
		}
	}

	TRACE("%u events reported", triggered);

	return triggered;
}

/**
 * @}
 */
