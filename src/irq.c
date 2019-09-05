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

/**
 * Deallocate a single IRQ.
 *
 * @param afu the AFU the IRQ belongs to
 * @param irq the IRQ
 */
void irq_dealloc(ocxl_afu *afu, ocxl_irq *irq)
{
	if (irq->addr) {
		if (munmap(irq->addr, afu->page_size)) {
			errmsg(afu, OCXL_INTERNAL_ERROR, "Could not unmap IRQ page: %d: '%s'",
			       errno, strerror(errno));
		}
		irq->addr = NULL;
	}

	if (irq->event.irq_offset) {
		int rc = ioctl(afu->fd, OCXL_IOCTL_IRQ_FREE, &irq->event.irq_offset);
		if (rc) {
			errmsg(afu, OCXL_INTERNAL_ERROR, "Could not free IRQ in kernel: %d", rc);
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
 * @defgroup ocxl_irq OpenCAPI IRQ, Event & Wake Functions
 *
 * These functions allow the allocation and handling of AFU IRQs, OpenCAPI events, and wakes.
 * IRQs can be handled either via requesting an array of triggered IRQ handles (via ocxl_afu_check),
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
static ocxl_err irq_allocate(ocxl_afu *afu, ocxl_irq *irq, void *info)
{
	irq->event.irq_offset = 0;
	irq->event.eventfd = -1;
	irq->event.reserved = 0;
	irq->irq_number = UINT16_MAX;
	irq->addr = NULL;
	irq->info = info;
	irq->fd_info.type = EPOLL_SOURCE_IRQ;
	irq->fd_info.irq = irq;

	ocxl_err ret = OCXL_INTERNAL_ERROR;

	int fd = eventfd(0, EFD_CLOEXEC);
	if (fd < 0) {
		errmsg(afu, ret, "Could not open eventfd : %d: '%s'", errno, strerror(errno));
		goto errend;
	}
	irq->event.eventfd = fd;

	int rc = ioctl(afu->fd, OCXL_IOCTL_IRQ_ALLOC, &irq->event.irq_offset);
	if (rc) {
		errmsg(afu, ret, "Could not allocate IRQ in kernel: %d: '%s'", errno, strerror(errno));
		goto errend;
	}

	rc = ioctl(afu->fd, OCXL_IOCTL_IRQ_SET_FD, &irq->event);
	if (rc) {
		errmsg(afu, ret, "Could not set event descriptor in kernel: %d: '%s'", errno, strerror(errno));
		goto errend;
	}

	irq->addr = mmap(NULL, afu->page_size, PROT_WRITE, MAP_SHARED,
	                 afu->fd, irq->event.irq_offset);
	if (irq->addr == MAP_FAILED) {
		errmsg(afu, ret, "mmap for IRQ failed: %d: '%s'", errno, strerror(errno));
		goto errend;
	}

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.ptr = &irq->fd_info;
	if (epoll_ctl(afu->epoll_fd, EPOLL_CTL_ADD, irq->event.eventfd, &ev) == -1) {
		errmsg(afu, ret, "Could not add IRQ fd %d to epoll fd %d: %d: '%s'",
		       irq->event.eventfd, afu->epoll_fd, errno, strerror(errno));
		goto errend;
	}

	return OCXL_OK;

errend:
	irq_dealloc(afu, irq);
	return ret;
}

/**
 * Allocate an IRQ for an open AFU.
 *
 * Once allocated, the IRQ handle can be retrieved with ocxl_irq_get_handle(),
 * and written into an AFU specific register in the AFU's MMIO area. The AFU
 * can then trigger the IRQ, which can be listened for with ocxl_afu_event_check(),
 * or by obtaining an event descriptor via ocxl_irq_get_fd() and using it with
 * poll(), select(), etc.
 *
 * @param afu the AFU to allocate IRQs for
 * @param info user information to associate with the handle (may be NULL)
 * @param[out] irq the 0 indexed IRQ number that was allocated. This will be monotonically incremented by each subsequent call.
 *
 * @retval OCXL_OK if the IRQs have been allocated
 * @retval OCXL_NO_MEM if a memory allocation error occurred
 */
ocxl_err ocxl_irq_alloc(ocxl_afu_h afu, void *info, ocxl_irq_h *irq)
{
	if (afu->irq_count == afu->irq_max_count) {
		ocxl_err rc = grow_buffer(afu, (void **)&afu->irqs, &afu->irq_max_count, sizeof(ocxl_irq), INITIAL_IRQ_COUNT);
		if (rc != OCXL_OK) {
			errmsg(afu, rc, "Could not grow IRQ buffer");
			return rc;
		}
	}

	ocxl_err rc = irq_allocate(afu, &afu->irqs[afu->irq_count], info);
	if (rc != OCXL_OK) {
		errmsg(afu, rc, "Could not allocate IRQ");
		return rc;
	}
	afu->irqs[afu->irq_count].irq_number = afu->irq_count;

	*irq = (ocxl_irq_h)afu->irq_count;
	afu->irq_count++;

	return OCXL_OK;
}

/**
 * Get the 64 bit IRQ handle for an IRQ.
 *
 * This handle can be written to the AFU's MMIO area to allow the AFU to trigger the IRQ.
 *
 * @param afu the AFU the IRQ belongs to
 * @param irq the IRQ to get the handle of
 *
 * @return the handle, or 0 if the handle is invalid
 */
uint64_t ocxl_irq_get_handle(ocxl_afu_h afu, ocxl_irq_h irq)
{
	if (irq > afu->irq_count) {
		return 0;
	}

	return (uint64_t)afu->irqs[irq].addr;
}

/**
 * Get the file descriptor associated with an IRQ.
 *
 * This descriptor may be used with select/poll to determine if an IRQ is triggered.
 *
 * @param afu the AFU the IRQ belongs to
 * @param irq the IRQ to get the descriptor of
 *
 * @return the handle, or -1 if the descriptor is invalid
 */
int ocxl_irq_get_fd(ocxl_afu_h afu, ocxl_irq_h irq)
{
	if (irq > afu->irq_count) {
		return -1;
	}

	return afu->irqs[irq].event.eventfd;
}


/**
 * Get a descriptor that will trigger a poll when an AFU event occurs.
 *
 * When triggered, call ocxl_read_afu_event() to extract the event information.
 *
 * @see ocxl_afu_event_check()
 *
 * @pre the AFU has been opened
 *
 * @param afu the AFU the IRQ belongs to
 *
 * @return the handle
 */
int ocxl_afu_get_event_fd(ocxl_afu_h afu)
{
	return afu->fd;
}

typedef struct ocxl_kernel_event_header ocxl_kernel_event_header;
typedef struct ocxl_kernel_event_xsl_fault_error ocxl_kernel_event_xsl_fault_error;

/**
 * @internal
 *
 * Populate an XSL fault error event.
 *
 * @param event the event to populate
 * @param body the event body from the kernel
 */
static void populate_xsl_fault_error(ocxl_afu *afu, ocxl_event *event, void *body)
{
	ocxl_kernel_event_xsl_fault_error *err = body;

	event->type = OCXL_EVENT_TRANSLATION_FAULT;
	event->translation_fault.addr = (void *)err->addr;
#ifdef _ARCH_PPC64
	event->translation_fault.dsisr = err->dsisr;
	TRACE(afu, "Translation fault error received, addr=%p, dsisr=%llx, count=%llu",
	      event->translation_fault.addr, event->translation_fault.dsisr, err->count);
#else
	TRACE(afu, "Translation fault error received, addr=%p, count=%llu",
	      event->translation_fault.addr, err->count);
#endif
	event->translation_fault.count = err->count;
}

/**
 * @internal
 *
 * Read a single AFU event from the main AFU descriptor.
 *
 * This function should not be called directly, instead, use the ocxl_read_afu_event()
 * wrapper.
 *
 * An AFU may report OpenCAPI events independently from it's IRQs. When an event is
 * available, as notified by the descriptor returned by ocxl_afu_get_event_fd()
 * triggering a poll() or select(), this function will extract the information
 * and notify the caller if there are any further events to be queried.
 *
 * If an event is available and should be handled by the caller,
 * OCXL_EVENT_ACTION_SUCCESS is returned, and the event struct populated. The event struct
 * should be parsed by first checking the type:
 *   Value							| Action
 *   ------------------------------ | -------
 *   OCXL_EVENT_IRQ					| This value cannot be generated by this function
 *   OCXL_EVENT_TRANSLATION_FAULT	| An OpenCAPI translation fault error has been issued, that is, the system has been unable to resolve an effective address. Events[i].translation_fault will be populated with the details of the error
 *
 * @see ocxl_read_afu_event()
 *
 * @param afu the AFU to read the event from
 * @param event_api_version the version of the event API that the caller wants to see
 * @param[out] event event to populate
 * @param[out] last true if this was the last event to read from the kernel for now
 *
 * @retval OCXL_EVENT_ACTION_SUCCESS if the event should be handled
 * @retval OCXL_EVENT_ACTION_FAIL if the event read failed (fatal)
 * @retval OCXL_EVENT_ACTION_NONE if there was no event to read
 * @retval OCXL_EVENT_ACTION_IGNORE if the read was successful but should be ignored
 */
static ocxl_event_action read_afu_event(ocxl_afu_h afu, uint16_t event_api_version, ocxl_event *event, int *last)
{
	size_t event_size = sizeof(ocxl_kernel_event_header);
	*last = 0;

	uint16_t max_supported_event = 0;

	switch (event_api_version) {
	case 0:
		event_size += sizeof(ocxl_kernel_event_xsl_fault_error);
		max_supported_event = OCXL_AFU_EVENT_XSL_FAULT_ERROR;
		break;
	default:
		errmsg(afu, OCXL_INTERNAL_ERROR, "Unsupported event API version %u, your libocxl library may be too old",
		       event_api_version);
		return OCXL_EVENT_ACTION_FAIL;
	}

	char buf[event_size];

	ssize_t buf_used;
	if ((buf_used = read(afu->fd, buf, event_size)) < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			*last = 1;
			return OCXL_EVENT_ACTION_NONE;
		}

		errmsg(afu, OCXL_INTERNAL_ERROR, "read of event header from fd %d failed: %d: %s",
		       afu->fd, errno, strerror(errno));
		return OCXL_EVENT_ACTION_FAIL;
	} else if (buf_used < (ssize_t)sizeof(ocxl_kernel_event_header)) {
		errmsg(afu, OCXL_INTERNAL_ERROR, "short read of event header from fd %d", afu->fd);
		return OCXL_EVENT_ACTION_FAIL;
	}

	ocxl_kernel_event_header *header = (ocxl_kernel_event_header *)buf;

	if (header->type > max_supported_event) {
		TRACE(afu, "Unknown event received from kernel of type %u", header->type);
		*last = !! (header->flags & OCXL_KERNEL_EVENT_FLAG_LAST);
		return OCXL_EVENT_ACTION_IGNORE;
	}

	switch (header->type) {
	case OCXL_AFU_EVENT_XSL_FAULT_ERROR:
		if (buf_used != sizeof(ocxl_kernel_event_header) + sizeof(ocxl_kernel_event_xsl_fault_error)) {
			errmsg(afu, OCXL_INTERNAL_ERROR,
			       "Incorrectly sized buffer received from kernel for XSL fault error, expected %d, got %d",
			       sizeof(ocxl_kernel_event_header) + sizeof(ocxl_kernel_event_xsl_fault_error),
			       buf_used);
			return OCXL_EVENT_ACTION_FAIL;
		}
		populate_xsl_fault_error(afu, event, buf + sizeof(ocxl_kernel_event_header));
		break;

	default:
		errmsg(afu, OCXL_INTERNAL_ERROR, "Unknown event %d, max_supported_event %d",
		       header->type, max_supported_event);
		return OCXL_EVENT_ACTION_FAIL;
	}

	*last = !! (header->flags & OCXL_KERNEL_EVENT_FLAG_LAST);
	return OCXL_EVENT_ACTION_SUCCESS;
}

/**
 * Check for pending IRQs and other events.
 *
 * This function should not be called directly, instead, use the ocxl_afu_event_check()
 * wrapper.
 *
 * Waits for the AFU to report an event or IRQs. On return, events will be populated
 * with the reported number of events. Each event may be either an AFU event, or an IRQ,
 * which can be determined by checking the value of events[i].type:
 *   Value							| Action
 *   ------------------------------ | -------
 *   OCXL_EVENT_IRQ					| An IRQ was triggered, and events[i].irq is populated with the IRQ information identifying which IRQ was triggered
 *   OCXL_EVENT_TRANSLATION_FAULT	| An OpenCAPI translation fault error has been issued, that is, the system has been unable to resolve an effective address. Events[i].translation_fault will be populated with the details of the error
 *
 * @see ocxl_afu_event_check
 *
 * @param afu the AFU holding the interrupts
 * @param timeout how long to wait (in milliseconds) for interrupts to arrive, set to -1 to wait indefinitely, or 0 to return immediately if no events are available
 * @param[out] events the triggered events (caller allocated)
 * @param event_count the number of events that can fit into the events array
 * @param event_api_version the version of the event API that the caller wants to see
 *
 * @return the number of events triggered, if this is the same as event_count, you should call ocxl_afu_event_check again
 * @retval -1 if an error occurred
 */
int ocxl_afu_event_check_versioned(ocxl_afu_h afu, int timeout, ocxl_event *events, uint16_t event_count,
                                   uint16_t event_api_version)
{
	TRACE(afu, "Waiting up to %dms for AFU events", timeout);

	if (event_count > afu->epoll_event_count) {
		free(afu->epoll_events);
		afu->epoll_events = NULL;
		afu->epoll_event_count = 0;

		struct epoll_event *events = malloc(event_count * sizeof(*events));
		if (events == NULL) {
			errmsg(afu, OCXL_NO_MEM, "Could not allocate space for %d events", event_count);
			return -1;
		}

		afu->epoll_events = events;
		afu->epoll_event_count = event_count;
	}

	int count;
	if ((count = epoll_wait(afu->epoll_fd, afu->epoll_events, event_count, timeout)) == -1) {
		errmsg(afu, OCXL_INTERNAL_ERROR, "epoll_wait failed waiting for AFU events: %d: '%s'",
		       errno, strerror(errno));
		return -1;
	}

	uint16_t triggered = 0;
	for (int event = 0; event < count; event++) {
		epoll_fd_source *info = (epoll_fd_source *)afu->epoll_events[event].data.ptr;
		ocxl_event_action ret;
		ssize_t buf_used;
		uint64_t count;
		int last;

		switch (info->type) {
		case EPOLL_SOURCE_OCXL:
			while ((ret = read_afu_event(afu, event_api_version, &events[triggered], &last)),
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
			buf_used = read(info->irq->event.eventfd, &count, sizeof(count));
			if (buf_used < 0) {
				errmsg(afu, OCXL_INTERNAL_ERROR, "read of eventfd %d IRQ %d failed: %d: %s",
				       info->irq->event.eventfd, info->irq->irq_number, errno, strerror(errno));
				continue;
			} else if (buf_used != (ssize_t)sizeof(count)) {
				errmsg(afu, OCXL_INTERNAL_ERROR, "short read of eventfd %d IRQ %d");
				continue;
			}
			events[triggered].type = OCXL_EVENT_IRQ;
			events[triggered].irq.irq = info->irq->irq_number;
			events[triggered].irq.handle = (uint64_t)info->irq->addr;
			events[triggered].irq.info = info->irq->info;
			events[triggered++].irq.count = count;

			TRACE(afu, "IRQ received, irq=%u id=%llx info=%p count=%llu",
			      info->irq->irq_number, (uint64_t)info->irq->addr, info->irq->info, count);

			break;
		}
	}

	TRACE(afu, "%u events reported", triggered);

	return triggered;
}

/**
 * Get the thread ID required to wake up a Power 9 wait instruction
 *
 * The thread ID should be provided to the AFU, along with a condition variable to
 * indicate a true wake condition.
 *
 * Note that multiple AFU contexts within the same thread will share the same thread ID.
 * Thread IDs are cached within a context, and are requested from the kernel the first time
 * this function is called for an AFU context.
 *
 * If sharing AFU contexts between threads, the thread ID should be requested only in the thread
 * that will be waiting for the AFU context.
 *
 * @param afu the AFU to get the thread ID for
 * @param[out] thread_id the thread ID
 * @retval OCXL_OK if the thread ID was retrieved successfully
 * @retval OCXL_NO_DEV if the OpenCAPI device is not capable of Power 9 notify/wake
 *
 * @see ocxl_wait()
 */
ocxl_err ocxl_afu_get_p9_thread_id(ocxl_afu_h afu, uint16_t *thread_id)
{
	ocxl_afu *my_afu = (ocxl_afu *)afu;

	struct ocxl_ioctl_features features;

	int rc = ioctl(my_afu->fd, OCXL_IOCTL_GET_FEATURES, &features);
	if (rc) {
		errmsg(afu, OCXL_NO_DEV, "Could not identify platform: %d %s",
		       errno, strerror(errno));
		return OCXL_NO_DEV;
	}

	if (!(features.flags[0] & OCXL_IOCTL_FEATURES_FLAGS0_P9_WAIT)) {
		errmsg(afu, OCXL_NO_DEV, "Power 9 wait is not available on this machine");
		return OCXL_NO_DEV;
	}

	struct ocxl_ioctl_p9_wait wait_data;

	rc = ioctl(my_afu->fd, OCXL_IOCTL_ENABLE_P9_WAIT, &wait_data);
	if (rc) {
		errmsg(afu, OCXL_NO_DEV, "Could not enable wait in kernel: %d %s",
		       errno, strerror(errno));
		return OCXL_NO_DEV;
	}

	*thread_id = wait_data.thread_id;

	return OCXL_OK;
}

/**
 * @}
 */
