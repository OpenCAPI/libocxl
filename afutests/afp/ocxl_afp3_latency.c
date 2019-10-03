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
#include <string.h>
#include <getopt.h>
#include <sys/mman.h>
#include <signal.h>

#include "libocxl.h"
#include "ocxl_afp3.h"

#define AFU_NAME "IBM,AFP3"
#define BUF_512MB (512 * 1024 * 1024)
#define BIT(n) (1ull << n)

static char *device = NULL;
static int verbose = 0;
static int size_ping = 8;
static int size_ld = 64;
static int size_st = 64;
static int extra_read = 0;
static uint64_t iterations = 10000;

static uint64_t disableAfu  = 0x0000000000000000;
static uint64_t resetCnt    = 0x4000000000000000;

#define miso()          asm volatile("or 26, 26, 26")

#define mfspr(rn)       ({unsigned long rval;				\
                        asm volatile("mfspr %0, %1"			\
				: "=r" (rval) : "i" (rn)); rval;})

static uint64_t read_timebase(void)
{
	return mfspr(268);
}

static void printf_buf(uint64_t addr, uint64_t size)
{
	unsigned int i, j;
	uint8_t *base_p = (uint8_t *)addr;
	uint64_t per_line = 32;

	for (i = 0; i < size/per_line; i++) {
		printf("0x%016lx:", (uint64_t) base_p);
		for (j = 0; j < per_line; j++) {
			if (j % 8 == 0)
				printf(" ");

			printf("%02x", *base_p);
			base_p++;
		}
		printf("\n");
	}
	printf("\n");
}

static int flag_stop = 0;
static void stop_handler(int signal)
{
	printf("Signal %d received, stopping\n", signal);
	flag_stop = 1;
}

static inline uint64_t ping_8B(uint64_t global_mmio_start,
			       volatile uint64_t *flag, uint64_t enable_in,
			       uint64_t *count)
{
	uint64_t *afu_enable_reg_p;
	uint64_t timebase[2];
	uint64_t j, loop_count;

	afu_enable_reg_p = (uint64_t *)(global_mmio_start +
					AFUEnable_AFP_REGISTER);

	if (*count) {
		loop_count = *count;
	} else {
		loop_count = ~0ull;
		printf("Running test forever, interrupt with ctrl-c\n");
	}

	timebase[0] = read_timebase();
	for (j = 0; j < loop_count; j++) {
		if (flag_stop)
			break;

		*flag = 0;

		*afu_enable_reg_p = enable_in;
		miso(); // force no gather

		while (*flag == 0);
	}
	timebase[1] = read_timebase();

	*count = j;
	return (timebase[1] - timebase[0]);
}

// use '-m' option with value > 8 to use this function doing a 64/128B
// MMIO write before the mmpp DMA write
static inline uint64_t ping_OVER_8B(uint64_t global_mmio_start,
				    volatile uint64_t *flag,
				    uint64_t enable_in, uint64_t *count)
{
	uint64_t *afu_enable_reg_p, *afu_large_data0_p;
	uint64_t timebase[2];
	uint64_t i, j, num_dw, loop_count;

	fprintf(stderr, "Use of ping data bigger than 8B requires special support in the ocxl driver for mmio write-combine. Disabled by default as it generates HMI on default setup\n");
	*count = 1;
	return 0;

	afu_enable_reg_p  = (uint64_t *)(global_mmio_start +
					 AFUEnable_AFP_REGISTER);
	afu_large_data0_p = (uint64_t *)(global_mmio_start +
					 Large_Data0_AFP_REGISTER);

	num_dw = size_st / sizeof(uint64_t);

	// Write enable register.
	// The AFU is configured to use the register data for its DMA
	// writes, so we need to make sure it's not 0 first.
	for (i = 0; i < num_dw; i++)
		*(afu_large_data0_p + i) = 0xCEECEECEECEE0000 + i;

	*flag = 0;
	*afu_enable_reg_p = (uint64_t) enable_in;
	miso(); // force no gather

	while (*flag == 0);

	if (*count) {
		loop_count = *count;
	} else {
		loop_count = ~0ull;
		printf("Running test forever, interrupt with ctrl-c\n");
	}

	timebase[0] = read_timebase();
	for (j = 0; j < loop_count; j++) {
		if (flag_stop)
			break;

		*flag = 0;
		// Write the large_data0 128 register
		// num_dw = 8 if m64, num_dw = 16 if m128
		for (i = 0; i < num_dw; i++) {
			// Write large_data0  register
			*(afu_large_data0_p + i) = 0xDAFADAFADAFA0000 + i;
		}

		miso(); // force no gather

		while (*flag == 0);
	}
	timebase[1] = read_timebase();

	*count = j;
	return (timebase[1] - timebase[0]);
}

