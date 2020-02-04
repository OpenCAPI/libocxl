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

#include <unistd.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "libocxl.h"
#include "ocxl_afp3.h"

#define AFU_NAME "IBM,AFP3"

#define CACHELINE_BYTES 128
#define PAGE_BYTES 4096
#define AFU_MMIO_REG_SIZE 0x4000000
#define BUF_512MB 536870912
#define BUF_4MB 4194304


static int verbose;
static int timeout = 1;
static int tags_ld = 0;
static int tags_st = 7;
static int size_ld = 128;
static int size_st = 128;
static int npu_ld = 0;
static int npu_st = 0;
static uint16_t numLoops = 3;
static uint16_t waitTime = 2;
static uint64_t offsetmask = 0x3FF;   // Default to 4MB

static uint64_t enableAfu   = 0x8000000000000000;
static uint64_t disableAfu  = 0x0000000000000000;
static uint64_t resetCnt    = 0x4000000000000000;

static void print_help(char *name)
{
	printf("Usage: %s [ options ]\n", name);
	printf("\t--tags_ld   \tNumber of tags for loads.  Default=%d\n", tags_ld);
	printf("\t--tags_st   \tNumber of tags for stores.  Default=%d\n", tags_st);
	printf("\t            \t 0 -   0 tags (disabled)\n");
	printf("\t            \t 1 -   1 tag\n");
	printf("\t            \t 2 -   2 tags\n");
	printf("\t            \t 3 -   4 tags\n");
	printf("\t            \t 4 -  16 tags\n");
	printf("\t            \t 5 -  64 tags\n");
	printf("\t            \t 6 - 256 tags\n");
	printf("\t            \t 7 - 512 tags\n");
	printf("\t--size_ld   \tData size, in Bytes, for loads.   Supported values: 64, 128, 256.  Default=%d\n", size_ld);
	printf("\t--size_st   \tData size, in Bytes, for stores.  Supported values: 64, 128, 256.  Default=%d\n", size_st);
	printf("\t--npu_ld    \tUse rd_wnitc.n for loads.  Default is rd_wnitc\n");
	printf("\t--npu_st    \tUse dma_w.n for stores.  Default is dma_w\n");
	printf("\t--num       \tNumber of times to check perf counts, default is %d\n", numLoops);
	printf("\t--wait      \tAmount of seconds to wait between perf count reads, default is %d\n", waitTime);
	printf("\t--prefetch  \tInitialize buffer memory\n");
	printf("\t--offsetmask\tDetermines how much of buffer to use.  Default 512kB.  Valid Range: 4K-512M.  Format: NumberLetter, e.g. 4K, 512K, 1M, 512M\n");
	printf("\t--timeout   \tDefault=%d seconds\n", timeout);
	printf("\t--device    \tDevice to open instead of first AFP AFU found\n");
	printf("\t--verbose   \tVerbose output\n");
	printf("\t--help      \tPrint this message\n");
	printf("\n");
}

