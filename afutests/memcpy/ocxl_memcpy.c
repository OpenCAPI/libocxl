/*
 * Copyright 2018 International Business Machines
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

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include "libocxl.h"

#define LOG_ERR(pid, fmt, x...) fprintf(stderr, "Process %d: " fmt, pid, ##x)
#define LOG_INF(pid, fmt, x...) printf("Process %d: " fmt, pid, ##x)

#define AFU_NAME "IBM,MEMCPY3"
#define AFU_MAX_PROCESSES 512

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
	int loop_count;
	int size;
	int irq;
	int completion_timeout;
	int reallocate;
	int initialize;
	char *device;
	int wake_host_thread;
	int increment;
	int atomic_cas;
	int shared_mem;
	/* global vars */
	int shmid;
	char *lock;
	char *counter;
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

int global_setup(struct memcpy_test_args *args)
{
	ocxl_err err;
	ocxl_afu_h afu_h;
	uint64_t reg, cfg;
	pid_t pid;
	ocxl_mmio_h global_mmio;

	pid = getpid();
	if (args->device)
		err = ocxl_afu_open_from_dev(args->device, &afu_h);
	else
		err = ocxl_afu_open(AFU_NAME, &afu_h);

	if (err != OCXL_OK) {
		LOG_ERR(pid, "ocxl_afu_open() failed: %d\n", err);
		return -1;
	}

	err = ocxl_mmio_map(afu_h, OCXL_GLOBAL_MMIO, &global_mmio);
	if (err != OCXL_OK) {
		LOG_ERR(pid, "global ocxl_mmio_map() failed: %d\n", err);
		return -1;
	}

	// cfg = 0;
	// cfg |= (1ull << 3); /* disable 256B ops */
	// cfg &= ~((0xFFFFFFFFull) << 32);
	// cfg |=   (0xFFFFFFFCull) << 32;
	// cfg |= (1ull << 30); /* disable back-off timers */
	// cfg |= (3ull << 17); /* xtouch enable */
	// cfg |= (0b111111) << 8; /* all bypass */
	ocxl_mmio_read64(global_mmio, MEMCPY_AFU_GLOBAL_CFG, OCXL_MMIO_LITTLE_ENDIAN, &cfg);
	LOG_INF(pid, "AFU config = %#lx\n", cfg);

	reg = 0x8008008000000000;
	err = ocxl_mmio_write64(global_mmio, MEMCPY_AFU_GLOBAL_TRACE, OCXL_MMIO_LITTLE_ENDIAN, reg);
	if (err != OCXL_OK) {
		LOG_ERR(pid, "global ocxl_mmio_write64(trace) failed: %d\n", err);
		return -1;
	}

	reg = 0x000000000007100B;
	err = ocxl_mmio_write64(global_mmio, MEMCPY_AFU_GLOBAL_TRACE, OCXL_MMIO_LITTLE_ENDIAN, reg);
	if (err != OCXL_OK) {
		LOG_ERR(pid, "global ocxl_mmio_write64(trace) failed: %d\n", err);
		return -1;
	}
	LOG_INF(pid, "traces reset and rearmed\n");
	ocxl_afu_close(afu_h);
	return 0;
}

int shm_create(struct memcpy_test_args *args)
{
	/* Allocate shared memory for atomic lock and counter */
	args->shmid = shmget(IPC_PRIVATE, getpagesize(), 0);
	if (args->shmid == -1) {
		perror("Error getting shared memory segment");
		return -1;
	}
	args->lock = shmat(args->shmid, NULL, 0);
	if (args->lock == (char *)-1) {
		perror("Unable to attach shared memory segment");
		if (shmctl(args->shmid, IPC_RMID, NULL))
			perror("Error destroying shared memory segment");
		return -1;
	}
	args->counter = args->lock + args->size;
	return 0;
}

void shm_destroy(struct memcpy_test_args *args)
{
	if (shmdt(args->lock))
		perror("Error detaching shared memory segment");
	if (shmctl(args->shmid, IPC_RMID, NULL))
		perror("Error destroying shared memory segment");
}

int wait_for_status(struct memcpy_work_element *we, int timeout, pid_t pid)
{
	struct timeval test_timeout, temp;

	temp.tv_sec = timeout;
	temp.tv_usec = 0;

	gettimeofday(&test_timeout, NULL);
	timeradd(&test_timeout, &temp, &test_timeout);

	for (;; gettimeofday(&temp, NULL)) {
		if (timercmp(&temp, &test_timeout, >)) {
			LOG_ERR(pid, "timeout polling for completion\n");
			return -1;
		}
		if (we->status)
			break;
	}
	return 0;
}