//Main function called after line commands arguments processed
int ocapi_afp3_lat(void)
{
	int rc, j, k;
	int size_enc_ld, size_enc_st;
	int tags_ld = 0, tags_st = 7;
	int npu_ld = 0, npu_st = 0;
	int num_dw, use_large_data;
	uint64_t total_latency, global_mmio_start, offsetmask;
	uint64_t wed_in, misc_in, enable_in, extra_read_ea_in;
	int flag_location;
	volatile uint64_t *buffer;
	size_t size;
	ocxl_err err;
	ocxl_afu_h afu_h;
	ocxl_mmio_h mmio_h;

	if (size_ping == 8)
		use_large_data = 0;
	else
		use_large_data = 1;

	size_ld = size_st;

	switch (size_st) {
	case 64:
		size_enc_st = 1;
		break;
	case 128:
		size_enc_st = 2;
		break;
	case 256:
	case 512:
		size_enc_st = 3;
		break;
	default:
		printf("\nIllegal value entered for --size_st argument = %d!!!!\n", size_st);
		return -1;
	}

	switch (size_ld) {
	case 64:
		size_enc_ld = 1;
		break;
	case 128:
		size_enc_ld = 2;
		break;
	case 256:
	case 512:
		size_enc_ld = 3;
		break;
	default:
		printf("\nIllegal value entered for --size_ld argument = %d!!!!\n", size_ld);
		return -1;
	}

	if ((tags_ld != 0) || (tags_st == 0))
		printf("WARNING: For MMIO ping-pong latency mode, it is recommended to enable stores (tags_st > 0), and disable loads (tags_ld = 0)\n");

	printf("Parameters used: tags_ld=%d - size_ld=%d - tags_st=%d - size_st=%d\n",
	       tags_ld, size_ld, tags_st, size_st);

	// Open AFU device(s)
	if (verbose)
		printf("Calling ocxl_afu_open\n");
	if (device)
		err = ocxl_afu_open_from_dev(device, &afu_h);
	else
		err = ocxl_afu_open(AFU_NAME, &afu_h);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_afu_open() failed for %s, error %d\n",
			device ? device : AFU_NAME, err);
		return err;
	}

	// attach to afu - attach does not "start" the afu anymore
	if (verbose)
		printf("Calling ocxl_afu_attach\n");
	err = ocxl_afu_attach(afu_h, OCXL_ATTACH_FLAGS_NONE);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_afu_attach: %d", err);
		return err;
	}

	// map the mmio spaces
	err = ocxl_mmio_map(afu_h, OCXL_GLOBAL_MMIO, &mmio_h);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_mmio_map: %d\n", err);
		return err;
	}

	err = ocxl_mmio_get_info(mmio_h, (void **)&global_mmio_start, &size);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_mmio_get_info: %d\n", err);
		return err;
	}
	printf("MMIO INFO: address 0x%016lx - size 0x%lx\n",
	       global_mmio_start, size);

	// Allocate a buffer for "to" memory buffer.
	// Force alignment of address on cacheline boundary.
	offsetmask = 0x7F << 12;  // Hardcode to 512K
	rc = posix_memalign((void **) &buffer, BUF_512MB, BUF_512MB);
	if (rc) {
		perror("memalign main buffer");
		return -1;
	}
	if (verbose)
		printf("Allocated Buffer memory @ 0x%016llx\n",
		       (long long)buffer);

	// Turn off MMIO latency mode
	err = ocxl_mmio_write64(mmio_h, AFUEnable_AFP_REGISTER,
				OCXL_MMIO_LITTLE_ENDIAN, disableAfu);
	if (err != OCXL_OK) {
		fprintf(stderr,
			"ocxl_mmio_write64(AFUEnable_AFP_REGISTER): %d\n", err);
		return err;
	}

	// Initialize WED value
	wed_in = (uint64_t) buffer +
		(tags_ld << 9) + (size_enc_ld << 7) + (npu_ld << 6) +
		(tags_st << 3) + (size_enc_st << 1) + (npu_st);
	if (verbose)
		printf("WED = %016lx\n", wed_in);
	err = ocxl_mmio_write64(mmio_h, AFUWED_AFP_REGISTER,
				OCXL_MMIO_LITTLE_ENDIAN, wed_in);
	if (err != OCXL_OK) {
		fprintf(stderr,
			"ocxl_mmio_write64(AFUWED_AFP_REGISTER): %d\n", err);
		return err;
	}

	if (verbose)
		printf("BUFMASK = %016lx\n", offsetmask);
	err = ocxl_mmio_write64(mmio_h, AFUBufmask_AFP_REGISTER,
				OCXL_MMIO_LITTLE_ENDIAN, offsetmask);
	if (err != OCXL_OK) {
		fprintf(stderr,
			"ocxl_mmio_write64(AFUBufmask_AFP_REGISTER): %d\n", err);
		return err;
	}

	if (use_large_data) {
		misc_in = 1 << 12; // 0b01: triggered by writing or
				   // reading large data 0 register
		if (verbose)
			printf("MISC_REG = %016lx\n", misc_in);

		err = ocxl_mmio_write64(mmio_h, AFUMisc_AFP_REGISTER,
					OCXL_MMIO_LITTLE_ENDIAN, misc_in);
		if (err != OCXL_OK) {
			fprintf(stderr,
				"ocxl_mmio_write64(AFUMisc_AFP_REGISTER): %d\n",
				err);
			return err;
		}
	}

	if (verbose)
		printf("CONTROL_REG (reset) = %016lx\n", resetCnt);
	err = ocxl_mmio_write64(mmio_h, AFUControl_AFP_REGISTER,
				OCXL_MMIO_LITTLE_ENDIAN, resetCnt);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_mmio_write64(AFUControl_AFP_REGISTER): %d\n", err);
		return err;
	}

	if (extra_read) {
		// Set Read address to base address + 1K.  This way,
		// it does not overlap with MMIO Latency DMA Writes,
		// and we do not need to set up more memory
		extra_read_ea_in = (uint64_t) buffer + 1024;
		if (verbose)
			printf("EXTRA_READ_EA = %016lx\n", extra_read_ea_in);

		err = ocxl_mmio_write64(mmio_h, AFUExtraReadEA_AFP_REGISTER,
					OCXL_MMIO_LITTLE_ENDIAN,
					extra_read_ea_in);
		if (err != OCXL_OK) {
			fprintf(stderr,
				"ocxl_mmio_write64(AFUExtraReadEA_AFP_REGISTER): %d\n", err);
			return err;
		}

		printf("Initializing extra_read memory .....\n");
		for (j = 0; j < 64; j++)
			buffer[(1024/8) + j] = 0xdafa0201dafa0100 + j;

		if (verbose) {
			printf("Done initializing extra read memory\n");
			printf_buf(extra_read_ea_in, 512);
		}
	}

	// Set ENABLE register
	enable_in = BIT(63) | BIT(62); // AFU enable | MMIO ping pong latency test mode
	if (size_st == 512)
		enable_in |= BIT(61); // use 512B stores
	if (use_large_data)
		enable_in |= BIT(60); // use large ping pong data register for DMA write(s)
	if (extra_read)
		enable_in |= BIT(59); // extra read mode
	if (size_ld == 512)
		enable_in |= BIT(58); // use 512B loads
	if (verbose) {
		printf("ENABLE_REG = %016lx", enable_in);
		if (use_large_data)
			printf("\t> use large data regs value\n");
		else
			printf("\n");
	}

	num_dw = size_st / sizeof(uint64_t);
	for (k = 0; k < num_dw; k++)
		buffer[k] = 0;

	if (verbose) {
		printf("Buffer before test\n");
		printf_buf((uint64_t) buffer, 512);
	}

	asm volatile("": : :"memory");
	asm volatile("sync");

	///////////////////////////////////////////////////////////////////////
	// MMIO Ping-Pong Latency Test
	///////////////////////////////////////////////////////////////////////

	if (verbose)
		printf("Calling ping_pong test\n");
	printf("MMIO WR %dB (host to card) -> %sDMA WR %dB (card to host)\n",
	       size_ping, (extra_read ? "DMA RD + " : ""), size_st);

	// flag_location is the address where lower bytes of counter
	// value will be set
	flag_location = (size_st - 64) / sizeof(uint64_t);

	if (size_ping == 8)
		total_latency = ping_8B(global_mmio_start,
					&buffer[flag_location], enable_in,
					&iterations);
	else
		total_latency = ping_OVER_8B(global_mmio_start,
					     &buffer[flag_location], enable_in,
					     &iterations);

	if (verbose) {
		usleep(100000); // .1s
		printf("\nBuffer after test\n");
		printf_buf((uint64_t) buffer, 512);
	}
	printf("Completed %lu iterations. Total time measured using timebase: %10.2f ns\n",
	       iterations, total_latency*1000./512);
	printf("Average roundtrip per iteration: %10.2f ns\n",
	       total_latency*1000./iterations/512);

	// Turn off MMIO latency mode
	err = ocxl_mmio_write64(mmio_h, AFUEnable_AFP_REGISTER,
				OCXL_MMIO_LITTLE_ENDIAN, disableAfu);
	if (err != OCXL_OK) {
		fprintf(stderr,
			"ocxl_mmio_write64(AFUEnable_AFP_REGISTER): %d\n", err);
		return err;
	}

	if (verbose)
		printf("Unmap afu\n");
	ocxl_mmio_unmap(mmio_h);

	if (verbose)
		printf("Free afu\n");
	ocxl_afu_close(afu_h);
	return 0;
}