int main(int argc, char *argv[])
{
	int opt;
	int rc;
	int size_enc_ld;
	int size_enc_st;
	int option_index = 0;
	int prefetch = 0;
	uint64_t pasid;
	uint64_t wed_in = 0;
	uint64_t *buffer;
	ocxl_afu_h afu_h;
	ocxl_err err;
	ocxl_mmio_h global;
	char *device = NULL;

	// Parse parameters
	static struct option long_options[] = {
		{"tags_ld", required_argument, 0, 'a'},
		{"tags_st", required_argument, 0, 'b'},
		{"size_ld", required_argument, 0, 'y'},
		{"size_st", required_argument, 0, 'z'},
		{"num", required_argument, 0, 'n'},
		{"wait", required_argument, 0, 'w'},
		{"prefetch", no_argument, 0, 'p'},
		{"offsetmask", required_argument, 0, 'o'},
		{"timeout", required_argument, 0, 't'},
		{"verbose", no_argument, &verbose,  1 },
		{"help", no_argument, 0, 'h'},
		{"device", required_argument, 0, 'd'},
		{NULL, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "avhc:t:d:", long_options, &option_index)) >= 0) {
		switch (opt) {
		case 0:
		case 'v':
			break;
		case 'a':
			tags_ld = strtoul(optarg, NULL, 0);
			break;
		case 'b':
			tags_st = strtoul(optarg, NULL, 0);
			break;
		case 'y':
			size_ld = strtoul(optarg, NULL, 0);
			break;
		case 'z':
			size_st = strtoul(optarg, NULL, 0);
			break;
		case 'n':
			numLoops = (uint16_t) strtoul(optarg, NULL, 0);
			break;
		case 'w':
			waitTime = (uint16_t) strtoul(optarg, NULL, 0);
			break;
		case 'p':
			prefetch = 1;
			break;
		case 'o':
			if (!strcasecmp(optarg, "4K")) {
				offsetmask = 0x00;
			} else if (!strcasecmp(optarg, "8K")) {
				offsetmask = 0x01;
			} else if (!strcasecmp(optarg, "16K")) {
				offsetmask = 0x03;
			} else if (!strcasecmp(optarg, "32K")) {
				offsetmask = 0x07;
			} else if (!strcasecmp(optarg, "64K")) {
				offsetmask = 0x0F;
			} else if (!strcasecmp(optarg, "128K")) {
				offsetmask = 0x1F;
			} else if (!strcasecmp(optarg, "256K")) {
				offsetmask = 0x3F;
			} else if (!strcasecmp(optarg, "512K")) {
				offsetmask = 0x7F;
			} else if (!strcasecmp(optarg, "1M")) {
				offsetmask = 0xFF;
			} else if (!strcasecmp(optarg, "2M")) {
				offsetmask = 0x1FF;
			} else if (!strcasecmp(optarg, "4M")) {
				offsetmask = 0x3FF;
			} else if (!strcasecmp(optarg, "8M")) {
				offsetmask = 0x7FF;
			} else if (!strcasecmp(optarg, "16M")) {
				offsetmask = 0xFFF;
			} else if (!strcasecmp(optarg, "32M")) {
				offsetmask = 0x1FFF;
			} else if (!strcasecmp(optarg, "64M")) {
				offsetmask = 0x3FFF;
			} else if (!strcasecmp(optarg, "128M")) {
				offsetmask = 0x7FFF;
			} else if (!strcasecmp(optarg, "256M")) {
				offsetmask = 0xFFFF;
			} else if (!strcasecmp(optarg, "512M")) {
				offsetmask = 0x1FFFF;
			} else if (!strcasecmp(optarg, "1G")) {
				offsetmask = 0x3FFFF;
			} else if (!strcasecmp(optarg, "2G")) {
				offsetmask = 0x7FFFF;
			} else if (!strcasecmp(optarg, "4G")) {
				offsetmask = 0xFFFFF;
			} else {
				fprintf(stderr, "Illegal value entered for --offsetmask argument = 0x%lx  Must be string: 4K-512M\n", offsetmask);
				print_help(argv[0]);
				return -1;
			}
			if (offsetmask > 0x3FF)
				printf("Warning: offsetmask is bigger than the 4MB memory buffer allocated by this app\n");
			break;
		case 't':
			timeout = strtoul(optarg, NULL, 0);
			break;
		case 'd':
			device = optarg;
			break;
		case 'h':
			print_help(argv[0]);
			return 0;
			break;
		default:
			print_help(argv[0]);
			return -1;
		}
	}

	offsetmask <<= 12;

	switch (size_ld) {
	case 64:
		size_enc_ld = 1;
		break;
	case 128:
		size_enc_ld = 2;
		break;
	case 256:
		size_enc_ld = 3;
		break;
	default:
		fprintf(stderr, "Illegal value entered for --size_ld argument = %d\n", size_ld);
		print_help(argv[0]);
		return -1;
	}

	switch (size_st) {
	case 64:
		size_enc_st = 1;
		break;
	case 128:
		size_enc_st = 2;
		break;
	case 256:
		size_enc_st = 3;
		break;
	default:
		fprintf(stderr, "Illegal value entered for --size_st argument = %d\n", size_st);
		print_help(argv[0]);
		return -1;
	}

	ocxl_enable_messages(OCXL_ERRORS);

	if (verbose) {
		printf("Calling ocxl_afu_open\n");
	}
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
	if (verbose) {
		printf("Calling ocxl_afu_attach\n");
	}
	err = ocxl_afu_attach(afu_h, OCXL_ATTACH_FLAGS_NONE);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_afu_attach: %d", err);
		return err;
	}

	// map the global mmio space
	err = ocxl_mmio_map(afu_h, OCXL_GLOBAL_MMIO, &global);
	if (err != OCXL_OK) {
		fprintf(stderr, "global ocxl_mmio_map: %d", err);
		return err;
	}


	// Allocate a buffer for "to" memory buffer. Force alignment of address on cacheline boundary.
	rc = posix_memalign((void **) &buffer, BUF_4MB, BUF_4MB);
	if (rc) {
		fprintf(stderr, "Memory alloc failed for buffer: %d", rc);
		return rc;
	}
	if (verbose)
		printf("Allocated Buffer memory @ %p\n", buffer);

	if (prefetch) {
		printf("Initializing allocated memory\n");
		memset(buffer, 0x66, BUF_4MB);
	}

	// Get the PASID for the currently open context.
	pasid = ocxl_afu_get_pasid(afu_h);
	if (verbose)
		printf("PASID = %ld\n", pasid);
	err = ocxl_mmio_write64(global, AFUPASID_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, pasid);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_mmio_write64: %d", err);
		return err;
	}

	// Initialize WED value
	wed_in = (uint64_t) buffer + (tags_ld * 512) + (size_enc_ld * 128) + (npu_ld * 64) + (tags_st * 8) +
	         (size_enc_st * 2) + (npu_st);
	if (verbose)
		printf("WED = 0x%lx\n", wed_in);

	err = ocxl_mmio_write64(global, AFUWED_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, (uint64_t) wed_in);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_mmio_write64: %d", err);
		return err;
	}

	if (verbose)
		printf("BUFMASK = %lx\n", offsetmask);
	err = ocxl_mmio_write64(global, AFUBufmask_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, (uint64_t) offsetmask);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_mmio_write64: %d", err);
		return err;
	}

	if (verbose)
		printf("CONTROL_REG(reset) = %lx\n", resetCnt);
	err = ocxl_mmio_write64(global, AFUControl_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, (uint64_t) resetCnt);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_mmio_write64: %d", err);
		return err;
	}

	// Set ENABLE value
	if (verbose)
		printf("ENABLE_REG = %lx\n", enableAfu);
	err = ocxl_mmio_write64(global, AFUEnable_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, (uint64_t) enableAfu);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_mmio_write64: %d", err);
		return err;
	}
	printf("Parameters used: tags_ld=%d - size_ld=%d - tags_st=%d - size_st=%d\n",
	       tags_ld, size_ld, tags_st, size_st);

	////////////////////////////////////////////////////////////////////////
	// Measure bandwidth
	////////////////////////////////////////////////////////////////////////
	sleep(1);

	uint16_t i;
	struct timeval c0Time, c0Time_prev;
	double c0TimeElapsed, cyclesElapsed;
	uint64_t count0_prev, count1_prev, count2_prev, count3_prev, count4_prev, count5_prev, count6_prev, count7_prev;
	uint64_t count0, count1, count2, count3, count4, count5, count6, count7;
	uint64_t delta_cnt0, delta_cnt1, delta_cnt2, delta_cnt3, delta_cnt4, delta_cnt5, delta_cnt6, delta_cnt7;
	double bw_cnt0, bw_cnt1, bw_cnt2, bw_cnt3, bw_cnt4, bw_cnt5, bw_cnt6, bw_cnt7;
	double bpc_tb_cnt0, bpc_tb_cnt1, bpc_tb_cnt2, bpc_tb_cnt3, bpc_tb_cnt4, bpc_tb_cnt5, bpc_tb_cnt6, bpc_tb_cnt7;
	double bw_tb_cnt0, bw_tb_cnt1, bw_tb_cnt2, bw_tb_cnt3, bw_tb_cnt4, bw_tb_cnt5, bw_tb_cnt6, bw_tb_cnt7;


	printf("Counter         Curr Count (64B) Prev Count       Count Diff.      BW (GB/s) using App clock\tBytes or Events per AFP cycle\t\tBW using 200MHz AFU clock (GB/s)\n");
	printf("-----------------------------------------------------------------------------------------\n");

	gettimeofday(&c0Time_prev, NULL);

	err = ocxl_mmio_read64(global, AFUPerfCnt0_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, &count0_prev);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_mmio_read64: %d", err);
		return err;
	}

	err = ocxl_mmio_read64(global, AFUPerfCnt1_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, &count1_prev);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_mmio_read64: %d", err);
		return err;
	}

	err = ocxl_mmio_read64(global, AFUPerfCnt2_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, &count2_prev);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_mmio_read64: %d", err);
		return err;
	}

	err = ocxl_mmio_read64(global, AFUPerfCnt3_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, &count3_prev);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_mmio_read64: %d", err);
		return err;
	}

	err = ocxl_mmio_read64(global, AFUPerfCnt4_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, &count4_prev);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_mmio_read64: %d", err);
		return err;
	}

	err = ocxl_mmio_read64(global, AFUPerfCnt5_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, &count5_prev);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_mmio_read64: %d", err);
		return err;
	}

	err = ocxl_mmio_read64(global, AFUPerfCnt6_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, &count6_prev);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_mmio_read64: %d", err);
		return err;
	}

	err = ocxl_mmio_read64(global, AFUPerfCnt7_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, &count7_prev);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_mmio_read64: %d", err);
		return err;
	}

	sleep(waitTime);
	for (i=0; i<numLoops; i++) {

		gettimeofday(&c0Time, NULL);

		err = ocxl_mmio_read64(global, AFUPerfCnt0_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, &count0);
		if (err != OCXL_OK) {
			fprintf(stderr, "ocxl_mmio_read64: %d", err);
			return err;
		}

		err = ocxl_mmio_read64(global, AFUPerfCnt1_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, &count1);
		if (err != OCXL_OK) {
			fprintf(stderr, "ocxl_mmio_read64: %d", err);
			return err;
		}

		err = ocxl_mmio_read64(global, AFUPerfCnt2_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, &count2);
		if (err != OCXL_OK) {
			fprintf(stderr, "ocxl_mmio_read64: %d", err);
			return err;
		}

		err = ocxl_mmio_read64(global, AFUPerfCnt3_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, &count3);
		if (err != OCXL_OK) {
			fprintf(stderr, "ocxl_mmio_read64: %d", err);
			return err;
		}

		err = ocxl_mmio_read64(global, AFUPerfCnt4_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, &count4);
		if (err != OCXL_OK) {
			fprintf(stderr, "ocxl_mmio_read64: %d", err);
			return err;
		}

		err = ocxl_mmio_read64(global, AFUPerfCnt5_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, &count5);
		if (err != OCXL_OK) {
			fprintf(stderr, "ocxl_mmio_read64: %d", err);
			return err;
		}

		err = ocxl_mmio_read64(global, AFUPerfCnt6_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, &count6);
		if (err != OCXL_OK) {
			fprintf(stderr, "ocxl_mmio_read64: %d", err);
			return err;
		}

		err = ocxl_mmio_read64(global, AFUPerfCnt7_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, &count7);
		if (err != OCXL_OK) {
			fprintf(stderr, "ocxl_mmio_read64: %d", err);
			return err;
		}

		c0TimeElapsed = (c0Time.tv_sec - c0Time_prev.tv_sec) * 1000000 + c0Time.tv_usec - c0Time_prev.tv_usec;
		cyclesElapsed = count0 - count0_prev;

		bw_cnt0 = (double)(count0 - count0_prev) * (1 / (c0TimeElapsed / 1000000)) / 1000000000; //  convert to Billion cycles
		bw_cnt1 = (double)(count1 - count1_prev) * (64 / (c0TimeElapsed / 1000000)) / 1000000000; //  convert B/s to GB/s
		bw_cnt2 = (double)(count2 - count2_prev) * (64 / (c0TimeElapsed / 1000000)) / 1000000000; //  convert B/s to GB/s
		bw_cnt3 = (double)(count3 - count3_prev) * (64 / (c0TimeElapsed / 1000000)) / 1000000000; //  convert B/s to GB/s
		bw_cnt4 = (double)(count4 - count4_prev) * (64 / (c0TimeElapsed / 1000000)) / 1000000000; //  convert B/s to GB/s
		bw_cnt5 = (double)(count5 - count5_prev) * (64 / (c0TimeElapsed / 1000000)) / 1000000000; //  convert B/s to GB/s
		bw_cnt6 = (double)(count6 - count6_prev) * (64 / (c0TimeElapsed / 1000000)) / 1000000000; //  convert B/s to GB/s
		bw_cnt7 = (double)(count7 - count7_prev) * (1 / (c0TimeElapsed / 1000000)) / 1000000000; //  convert to Billion events

		bpc_tb_cnt0 = (double)(count0 - count0_prev) *  1 / cyclesElapsed;
		bpc_tb_cnt1 = (double)(count1 - count1_prev) * 64 / cyclesElapsed;
		bpc_tb_cnt2 = (double)(count2 - count2_prev) * 64 / cyclesElapsed;
		bpc_tb_cnt3 = (double)(count3 - count3_prev) * 64 / cyclesElapsed;
		bpc_tb_cnt4 = (double)(count4 - count4_prev) * 64 / cyclesElapsed;
		bpc_tb_cnt5 = (double)(count5 - count5_prev) * 64 / cyclesElapsed;
		bpc_tb_cnt6 = (double)(count6 - count6_prev) * 64 / cyclesElapsed;
		bpc_tb_cnt7 = (double)(count7 - count7_prev) *  1 / cyclesElapsed;

		bw_tb_cnt0 = (double)(count0 - count0_prev) * (1  / (cyclesElapsed / 200000000)) /
		             1000000000; //  convert to Billion cycles/s
		bw_tb_cnt1 = (double)(count1 - count1_prev) * (64 / (cyclesElapsed / 200000000)) /
		             1000000000; //  convert to Billion GB/s
		bw_tb_cnt2 = (double)(count2 - count2_prev) * (64 / (cyclesElapsed / 200000000)) /
		             1000000000; //  convert to Billion GB/s
		bw_tb_cnt3 = (double)(count3 - count3_prev) * (64 / (cyclesElapsed / 200000000)) /
		             1000000000; //  convert to Billion  GB/s
		bw_tb_cnt4 = (double)(count4 - count4_prev) * (64 / (cyclesElapsed / 200000000)) /
		             1000000000; //  convert to Billion GB/s
		bw_tb_cnt5 = (double)(count5 - count5_prev) * (64 / (cyclesElapsed / 200000000)) /
		             1000000000; //  convert to Billion GB/s
		bw_tb_cnt6 = (double)(count6 - count6_prev) * (64 / (cyclesElapsed / 200000000)) /
		             1000000000; //  convert to Billion GB/s
		bw_tb_cnt7 = (double)(count7 - count7_prev) * (1  / (cyclesElapsed / 200000000)) /
		             1000000000; //  convert to Billion cycles/s

		delta_cnt0 = count0 - count0_prev;
		delta_cnt1 = count1 - count1_prev;
		delta_cnt2 = count2 - count2_prev;
		delta_cnt3 = count3 - count3_prev;
		delta_cnt4 = count4 - count4_prev;
		delta_cnt5 = count5 - count5_prev;
		delta_cnt6 = count6 - count6_prev;
		delta_cnt7 = count7 - count7_prev;

		printf("Total Cycles    %016lx %016lx %016lx %#12.8f %#1.8f %#12.8f\n", count0, count0_prev, delta_cnt0,  bw_cnt0,
		       bpc_tb_cnt0, bw_tb_cnt0);
		printf("Good Resp Total %016lx %016lx %016lx %#12.8f %#1.8f %#12.8f\n", count1, count1_prev, delta_cnt1,  bw_cnt1,
		       bpc_tb_cnt1, bw_tb_cnt1);
		printf("Good Resp Load  %016lx %016lx %016lx %#12.8f %#1.8f %#12.8f\n", count2, count2_prev, delta_cnt2,  bw_cnt2,
		       bpc_tb_cnt2, bw_tb_cnt2);
		printf("Good Resp Store %016lx %016lx %016lx %#12.8f %#1.8f %#12.8f\n", count3, count3_prev, delta_cnt3,  bw_cnt3,
		       bpc_tb_cnt3, bw_tb_cnt3);
		printf("Retries - Total %016lx %016lx %016lx %#12.8f %#1.8f %#12.8f\n", count4, count4_prev, delta_cnt4,  bw_cnt4,
		       bpc_tb_cnt4, bw_tb_cnt4);
		printf("Retries - Loads %016lx %016lx %016lx %#12.8f %#1.8f %#12.8f\n", count5, count5_prev, delta_cnt5,  bw_cnt5,
		       bpc_tb_cnt5, bw_tb_cnt5);
		printf("Retries - Store %016lx %016lx %016lx %#12.8f %#1.8f %#12.8f\n", count6, count6_prev, delta_cnt6,  bw_cnt6,
		       bpc_tb_cnt6, bw_tb_cnt6);
		printf("No cred cycles  %016lx %016lx %016lx %#12.8f %#1.8f %#12.8f\n", count7, count7_prev, delta_cnt7,  bw_cnt7,
		       bpc_tb_cnt7, bw_tb_cnt7);
		printf("\n");

		count0_prev = count0;
		count1_prev = count1;
		count2_prev = count2;
		count3_prev = count3;
		count4_prev = count4;
		count5_prev = count5;
		count6_prev = count6;
		count7_prev = count7;
		c0Time_prev = c0Time;

		sleep(waitTime);
	}

	if (verbose)
		printf("Stopping AFU\n");
	// stop afu
	err = ocxl_mmio_write64(global, AFUEnable_AFP_REGISTER, OCXL_MMIO_LITTLE_ENDIAN, disableAfu);
	if (err != OCXL_OK) {
		fprintf(stderr, "ocxl_mmio_write64: %d", err);
		return err;
	}

	if (verbose)
		printf("Free afu\n");
	ocxl_afu_close(afu_h);
	return 0;
}