int wait_for_irq(struct memcpy_work_element *we, int timeout, pid_t pid, ocxl_afu_h afu_h, uint64_t irq_ea,
                 uint64_t err_ea)
{
	ocxl_event event;
	int nevent;

	nevent = ocxl_afu_event_check(afu_h, timeout * 1000, &event, 1);
	if (nevent != 1) {
		if (nevent == 0)
			LOG_ERR(pid, "timeout waiting for AFU interrupt\n");
		else
			LOG_ERR(pid, "unexpected return value for ocxl_afu_event_check(): %d\n", nevent);
		return -1;
	}
	if (event.type != OCXL_EVENT_IRQ) {
		LOG_ERR(pid, "unexpected event type returned by ocxl_afu_event_check(): %d\n", event.type);
		return -1;

	}
	if (event.irq.handle != irq_ea) {
		if (event.irq.handle == err_ea)
			LOG_ERR(pid, "received error irq instead of AFU irq\n");
		else
			LOG_ERR(pid, "received unknown irq EA=0x%lx\n", event.irq.handle);
		return -1;
	}
	/*
	 * It's possible to receive the AFU interrupt before the work
	 * element is marked as completed. So poll for status as
	 * well. It should be short, except in case of troubles
	 */
	return wait_for_status(we, timeout, pid);
}

int wait_fast(struct memcpy_work_element *we, int timeout, pid_t pid, ocxl_afu_h afu_h, uint64_t irq_ea)
{
	struct timeval test_timeout, temp;
	ocxl_event event;
	int nevent;

	temp.tv_sec = timeout;
	temp.tv_usec = 0;

	gettimeofday(&test_timeout, NULL);
	timeradd(&test_timeout, &temp, &test_timeout);

	/*
	 * Warning: the result of the test is not deterministic:
	 *
	 * - if the thread is running on a CPU when the AFU is sending
	 *   the wake_host_thread command, then the command is
	 *   accepted and the thread gets out of ocxl_wait().
	 *
	 * - if the thread is not running, the wake_host_thread fails
	 *   and we'll receive an AFU interrupt.
	 *
	 * We don't sleep in the below loop to maxime the chances of
	 * having the thread running.
	 */
	for (;;) {
		ocxl_wait();
		if (we->status)
			break;
		gettimeofday(&temp, NULL);
		if (timercmp(&temp, &test_timeout, >)) {
			LOG_ERR(pid, "timeout waiting for wake_host_thread\n");
			return -1;
		}
	}

	/* if interrupt is sent, status is 0x11 (complete, fault response) */
	if (we->status != 1) {
		nevent = ocxl_afu_event_check(afu_h, 1000, &event, 1);
		if (nevent == 1) {
			if (event.type != OCXL_EVENT_IRQ || event.irq.handle != irq_ea) {
				LOG_ERR(pid, "received unexpected event type %d while in 'wait' (handle=%#lx)\n", event.type, event.irq.handle);
				return -1;
			}
		} else {
			LOG_ERR(pid, "wake_host_thread failed with status %d\n", we->status);
			return -1;
		}
	}
	return 0;
}

int restart_afu(pid_t pid, ocxl_mmio_h pp_mmio)
{
	ocxl_err err;
	uint64_t status;

	err = ocxl_mmio_read64(pp_mmio, MEMCPY_AFU_PP_STATUS, OCXL_MMIO_LITTLE_ENDIAN, &status);
	if (err != OCXL_OK) {
		LOG_ERR(pid, "read of process status failed: %d\n", err);
		return -1;
	}

	if (!(status & MEMCPY_AFU_PP_STATUS_Stopped))
		return 0; /* not stopped */

	err = ocxl_mmio_write64(pp_mmio, MEMCPY_AFU_PP_CTRL, OCXL_MMIO_LITTLE_ENDIAN, MEMCPY_AFU_PP_CTRL_Restart);
	if (err != OCXL_OK) {
		LOG_ERR(pid, "couldn't restart process: %d\n", err);
		return -1;
	}
	return 0;
}

