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

#define _DEFAULT_SOURCE

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include "libocxl.h"


#define LOG_ERR(fmt, x...) fprintf(stderr, fmt, ##x)
#define LOG_INF(fmt, x...) printf(fmt, ##x)

#define AFU_NAME "IBM,MEMCPY3"
#define MEMCPY_SIZE  2048 // Max of 2048

#define CACHELINESIZE	128
/* Queue sizes other than 512kB don't seem to work (still true?) */
#define QUEUE_SIZE	4095*CACHELINESIZE

#define MEMCPY_WED(queue, depth)			\
	((((uint64_t)queue) & 0xfffffffffffff000ULL) |	\
		(((uint64_t)depth) & 0xfffULL))

#define MEMCPY_WE_CMD(valid, cmd)		\
	(((valid) & 0x1) |			\
		(((cmd) & 0x3f) << 2))
#define MEMCPY_WE_CMD_VALID	(0x1 << 0)
#define MEMCPY_WE_CMD_WRAP	(0x1 << 1)
#define MEMCPY_WE_CMD_COPY		0
#define MEMCPY_WE_CMD_IRQ		1
#define MEMCPY_WE_CMD_STOP		2
#define MEMCPY_WE_CMD_WAKE_HOST_THREAD	3
#define MEMCPY_WE_CMD_INCREMENT		4
#define MEMCPY_WE_CMD_ATOMIC		5
#define MEMCPY_WE_CMD_TRANSLATE_TOUCH	6

/* global mmio registers */
#define MEMCPY_AFU_GLOBAL_CFG	0
#define MEMCPY_AFU_GLOBAL_TRACE	0x20

/* per-process mmio registers */
#define MEMCPY_AFU_PP_WED	0
#define MEMCPY_AFU_PP_STATUS	0x10
#define   MEMCPY_AFU_PP_STATUS_Terminated	0x8
#define   MEMCPY_AFU_PP_STATUS_Stopped		0x10

#define MEMCPY_AFU_PP_CTRL	0x18
#define   MEMCPY_AFU_PP_CTRL_Restart	(0x1 << 0)
#define   MEMCPY_AFU_PP_CTRL_Terminate	(0x1 << 1)
#define MEMCPY_AFU_PP_IRQ	0x28


struct memcpy_work_element {
	volatile uint8_t cmd; /* valid, wrap, cmd */
	volatile uint8_t status;
	uint16_t length;
	uint8_t cmd_extra;
	uint8_t reserved[3];
	uint64_t atomic_op;
	uint64_t src;  /* also irq EA or atomic_op2 */
	uint64_t dst;
} __packed;

struct memcpy_weq {
	struct memcpy_work_element *queue;
	struct memcpy_work_element *next;
	struct memcpy_work_element *last;
	int wrap;
	int count;
};

struct memcpy_test_args {
	bool enable_irq;
	bool verbose;
	int completion_timeout;
};

int memcpy3_queue_length(size_t queue_size)
{
	return queue_size/sizeof(struct memcpy_work_element);
}

void memcpy3_init_weq(struct memcpy_weq *weq, size_t queue_size)
{
	weq->queue = aligned_alloc(getpagesize(), queue_size);
	memset(weq->queue, 0, queue_size);
	weq->next = weq->queue;
	weq->last = weq->queue + memcpy3_queue_length(queue_size) - 1;
	weq->wrap = 0;
	weq->count = 0;
}

/*
 * Copies a work element into the queue, taking care to set the wrap
 * bit correctly.  Returns a pointer to the element in the queue.
 */
struct memcpy_work_element *memcpy3_add_we(struct memcpy_weq *weq, struct memcpy_work_element we)
{
	struct memcpy_work_element *new_we = weq->next;

	new_we->status = we.status;
	new_we->length = we.length;
	new_we->cmd_extra = we.cmd_extra;
	new_we->atomic_op = we.atomic_op;
	new_we->src = we.src;
	new_we->dst = we.dst;
	__sync_synchronize();
	new_we->cmd = (we.cmd & ~MEMCPY_WE_CMD_WRAP) | weq->wrap;
	weq->next++;
	if (weq->next > weq->last) {
		weq->wrap ^= MEMCPY_WE_CMD_WRAP;
		weq->next = weq->queue;
	}

	return new_we;
}

/**
 * Set up the Global MMIO area of the AFU
 *
 * @param afu the AFU handle
 * @return false on success
 */