static void print_help(char *name)
{
	printf("\nUsage: %s [ options ]\n", name);
	printf("\t-i 10000     --iterations\tDefault=%ld\n", iterations);
	printf("\t-p 64 to 512 --pong      \tPong size from card to host (Bytes) Default=%d\n", size_st);
	printf("\t-x           --extraread \tAdd an DMA extraread before the DMA Wr Default is no\n");
	printf("\t-f           --forever   \tRun until CTRL+C, Default=no\n");
	printf("\t-d           --device    \tDevice to open instead of first AFP AFU found\n");
	printf("\t-v           --verbose   \tVerbose output\n");
	printf("\t-h           --help      \tPrint this message\n");
	printf("\n");
}

int main(int argc, char *argv[])
{
	int opt;
	int option_index = 0;

	static struct option long_options[] = {
		{"iterations", required_argument, 0       , 'i'},
		{"ping",       required_argument, 0       , 'm'},
		{"pong",       required_argument, 0       , 'p'},
		{"extraread",  no_argument      , 0       , 'x'},
		{"forever",    no_argument      , 0       , 'f'},
		{"verbose",    no_argument      , &verbose,  1 },
		{"help",       no_argument      , 0       , 'h'},
		{"device",     required_argument, 0       , 'd'},
		{NULL, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "vxhi:p:m:fd:", long_options,
				  &option_index)) >= 0) {
		switch (opt) {
		case 'v':
			verbose = 1;
			break;
		case 'i':
			iterations = strtoul(optarg, NULL, 0);
			break;
		case 'm':
			size_ping = strtoul(optarg, NULL, 0);
			break;
		case 'p':
			size_st = strtoul(optarg, NULL, 0);
			break;
		case 'x':
			extra_read = 1;
			break;
		case 'f':
			iterations = 0;
			break;
		case 'h':
			print_help(argv[0]);
			return 0;
		case 'd':
			device = optarg;
			break;
		default:
			print_help(argv[0]);
			return -1;
		}
	}

	// Registering signal handlers, useful for 'forever' mode
	if (signal(SIGINT, stop_handler) == SIG_ERR)
		printf("\ncan't catch SIGINT\n");
	if (signal(SIGTERM, stop_handler) == SIG_ERR)
		printf("\ncan't catch SIGTERM\n");

	return ocapi_afp3_lat();
}