int test_afu_memcpy(struct memcpy_test_args *args)
{
	uint64_t wed;
	pid_t pid;
	int i, t, rc = -1;
	uint64_t status, afu_irq_ea = 0, err_irq_ea;
	uint16_t tidr;
	struct memcpy_weq weq;
	struct memcpy_work_element memcpy_we, irq_we;
	struct memcpy_work_element increment_we, atomic_cas_we;
	struct memcpy_work_element *first_we, *last_we;
	struct timeval start, end;
	char *src, *dst;
	int nevent;
	ocxl_err err;
	ocxl_afu_h afu_h;
	ocxl_irq_h afu_irq, err_irq;
	ocxl_event event;
	ocxl_mmio_h pp_mmio;

	/* max buffer size supported by AFU */
	if (args->size > 2048)
		return -1;

	pid = getpid();

	/* Allocate memory areas for afu to copy to/from */
	if (args->shared_mem) {
		rc = shm_create(args);
		if (rc)
			exit(1);
		src = args->counter;
		dst = args->lock;
		memcpy_we.src = htole64((uintptr_t) src);
		memcpy_we.dst = htole64((uintptr_t) dst);
	} else {
		src = aligned_alloc(64, getpagesize());
	}
	if (args->atomic_cas) {
		dst = args->lock;
	} else {
		dst = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (dst == MAP_FAILED) {
			LOG_ERR(pid, "mmap failed for destination buffer\n");
			return -1;
		}
	}

	ocxl_enable_messages(OCXL_ERRORS);

	if (args->device)
		err = ocxl_afu_open_from_dev(args->device, &afu_h);
	else
		err = ocxl_afu_open(AFU_NAME, &afu_h);

	if (err != OCXL_OK) {
		LOG_ERR(pid, "ocxl_afu_open() failed: %d\n", err);
		return -1;
	}

	memcpy3_init_weq(&weq, QUEUE_SIZE);

	/* Point the work element descriptor (wed) at the weq */
	wed = MEMCPY_WED(weq.queue, QUEUE_SIZE / CACHELINESIZE);
	LOG_INF(pid, "WED = 0x%lx  src = %p  dst = %p\n", wed, src, dst);

	/* Setup the atomic compare and swap work element */
	memset(&atomic_cas_we, 0, sizeof(atomic_cas_we));
	atomic_cas_we.cmd = MEMCPY_WE_CMD(0, MEMCPY_WE_CMD_ATOMIC);
	atomic_cas_we.length = htole16((uint16_t) sizeof(uint64_t));
	atomic_cas_we.src = htole64(1);
	atomic_cas_we.dst = htole64((uintptr_t) dst);
	atomic_cas_we.atomic_op = htole64(0);
	atomic_cas_we.cmd_extra = 0x19;

	/* Setup the increment work element */
	memset(&increment_we, 0, sizeof(increment_we));
	increment_we.cmd = MEMCPY_WE_CMD(0, MEMCPY_WE_CMD_INCREMENT);
	increment_we.length = htole16((uint16_t) sizeof(pid_t));
	increment_we.src = htole64((uintptr_t) src);
	increment_we.dst = htole64((uintptr_t) dst);

	/* Setup the memcpy work element */
	memset(&memcpy_we, 0, sizeof(memcpy_we));
	memcpy_we.cmd = MEMCPY_WE_CMD(0, MEMCPY_WE_CMD_COPY);
	memcpy_we.length = htole16((uint16_t) args->size);
	memcpy_we.src = htole64((uintptr_t) src);
	memcpy_we.dst = htole64((uintptr_t) dst);

	/* Setup the interrupt work element */
	if (args->irq || args->wake_host_thread) {
		err = ocxl_irq_alloc(afu_h, NULL, &afu_irq);
		if (err != OCXL_OK) {
			LOG_ERR(pid, "ocxl_irq_alloc() failed: %d\n", err);
			goto err;
		}
		afu_irq_ea = ocxl_irq_get_handle(afu_h, afu_irq);
		LOG_INF(pid, "irq EA = %lx\n", afu_irq_ea);

		memset(&irq_we, 0, sizeof(irq_we));
		irq_we.src = htole64(afu_irq_ea);
		if (args->irq)
			irq_we.cmd = MEMCPY_WE_CMD(1, MEMCPY_WE_CMD_IRQ);
		else {
			err = ocxl_afu_get_p9_thread_id(afu_h, &tidr);
			if (err < 0) {
				LOG_ERR(pid, "ocxl_afu_get_p9_thread_id() failed: %d\n", err);
				goto err;
			}
			/*
			 * tidr allocated before attaching, so it will
			 * be in the Process Element and the default
			 * tid value used by AFU
			 */
			irq_we.cmd = MEMCPY_WE_CMD(1, MEMCPY_WE_CMD_WAKE_HOST_THREAD);
		}
	}

	err = ocxl_irq_alloc(afu_h, NULL, &err_irq);
	if (err != OCXL_OK) {
		LOG_ERR(pid, "ocxl_irq_alloc(err) failed: %d\n", err);
		goto err;
	}
	err_irq_ea = ocxl_irq_get_handle(afu_h, err_irq);

	err = ocxl_afu_attach(afu_h, 0);
	if (err != OCXL_OK) {
		LOG_ERR(pid, "ocxl_attach() failed: %d\n", err);
		goto err;
	}

	err = ocxl_mmio_map(afu_h, OCXL_PER_PASID_MMIO, &pp_mmio);
	if (err != OCXL_OK) {
		LOG_ERR(pid, "pp ocxl_mmio_map() failed: %d\n", err);
		goto err;
	}

	err = ocxl_mmio_write64(pp_mmio, MEMCPY_AFU_PP_IRQ, OCXL_MMIO_LITTLE_ENDIAN, err_irq_ea);
	if (err != OCXL_OK) {
		LOG_ERR(pid, "ocxl_mmio_write64(err irq) failed: %d\n", err);
		goto err;
	}
	__sync_synchronize();
	err = ocxl_mmio_write64(pp_mmio, MEMCPY_AFU_PP_WED, OCXL_MMIO_LITTLE_ENDIAN, wed);
	if (err != OCXL_OK) {
		LOG_ERR(pid, "ocxl_mmio_write64(wed) failed: %d\n", err);
		goto err;
	}

	/* Initialise source buffer with unique(ish) per-process value */
	if (args->atomic_cas) {
		memset(src, 0, args->size);
		increment_we.src = htole64((uintptr_t) args->counter);
		increment_we.dst = htole64((uintptr_t) args->counter);
	} else if (args->increment) {
		*(pid_t *)src = htole32(pid - 1);
	} else {
		for (i = 0; i < args->size; i++)
			*(src + i) = pid & 0xff;
	}
	rc = 0;
	gettimeofday(&start, NULL);
	/*
	 * AFU bug (?):
	 * With -l2 or more, the AFU completes Increment #1,
	 * but hangs instead of returning from Increment #2.
	 *
	 * Workaround:
	 * If we insert a Copy command with the same dst address
	 * before Increment #1, then the AFU behaves as expected
	 * and completes any number of subsequent Increments.
	 */
	if (args->increment)
		args->loop_count++;
	for (i = 0; i < args->loop_count; i++) {

		/* setup the work queue */
		if (args->atomic_cas) {
			/* acquire lock */
			first_we = memcpy3_add_we(&weq, atomic_cas_we);
			/* increment counter */
			last_we = memcpy3_add_we(&weq, increment_we);
			last_we->cmd |= MEMCPY_WE_CMD_VALID;
			/* release lock */
			last_we = memcpy3_add_we(&weq, memcpy_we);
			last_we->cmd |= MEMCPY_WE_CMD_VALID;
		} else if (args->increment && i) {
			*(pid_t *)src = htole32(le32toh(*(pid_t *)src) + 1);
			first_we = last_we = memcpy3_add_we(&weq, increment_we);
		} else {
			first_we = last_we = memcpy3_add_we(&weq, memcpy_we);
		}
		if (args->irq || args->wake_host_thread)
			last_we = memcpy3_add_we(&weq, irq_we);
		__sync_synchronize();

		/* press the big red 'go' button */
		first_we->cmd |= MEMCPY_WE_CMD_VALID;

		/*
		 * wait for the AFU to be done
		 *
		 * if we're using an interrupt, we can go to sleep.
		 * Otherwise, we poll the last work element status from memory
		 */
		if (args->irq)
			rc = wait_for_irq(last_we, args->completion_timeout, pid, afu_h, afu_irq_ea, err_irq_ea);
		else if (args->wake_host_thread)
			rc = wait_fast(last_we, args->completion_timeout, pid, afu_h, afu_irq_ea);
		else
			rc = wait_for_status(last_we, args->completion_timeout, pid);
		if (rc)
			goto err_status;
		if (first_we->status != 1) {
			LOG_ERR(pid, "unexpected status 0x%x for copy\n", first_we->status);
			goto err_status;
		}
		if (args->irq && last_we->status != 1) {
			LOG_ERR(pid, "unexpected status 0x%x for irq\n", last_we->status);
			goto err_status;
		}
		if (args->wake_host_thread && (last_we->status != 1) &&
		    (last_we->status != 0x11)) {
			LOG_ERR(pid, "unexpected status 0x%x for wake_host_thread\n", last_we->status);
			goto err_status;
		}

		/*
		 * The memory barrier is to avoid instructions
		 * re-ordering and make sure no output addresses are
		 * read before the work element status is complete
		 */
		__sync_synchronize();

		if (args->atomic_cas) {
			;	/* atomicity is checked at the end of main() */
		} else if (args->increment && i) {
			if (le32toh(*(pid_t *)dst)-le32toh(*(pid_t *)src)-1) {
				LOG_ERR(pid, "increment error on loop %d\n", i);
				goto err_status;
			}
		} else {
			if (memcmp(dst, src, args->size)) {
				LOG_ERR(pid, "copy error on loop %d\n", i);
				goto err_status;
			}
		}
		if (args->irq || args->wake_host_thread) {
			/* AFU engine stops on irq, need to restart it */
			rc = restart_afu(pid, pp_mmio);
			if (rc)
				goto err_status;
		}

		if (args->reallocate) {
			/*
			 * unmap/remap the destination buffer to force a TLBI
			 * and extra memory translation with each loop
			 */
			if (args->shared_mem) {
				shm_destroy(args);
				shm_create(args);
				src = args->counter;
				dst = args->lock;
				memcpy_we.src = htole64((uintptr_t) src);
			} else {
				munmap(dst, getpagesize());
				dst = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
				if (dst == MAP_FAILED) {
					LOG_ERR(pid, "reallocation of destination buffer failed\n");
					goto err;
				}
			}
			memcpy_we.dst = htole64((uintptr_t) dst);
			if (args->initialize) {
				/* let us fault in the destination buffer */
				memset(dst, 0, args->size);
			}
		} else if (! args->atomic_cas) {
			memset(dst, 0, args->size);
		}
	}

	gettimeofday(&end, NULL);
	t = (end.tv_sec - start.tv_sec)*1000000 + end.tv_usec - start.tv_usec;

	/* catch any error interrupt */
	nevent = ocxl_afu_event_check(afu_h, 0, &event, 1);
	if (nevent != 0) {
		if (nevent == 1) {
			if (event.irq.handle == err_irq_ea)
				LOG_ERR(pid, "received error interrupt at end of test\n");
			else
				LOG_ERR(pid, "received unexpected event at end of test, type %d\n", event.type);
		} else {
			LOG_ERR(pid, "invalid return value for ocxl_afu_event_check: %d\n", nevent);
		}
		goto err_status;
	}

	LOG_INF(pid, "%d loops in %d uS (%0.2f uS per loop)\n", args->loop_count, t, ((float) t)/args->loop_count);
	ocxl_afu_close(afu_h);
	if (args->shared_mem)
		shm_destroy(args);
	return 0;

err_status:
	err = ocxl_mmio_read64(pp_mmio, MEMCPY_AFU_PP_STATUS, OCXL_MMIO_LITTLE_ENDIAN, &status);
	if (err != OCXL_OK)
		LOG_ERR(pid, "read of process status failed: %d\n", err);
	else
		LOG_ERR(pid, "process status at end of failed test=0x%lx\n", status);
err:
	ocxl_afu_close(afu_h);
	if (args->shared_mem)
		shm_destroy(args);
	return -1;
}