static bool global_setup(ocxl_afu_h afu)
{
	uint64_t cfg;
	ocxl_mmio_h global;

	// Map the full global MMIO area of the AFU
	if (OCXL_OK != ocxl_mmio_map(afu, OCXL_GLOBAL_MMIO, &global)) {
		return true;
	}

	if (OCXL_OK != ocxl_mmio_read64(global, MEMCPY_AFU_GLOBAL_CFG, OCXL_MMIO_LITTLE_ENDIAN, &cfg)) {
		LOG_ERR("Reading global config register failed\n");
		return true;
	}
	LOG_INF("AFU config = 0x%lx\n", cfg);

	uint64_t reg = 0x8008008000000000;
	if (OCXL_OK != ocxl_mmio_write64(global, MEMCPY_AFU_GLOBAL_TRACE, OCXL_MMIO_LITTLE_ENDIAN, reg)) {
		LOG_ERR("Writing trace register failed\n");
		return true;
	}

	LOG_INF("traces reset and rearmed\n");

	return 0;
}

/**
 * Restart the AFU if it is stopped
 *
 * @param pp_mmio the per-PASID MMIO area of the AFU to restart
 * @return false on success, true on failure
 */
static bool restart_afu_if_stopped(ocxl_mmio_h pp_mmio)
{
	// Allow the AFU to proceed
	if (OCXL_OK != ocxl_mmio_write64(pp_mmio, MEMCPY_AFU_PP_CTRL, OCXL_MMIO_LITTLE_ENDIAN, MEMCPY_AFU_PP_CTRL_Restart)) {
		LOG_ERR("couldn't restart memcpy after interrupt\n");
		return true;
	}

	return false;
}

/**
 * Wait for a completion IRQ
 *
 * @param timeout the maximum amount of time to wait (seconds)
 * @param afu the AFU that will be issuing the IRQ
 * @param pp_mmio the per-PASID MMIO area of the AFU (or 0 if completion IRQ is not used)
 * @param irq_ea the handle of the completion IRQ (or 0 if not used)
 * @param err_ea the handle of the error IRQ
 *
 * @return a bitwise OR of issues detected
 * 	0x01: An AFU error was detected
 * 	0x02: A translation fault was received
 * 	0x04: An error occurred while accessing the AFU
 * 	0x08: A timeout occurred
 */
static int wait_for_irq(int timeout, ocxl_afu_h afu, ocxl_mmio_h pp_mmio, uint64_t irq_ea, uint64_t err_ea)
{
	ocxl_event event;
	int event_count;

	int check_timeout = timeout * 1000; // convert to milliseconds
	int ret = 0;

	do {
		event_count = ocxl_afu_event_check(afu, check_timeout, &event, 1);
		if (event_count < 0) {
			return 0x04;
		}

		if (event_count == 0) {
			if (timeout) {
				LOG_ERR("Timeout waiting for interrupt\n");
				ret |= 0x08;
			}
			break;
		}

		// No need to wait if we go around the loop again
		check_timeout = 0;

		switch (event.type) {
		case OCXL_EVENT_IRQ:
			if (irq_ea && event.irq.handle == irq_ea) { // We have an AFU interrupt
				LOG_INF("AFU completion interrupt received\n");
				restart_afu_if_stopped(pp_mmio);
				return 0; // Successfully got the completion interrupt & restarted the AFU
			} else if (event.irq.handle == err_ea) { // We have an AFU error interrupt
				LOG_ERR("AFU error interrupt received\n");
				ret |= 0x01;
			}
			break;
		case OCXL_EVENT_TRANSLATION_FAULT:
			LOG_ERR("Translation fault detected, addr=%p count=%lu\n",
			        event.translation_fault.addr, event.translation_fault.count);
			ret |= 0x02;
			break;
		}
	} while (event_count == 1); // Go back around in case there are more events to process

	return ret;
}

/**
 * Wait for a completion bit to be set
 *
 * @param timeout the maximum amount of time to wait (seconds)
 * @param work_element the work element to poll for completion
 * @param afu the AFU that will be issuing the IRQ
 * @param err_ea the handle of the error IRQ
 *
 * @return a bitwise OR of issues detected
 * 	0x01: An AFU error was detected
 * 	0x02: A translation fault was received
 * 	0x04: An error occurred while accessing the AFU
 * 	0x08: A timeout occurred
 */
