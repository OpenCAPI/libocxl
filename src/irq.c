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

	irq->callback = NULL;
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
	irq->callback = NULL;
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

	int irq_count = HASH_COUNT(my_afu->irqs);

	// Resize the return value array if required
	if (irq_count != my_afu->triggered_irq_count) {
		irq_count += 8;	// Some extra so we're not constantly freeing/allocing space

		if (my_afu->triggered_irq_ids) {
			free(my_afu->triggered_irq_ids);
		}

		my_afu->triggered_irq_ids = malloc(irq_count * sizeof(*my_afu->triggered_irq_ids));
		if (my_afu->triggered_irq_ids == NULL) {
			ret = OCXL_NO_MEM;
			goto errend;
		}

		my_afu->triggered_irq_count = irq_count;
	}

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
 * Associate user data with an IRQ handle
 *
 * @param afu the AFU the IRQ belongs to
 * @param irq the IRQ handle
 * @param info a pointer to the user data
 * @retval OCXL_OK if the information was associated
 * @retval OCXL_NO_IRQ if the IRQ handle is invalid
 */
ocxl_err ocxl_afu_irq_set_info(ocxl_afu_h afu, ocxl_irq_h irq, void *info)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;
	ocxl_irq *my_irq = get_irq(my_afu, irq);

	if (my_irq == NULL) {
		return OCXL_NO_IRQ;
	}

	my_irq->info = info;

	return OCXL_OK;
}

/**
 * Retrieve associated user data from an IRQ handle
 *
 * @param afu the AFU the IRQ belongs to
 * @param irq the IRQ handle
 * @param[out] info a pointer to the user data
 * @retval OCXL_OK if the information was associated
 * @retval OCXL_NO_IRQ if the IRQ handle is invalid
 */
ocxl_err ocxl_afu_irq_get_info(ocxl_afu_h afu, ocxl_irq_h irq, void **info)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;
	ocxl_irq *my_irq = get_irq(my_afu, irq);

	if (my_irq == NULL) {
		return OCXL_NO_IRQ;
	}

	*info = my_irq->info;

	return OCXL_OK;
}

/**
 * attach a callback to an IRQ
 *
 * @param afu the AFU hosting the IRQ
 * @param irq the IRQ handle
 * @param callback the callback to execute when the IRQ is triggered
 * @retval OCXL_OK if the callback was attached
 * @retval OCXL_NO_IRQ if the IRQ is invalid
 * @retval OCXL_ALREADY_DONE if there is already a callback attached
 */
ocxl_err ocxl_afu_irq_attach_callback(ocxl_afu_h afu, ocxl_irq_h irq,
                                      void (*callback) (ocxl_afu_h afu, ocxl_irq_h irq, void *info))
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;
	ocxl_irq *my_irq = get_irq(my_afu, irq);

	if (my_irq == NULL) {
		return OCXL_NO_IRQ;
	}

	if (my_irq->callback) {
		return OCXL_ALREADY_DONE;
	}

	my_irq->callback = callback;

	return OCXL_OK;
}

/**
 * detach a callback from an IRQ
 *
 * @param afu the AFU hosting the IRQ
 * @param irq the IRQ handle to detach
 * @retval OCXL_OK if the callback was attached
 * @retval OCXL_ALREADY_DONE if there was no callback attached
 */
ocxl_err ocxl_afu_irq_detach_callback(ocxl_afu_h afu, ocxl_irq_h irq)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;
	ocxl_irq *my_irq = get_irq(my_afu, irq);

	if (my_irq == NULL) {
		return OCXL_NO_IRQ;
	}

	if (my_irq->callback == NULL) {
		return OCXL_ALREADY_DONE;
	}

	my_irq->callback = NULL;
	my_irq->info = NULL;

	return OCXL_OK;
}

/**
 * Handle any pending IRQ callbacks
 *
 * Check for pending IRQs and issue their attached callbacks
 *
 * @param afu the AFU holding the interrupts
 * @param timeout how long to wait for interrupts to arrive
 * @return the number of IRQs handled
 */
uint16_t ocxl_afu_irq_handle_callbacks(ocxl_afu_h afu, struct timeval * timeout)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	fd_set irqs;
	int maxfd = -1;
	uint64_t count;

	FD_ZERO(&irqs);

	for (ocxl_irq * irq = my_afu->irqs; irq != NULL; irq = irq->hh.next) {
		if (irq->event.eventfd > 0 && irq->callback) {
			FD_SET(irq->event.eventfd, &irqs);
			if (maxfd < irq->event.eventfd) {
				maxfd = irq->event.eventfd;
			}
		}
	}

	uint16_t ready = select(maxfd + 1, &irqs, NULL, NULL, timeout);
	if (ready) {
		for (ocxl_irq * irq = my_afu->irqs; irq != NULL; irq = irq->hh.next) {
			if (irq->event.eventfd > 0 && irq->callback) {
				if (FD_ISSET(irq->event.eventfd, &irqs)) {
					if (read(irq->event.eventfd, &count, sizeof(count)) < 0) {
						errmsg("read of eventfd %d for AFU '%s' failed: %d: %s",
						       irq->event.eventfd, my_afu->identifier.afu_name,
						       errno, strerror(errno));
					}
					irq->callback(afu, irq->handle, irq->info);
				}
			}
		}
	}

	return ready;
}

/**
 * Check for pending IRQs
 *
 * @param afu the AFU holding the interrupts
 * @param timeout how long to wait for interrupts to arrive
 * @param[out] triggered_irqs a list of the tirggered IRQ handles
 * @return the number of IRQs triggered
 */
uint16_t ocxl_afu_irq_check(ocxl_afu_h afu, struct timeval * timeout, const ocxl_irq_h ** triggered_irqs)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	fd_set irqs;
	int maxfd = -1;
	uint64_t count;

	FD_ZERO(&irqs);

	for (ocxl_irq * irq = my_afu->irqs; irq != NULL; irq = irq->hh.next) {
		if (irq->event.eventfd >= 0 && irq->callback == NULL) {
			FD_SET(irq->event.eventfd, &irqs);
			if (maxfd < irq->event.eventfd) {
				maxfd = irq->event.eventfd;
			}
		}
	}

	int ready = select(maxfd + 1, &irqs, NULL, NULL, timeout);
	uint16_t triggered = 0;
	if (ready) {
		for (ocxl_irq * irq = my_afu->irqs; irq != NULL; irq = irq->hh.next) {
			if (irq->event.eventfd >= 0 && irq->callback == NULL) {
				if (FD_ISSET(irq->event.eventfd, &irqs)) {
					if (read(irq->event.eventfd, &count, sizeof(count)) < 0) {
						errmsg("read of eventfd %d for AFU '%s' failed: %d: %s",
						       irq->event.eventfd, my_afu->identifier.afu_name,
						       errno, strerror(errno));
					}
					my_afu->triggered_irq_ids[triggered++] = irq->handle;
				}
			}
		}
	}

	*triggered_irqs = my_afu->triggered_irq_ids;

	return triggered;
}

/**
 * @}
 */