void usage(char *name)
{
	fprintf(stderr, "Usage: %s [ options ]\n", name);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "\t-A\t\tRun the atomic compare and swap test\n");
	fprintf(stderr, "\t-a\t\tRun the increment test\n");
	fprintf(stderr, "\t-d <device>\tUse this opencapi card\n");
	fprintf(stderr, "\t-I\t\tInitialize the destination buffer after each loop\n");
	fprintf(stderr, "\t-i\t\tSend an interrupt after copy\n");
	fprintf(stderr, "\t-w\t\tSend a wake_host_thread command after copy\n");
	fprintf(stderr, "\t-l <loops>\tRun this number of memcpy loops (default 1)\n");
	fprintf(stderr, "\t-p <procs>\tFork this number of processes (default 1)\n");
	fprintf(stderr, "\t-p 0\t\tUse the maximum number of processes permitted by the AFU\n");
	fprintf(stderr, "\t-r\t\tReallocate the destination buffer in between 2 loops\n");
	fprintf(stderr, "\t-S\t\tOperate on shared memory\n");
	fprintf(stderr, "\t-s <bufsize>\tCopy this number of bytes (default 2048)\n");
	fprintf(stderr, "\t-t <timeout>\tSeconds to wait for the AFU to signal completion\n");
	exit(1);
}