static int wait_for_status(int timeout, struct memcpy_work_element *work_element, ocxl_afu_h afu, uint64_t err_ea)
{
	struct timeval test_timeout, temp;

	temp.tv_sec = timeout;
	temp.tv_usec = 0;

	gettimeofday(&test_timeout, NULL);
	timeradd(&test_timeout, &temp, &test_timeout);

	for (;; gettimeofday(&temp, NULL)) {
		if (timercmp(&temp, &test_timeout, >)) {
			LOG_ERR("timeout polling for completion\n");
			return -1;
		}

		int ret = wait_for_irq(0, afu, 0, 0, err_ea);
		if (ret) {
			return ret;
		}

		if (work_element->status) {
			break;
		}
	}
	return 0;
}

/**
 * Fill a buffer with data
 *
 * @param buf the buffer to fill
 * @param size the size of the buffer
 */
static void fill_buffer(char *buf, size_t size)
{
	/* Initialise source buffer */
	for (size_t i = 0; i < size; i++) {
		*(buf + i) = i & 0xff;
	}
}

/**
 * Display the status of the AFU
 *
 * @param pp_mmio the per-PASID MMIO area of the AFU context
 */
static void display_afu_status(ocxl_mmio_h pp_mmio)
{
	uint64_t status = 0;
	(void)ocxl_mmio_read64(pp_mmio, MEMCPY_AFU_PP_STATUS, OCXL_MMIO_LITTLE_ENDIAN, &status);

	if (status) {
		LOG_INF("AFU Status register is %lx\n", status);
	}
}
/**
 * Run a single memcpy operation
 *
 * @param afu the AFU to copy with
 * @param src the data source
 * @param dst where the dat should be copied to
 * @param size the number of bytes to copy
 * @param use_irq true if we should use AFU interrupts to signal completion, false for polling
 * @param timeout the timeout in seconds to wait for completion
 *
 * @return false on success
 */
static bool afu_memcpy(ocxl_afu_h afu, const char *src, char *dst, size_t size, bool use_irq, int timeout)
{
	uint64_t wed;
	struct memcpy_weq weq;
	struct memcpy_work_element memcpy_we, irq_we;
	struct memcpy_work_element *first_we, *last_we;

	memcpy3_init_weq(&weq, QUEUE_SIZE);

	// Point the work element descriptor (wed) at the work queue
	wed = MEMCPY_WED(weq.queue, QUEUE_SIZE / CACHELINESIZE);

	// Setup a work element in the queue
	memset(&memcpy_we, 0, sizeof(memcpy_we));
	memcpy_we.cmd = MEMCPY_WE_CMD(0, MEMCPY_WE_CMD_COPY);
	memcpy_we.length = htole16((uint16_t) size);
	memcpy_we.src = htole64((uintptr_t) src);
	memcpy_we.dst = htole64((uintptr_t) dst);

	LOG_INF("WED=0x%lx  src=%p  dst=%p size=%u\n", wed, src, dst, MEMCPY_SIZE);

	if (OCXL_OK != ocxl_afu_attach(afu, OCXL_ATTACH_FLAGS_NONE)) {
		goto err;
	}

	// Map the per-PASID MMIO space
	ocxl_mmio_h pp_mmio;
	if (OCXL_OK != ocxl_mmio_map(afu, OCXL_PER_PASID_MMIO, &pp_mmio)) {
		goto err;
	}

	ocxl_irq_h afu_irq;
	uint64_t afu_irq_handle = 0;
	if (use_irq) {
		// Setup the interrupt work element

		// Allocate an IRQ to use for AFU notifications
		if (OCXL_OK != ocxl_irq_alloc(afu, NULL, &afu_irq)) {
			goto err;
		}

		// Insert the IRQ handle into a work element
		afu_irq_handle = ocxl_irq_get_handle(afu, afu_irq);
		memset(&irq_we, 0, sizeof(irq_we));
		irq_we.cmd = MEMCPY_WE_CMD(1, MEMCPY_WE_CMD_IRQ);
		irq_we.src = htole64(afu_irq_handle);
		LOG_INF("irq EA = %lx\n", afu_irq_handle);
	}

	// Allocate an IRQ to report errors
	ocxl_irq_h err_irq;
	if (OCXL_OK != ocxl_irq_alloc(afu, NULL, &err_irq)) {
		goto err;
	}

	// Let the AFU know the handle to trigger for errors
	uint64_t err_irq_handle = ocxl_irq_get_handle(afu, err_irq);

	if (OCXL_OK != ocxl_mmio_write64(pp_mmio, MEMCPY_AFU_PP_IRQ, OCXL_MMIO_LITTLE_ENDIAN, err_irq_handle)) {
		goto err;
	}

	// memory barrier to ensure the descriptor is written to memory before we ask the AFU to use it
	__sync_synchronize();

	// Write the address of the work element descriptor to the AFU
	if (OCXL_OK != ocxl_mmio_write64(pp_mmio, MEMCPY_AFU_PP_WED, OCXL_MMIO_LITTLE_ENDIAN, wed)) {
		goto err;
	}

	// setup the work queue
	first_we = last_we = memcpy3_add_we(&weq, memcpy_we);
	if (use_irq) {
		last_we = memcpy3_add_we(&weq, irq_we);
	}
	__sync_synchronize();

	// Initiate the memcpy
	first_we->cmd |= MEMCPY_WE_CMD_VALID;

	/*
	 * wait for the AFU to be done
	 *
	 * if we're using an interrupt, we can go to sleep.
	 * Otherwise, we poll the last work element status from memory
	 */
	int rc = use_irq ? wait_for_irq(timeout, afu, pp_mmio, afu_irq_handle, err_irq_handle) :
	         wait_for_status(timeout, last_we, afu, err_irq_handle);
	if (rc) {
		goto err_status;
	}

	if (first_we->status != 1) {
		LOG_ERR("unexpected status 0x%x for copy\n", first_we->status);
		goto err_status;
	}

	if (last_we != first_we && last_we->status != 1) {
		LOG_ERR("unexpected status 0x%x for last work element\n", last_we->status);
		goto err_status;
	}

	restart_afu_if_stopped(pp_mmio);

	return 0;

err_status:
	display_afu_status(pp_mmio);
	goto err;

err:
	return true;
}