int main(int argc, char *argv[])
{
	struct memcpy_test_args args;
	int rc, c, i, j, buflen = 2048, processes = 1;
	pid_t pid, failing;

	args.loop_count = 1;
	args.size = buflen;
	args.irq = 0;
	args.completion_timeout = -1;
	args.reallocate = 0;
	args.initialize = 0;
	args.device = NULL;
	args.wake_host_thread = 0;
	args.increment = 0;
	args.atomic_cas = 0;
	args.shared_mem = 0;
	args.shmid = -1;
	args.lock = NULL;
	args.counter = NULL;

	while (1) {
		c = getopt(argc, argv, "+aAhl:p:Ss:Iit:rd:w");
		if (c < 0)
			break;
		switch (c) {
		case '?':
		case 'h':
			usage(argv[0]);
			break;
		case 'l':
			args.loop_count = atoi(optarg);
			break;
		case 'p':
			processes = atoi(optarg);
			break;
		case 's':
			buflen = atoi(optarg);
			break;
		case 'i':
			args.irq = 1;
			break;
		case 't':
			args.completion_timeout = atoi(optarg);
			break;
		case 'r':
			args.reallocate = 1;
			break;
		case 'I':
			args.initialize = 1;
			break;
		case 'd':
			args.device = optarg;
			break;
		case 'w':
			args.wake_host_thread = 1;
			break;
		case 'a':
			args.increment = 1;
			break;
		case 'A':
			args.atomic_cas = 1;
			break;
		case 'S':
			args.shared_mem = 1;
			break;
		}
	}

	if (processes == 0)
		processes = AFU_MAX_PROCESSES;

	if (args.completion_timeout == -1) {
		args.completion_timeout = processes / 5;
		if (args.completion_timeout < 10)
			args.completion_timeout = 10;
	}

	if (argv[optind]) {
		fprintf(stderr, "Error: Unexpected argument '%s'\n", argv[optind]);
		usage(argv[0]);
	}

	if (args.wake_host_thread && args.irq) {
		fprintf(stderr, "Error: -i and -w are mutually exclusive\n");
		usage(argv[0]);
	}

	if (args.atomic_cas && args.reallocate) {
		fprintf(stderr, "Error: -A and -r are mutually exclusive\n");
		usage(argv[0]);
	}

	if (args.atomic_cas && args.shared_mem) {
		fprintf(stderr, "Error: -A and -S are mutually exclusive\n");
		usage(argv[0]);
	}

	if (args.increment && args.reallocate) {
		fprintf(stderr, "Error: -a and -r are mutually exclusive\n");
		usage(argv[0]);
	}

	if (args.increment && args.shared_mem) {
		fprintf(stderr, "Error: -a and -S are mutually exclusive\n");
		usage(argv[0]);
	}

	rc = global_setup(&args);
	if (rc)
		exit(1);

	if (args.atomic_cas) {
		rc = shm_create(&args);
		if (rc)
			exit(1);

		/* initialize lock and counter */
		memset(args.lock, 0, args.size);
		memset(args.counter, 0, args.size);
		printf("Shared memory ID: %i attached at: %p\n", args.shmid, args.lock);
	}

	for (i = 0; i < processes; i++) {
		if (!fork())
			/* Child process */
			exit(test_afu_memcpy(&args));
	}

	rc = 0;
	failing = -1;
	for (i = 0; i < processes; i++) {
		pid = wait(&j);
		if (pid && j) {
			rc++;
			if (failing == -1)
				failing = pid;
		}
	}
	if (args.atomic_cas) {
		if (*(int *)args.counter != processes * args.loop_count) {
			fprintf(stderr,"Atomicity Error:\n");
			fprintf(stderr,"  procs=%d\n", processes);
			fprintf(stderr,"  loops=%d\n", args.loop_count);
			fprintf(stderr,"  procs*loops=%d\n", processes * args.loop_count);
			fprintf(stderr,"  count=%d (should be %d)\n", *(int *)args.counter, processes * args.loop_count);
			return -1;
		}
		shm_destroy(&args);
	}

	if (rc)
		fprintf(stderr, "%d test(s) failed. Check process %d, maybe others\n", rc, failing);
	else
		printf("Test successful\n");
	return rc;
}