static void usage(char *name)
{
	fprintf(stderr, "Usage: %s [ options ]\n", name);
	fprintf(stderr, "Options:\n");
	fprintf(stderr,
	        "\t-i\t\tSend an interrupt after copy\n");
	fprintf(stderr,
	        "\t-t <timeout>\tSeconds to wait for the AFU to signal completion\n");
	exit(1);
}

int main(int argc, char *argv[])
{
	struct memcpy_test_args args;

	args.enable_irq = false;
	args.completion_timeout = -1;
	args.verbose = false;

	while (1) {
		int c = getopt(argc, argv, "+hs:it:v");
		if (c < 0)
			break;
		switch (c) {
		case '?':
		case 'h':
			usage(argv[0]);
			break;
		case 'i':
			args.enable_irq = true;
			break;
		case 't':
			args.completion_timeout = atoi(optarg);
			break;
		case 'v':
			args.verbose = true;
			break;
		}
	}

	if (args.completion_timeout == -1) {
		args.completion_timeout = 10;
	}

	if (argv[optind]) {
		fprintf(stderr,
		        "Error: Unexpected argument '%s'\n", argv[optind]);
		usage(argv[0]);
	}

	// Enable messages for open calls
	if (args.verbose) {
		ocxl_enable_messages(OCXL_ERRORS | OCXL_TRACING);
	} else {
		ocxl_enable_messages(OCXL_ERRORS);
	}

	ocxl_afu_h afu;
	if (OCXL_OK != ocxl_afu_open(AFU_NAME, &afu)) {
		LOG_ERR("Could not open AFU '%s'\n", AFU_NAME);
		exit(1);
	}

	// Enable per-AFU messages
	if (args.verbose) {
		ocxl_afu_enable_messages(afu, OCXL_ERRORS | OCXL_TRACING);
	} else {
		ocxl_afu_enable_messages(afu, OCXL_ERRORS);
	}

	if (global_setup(afu)) {
		exit(1);
	}

	// Allocate memory areas for afu to copy to/from
	char *src = aligned_alloc(64, MEMCPY_SIZE);
	char *dst = aligned_alloc(64, MEMCPY_SIZE);

	fill_buffer(src, MEMCPY_SIZE);
	memset(dst, '\0', MEMCPY_SIZE);

	if (afu_memcpy(afu, src, dst, MEMCPY_SIZE, args.enable_irq, args.completion_timeout)) {
		ocxl_afu_close(afu);
		LOG_ERR("memcpy failed\n");
		return 1;
	}

	if (memcmp(dst, src, MEMCPY_SIZE)) {
		LOG_ERR("Memory contents do not match\n");
	} else {
		LOG_INF("Memory contents match\n");
	}

	ocxl_afu_close(afu);

	return 0;
}
