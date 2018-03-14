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

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>

#include <misc/ocxl.h>

#include "libocxl_internal.h"
#include "static.h"

#include <stdlib.h>

static const char *ocxl_sysfs_path = "/tmp/ocxl-test";
static const char *ocxl_dev_path = "/dev/ocxl-test";

#define MAX_TESTS 1024
#define TEST_NAME_LEN	32
#define SUITE_NAME_LEN	32

#define GLOBAL_MMIO_SIZE	(32*1024*1024)
#define PER_PASID_MMIO_SIZE	16384

typedef enum state {
	IN_PROGRESS,
	SUCCESS,
	SKIP,
	FAIL
} state;

typedef struct test {
	char suite[SUITE_NAME_LEN+1];
	char name[TEST_NAME_LEN+1];
	state state;
	int count; /**< The number of checks performed */
} test;

test tests[MAX_TESTS];
int current_test = -1;

#define ASSERT(statement) do { \
	tests[current_test].count++; \
	if (!(statement)) { \
		fprintf(stderr, "ASSERT: %s:%d: %s(): '%s' is false\n", __FILE__, __LINE__, __FUNCTION__, #statement); \
		test_stop(FAIL); \
		goto end; \
	} \
} while(0)

#define SKIP(statement, msg) do { \
	if ((statement)) { \
		fprintf(stderr, "Skip: '%s' is true\n", #statement); \
		test_stop(SKIP); \
		goto end; \
	} \
} while(0)


// virtocxl functions
pthread_t create_ocxl_device(const char *afu_name, size_t global_mmio_size, size_t per_pasid_mmio_size);
void stop_afu();
void term_afu();
void force_translation_fault(void *addr, uint64_t dsisr);
bool afu_is_attached();

/**
 * Start a test
 * @param suite the name of the test suite
 * @param name the name of the test
 */
static void test_start(const char *suite, const char *name) {
	printf("Starting test %s:%s\n",
			suite, name);
	current_test++;
	if (current_test == MAX_TESTS) {
		fprintf(stderr, "Too many tests");
		exit(1);
	}

	if (current_test && tests[current_test-1].state == IN_PROGRESS) {
		fprintf(stderr, "Could not start '%s:%s', previous test '%s:%s' is still in progress",
				suite, name, tests[current_test-1].suite, tests[current_test-1].name);
	}

	strncpy(tests[current_test].suite, suite, sizeof(tests->suite));
	strncpy(tests[current_test].name, name, sizeof(tests->name));
	tests[current_test].state = IN_PROGRESS;
	tests[current_test].count = 0;
}

/**
 * Complete the current test
 * @param state the state of the test
 */
static void test_stop(state state) {
	tests[current_test].state = state;
	if (state == SUCCESS) {
		printf("OK\n");
	} else if (state == SKIP) {
		printf("Skipped\n");
	} else if (state == FAIL) {
		printf("Failed\n");
	}
}

/**
 * Report the state of the tests
 * @return true if we have failed tests
 */
static bool test_report() {
	bool has_failed = false;

	printf("Summary:\n");

	int max_suite_name_len = 0;
	int max_test_name_len = 0;

	for (int test = 0; test <= current_test; test++) {
		int len = strlen(tests[test].suite);
		if (max_suite_name_len < len) {
			max_suite_name_len = len;
		}

		len = strlen(tests[test].name);
		if (max_test_name_len < len) {
			max_test_name_len = len;
		}
	}

	for (int test = 0; test <= current_test; test++) {
		char *state;
		switch (tests[test].state) {
		case IN_PROGRESS:
			state = "In Progress";
			has_failed = true;
			break;
		case SUCCESS:
			state = "OK";
			break;
		case SKIP:
			state = "Skipped";
			break;
		case FAIL:
			state = "Failed";
			has_failed = true;
			break;
		default:
			state = "Unknown";
			has_failed = true;
			break;
		}

		printf("\t%-*s\t%-*s\t%s\t%d checks\n",
				max_suite_name_len, tests[test].suite,
				max_test_name_len, tests[test].name, state, tests[test].count);
	}

	return has_failed;
}

/**
 * Ensure the init call modifies the data structure
 */
static void test_afu_init() {
	test_start("AFU", "init");

	ocxl_afu afu1, afu2;

	memset(&afu1, 0xff, sizeof(afu1));
	memset(&afu2, 0xff, sizeof(afu2));

	ASSERT(!memcmp(&afu1, &afu2, sizeof(afu1)));
	afu_init(&afu1);
	ASSERT(memcmp(&afu1, &afu2, sizeof(afu1)));

	ocxl_afu_h afu = (ocxl_afu_h) &afu1;
	ASSERT(ocxl_afu_get_device_path(afu) == NULL);
	ASSERT(ocxl_afu_get_sysfs_path(afu) == NULL);
	uint8_t major, minor;
	ocxl_afu_get_version(afu, &major, &minor);
	ASSERT(major == 0);
	ASSERT(minor == 0);
	ASSERT(ocxl_afu_get_event_fd(afu) == -1);
	ASSERT(ocxl_mmio_size(afu, OCXL_GLOBAL_MMIO) == 0);
	ASSERT(ocxl_mmio_size(afu, OCXL_PER_PASID_MMIO) == 0);

	ASSERT(ocxl_mmio_get_fd(afu, OCXL_GLOBAL_MMIO) == -1);
	ASSERT(ocxl_mmio_get_fd(afu, OCXL_PER_PASID_MMIO) == -1);

	test_stop(SUCCESS);

end:
	;
}

/**
 * Check that the AFU can allocated
 */
static void test_ocxl_afu_alloc() {
	test_start("AFU", "ocxl_afu_alloc");

	ocxl_afu template;
	afu_init(&template);

	ocxl_afu_h afu = OCXL_INVALID_AFU;
	ASSERT(OCXL_OK == afu_alloc(&afu));
	ASSERT(afu != 0);
	ASSERT(ocxl_afu_get_device_path(afu) == NULL);
	ASSERT(ocxl_afu_get_sysfs_path(afu) == NULL);
	ASSERT(ocxl_afu_get_event_fd(afu) == -1);
	ASSERT(ocxl_mmio_size(afu, OCXL_GLOBAL_MMIO) == 0);
	ASSERT(ocxl_mmio_size(afu, OCXL_PER_PASID_MMIO) == 0);
	ASSERT(ocxl_mmio_get_fd(afu, OCXL_GLOBAL_MMIO) == -1);
	ASSERT(ocxl_mmio_get_fd(afu, OCXL_PER_PASID_MMIO) == -1);

	test_stop(SUCCESS);

end:
	ocxl_afu_close(afu);
}

/**
 * Check device_matches
 */
static void test_device_matches() {
	test_start("AFU", "device_matches");

	DIR *dev_dir = opendir("/dev");
	int dev_fd = dirfd(dev_dir);
	struct stat urandom;
	ASSERT(!fstatat(dev_fd, "urandom", &urandom, 0));
	ASSERT(!device_matches(dev_fd, "zero", urandom.st_rdev));
	ASSERT(device_matches(dev_fd, "urandom", urandom.st_rdev));

	test_stop(SUCCESS);

end:
	closedir(dev_dir);
}

pthread_t afu_thread = 0;

/**
 * Create the virtual AFU
 * @post afu_thread is set and must be joined
 */
static void create_afu() {
	afu_thread = create_ocxl_device("IBM,Dummy", GLOBAL_MMIO_SIZE, PER_PASID_MMIO_SIZE);
	if (!afu_thread) {
		fprintf(stderr, "Could not create dummy AFU\n");
		exit(1);
	}
}

/**
 * check populate_metadata()
 */
static void test_populate_metadata() {
	test_start("AFU", "populate_metadata");

	struct stat dev_stat;
	ASSERT(!stat("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &dev_stat));

	ocxl_afu afu;
	afu_init(&afu);

	ASSERT(populate_metadata(dev_stat.st_rdev, &afu));
	ASSERT(!strcmp(afu.identifier.afu_name, "IBM,Dummy"));
	ASSERT(afu.identifier.afu_index == 0);
	ASSERT(!strcmp(afu.device_path, "/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0"));
	ASSERT(!strcmp(afu.sysfs_path, "/tmp/ocxl-test/IBM,Dummy.0001:00:00.1.0"));

	test_stop(SUCCESS);

end:
	;
}

/**
 * Check get_afu_by_path
 */
static void test_get_afu_by_path() {
	test_start("AFU", "get_afu_by_path");

	ocxl_afu_h afu = OCXL_INVALID_AFU;
	const char *symlink_path = "/tmp/ocxl-test-symlink";

	ocxl_enable_messages(OCXL_NO_MESSAGES);
	ASSERT(OCXL_NO_DEV == get_afu_by_path("/nonexistent", &afu));
	ocxl_enable_messages(OCXL_ERRORS);
	ASSERT(0 == afu);

	afu = 0;
	ASSERT(OCXL_OK == get_afu_by_path("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ASSERT(afu != 0);
	ASSERT(!strcmp(ocxl_afu_get_device_path(afu), "/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0"));
	ocxl_afu_close(afu);

	afu = 0;

	ASSERT(0 == symlink("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", symlink_path));

	ASSERT(OCXL_OK == get_afu_by_path(symlink_path, &afu));
	ASSERT(afu != 0);
	ASSERT(!strcmp(ocxl_afu_get_device_path(afu), "/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0"));

	test_stop(SUCCESS);

end:
	ocxl_enable_messages(OCXL_ERRORS);
	if (afu) {
		ocxl_afu_close(afu);
	}

	unlink(symlink_path);
}

/**
 * Check ocxl_afu_open
 */
static void test_afu_open() {
	test_start("AFU", "afu_open");

	ocxl_afu_h afu = OCXL_INVALID_AFU;

	ASSERT(OCXL_OK == get_afu_by_path("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu *my_afu = (ocxl_afu *)afu;
	ASSERT(my_afu->fd == -1);
	ASSERT(my_afu->epoll_fd == -1);

	ASSERT(OCXL_OK == afu_open(afu));

	ASSERT(my_afu->fd != -1);
	ASSERT(my_afu->epoll_fd != -1);

	ASSERT(ocxl_afu_get_event_fd(afu) != -1);
	ASSERT(ocxl_mmio_get_fd(afu, OCXL_GLOBAL_MMIO) != -1);
	ASSERT(ocxl_mmio_get_fd(afu, OCXL_PER_PASID_MMIO) != -1);

	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_close(afu);
	}
}

/**
 * Check ocxl_afu_open_specific
 */
static void test_ocxl_afu_open_specific() {
	ocxl_afu_h afu = OCXL_INVALID_AFU - 1;

	test_start("AFU", "ocxl_afu_open_specific");

	ocxl_enable_messages(OCXL_NO_MESSAGES);
	ASSERT(OCXL_NO_DEV == ocxl_afu_open_specific("nonexistent", NULL, -1, &afu));
	ASSERT(afu == OCXL_INVALID_AFU);
	ASSERT(OCXL_NO_DEV == ocxl_afu_open_specific("IBM,Dummy", "0001:00:00.2", -1, &afu));
	ASSERT(OCXL_NO_DEV == ocxl_afu_open_specific("IBM,Dummy", "0001:00:00.1", 1, &afu));
	ocxl_enable_messages(OCXL_ERRORS);
	ASSERT(OCXL_OK == ocxl_afu_open_specific("IBM,Dummy", "0001:00:00.1", 0, &afu));
	ASSERT(afu != OCXL_INVALID_AFU);
	ASSERT(!strcmp(ocxl_afu_get_device_path(afu), "/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0"));
	ocxl_afu_close(afu);
	afu = OCXL_INVALID_AFU;

	ASSERT(OCXL_OK == ocxl_afu_open_specific("IBM,Dummy", "0001:00:00.1", -1, &afu));
	ASSERT(afu != OCXL_INVALID_AFU);
	ASSERT(!strcmp(ocxl_afu_get_device_path(afu), "/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0"));

	ocxl_afu *my_afu = (ocxl_afu *)afu;
	ASSERT(my_afu->version_major == 5);
	ASSERT(my_afu->version_minor == 10);
	ASSERT(my_afu->global_mmio.length == 32*1024*1024);
	ASSERT(my_afu->per_pasid_mmio.length == 16384);
	ASSERT(my_afu->pasid == 1234);

	test_stop(SUCCESS);

end:
	ocxl_enable_messages(OCXL_ERRORS);
	if (afu) {
		ocxl_afu_close(afu);
	}
}

/**
 * Check ocxl_afu_open_from_dev
 */
static void test_ocxl_afu_open_from_dev() {
	test_start("AFU", "ocxl_afu_open_from_dev");

	ocxl_afu_h afu = OCXL_INVALID_AFU;

	ocxl_enable_messages(OCXL_NO_MESSAGES);
	ASSERT(OCXL_NO_DEV == ocxl_afu_open_from_dev("/nonexistent", &afu));
	ocxl_enable_messages(OCXL_ERRORS);

	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu *my_afu = (ocxl_afu *)afu;
	ASSERT(my_afu->fd != -1);
	ASSERT(my_afu->epoll_fd != -1);
	ASSERT(!strcmp(ocxl_afu_get_device_path(afu), "/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0"));

	test_stop(SUCCESS);

end:
	ocxl_enable_messages(OCXL_ERRORS);
	if (afu) {
		ocxl_afu_close(afu);
	}
}

/**
 * Check AFU getters
 */
static void test_afu_getters() {
	test_start("AFU", "getters");

	ocxl_afu_h afu = OCXL_INVALID_AFU;
	ASSERT(OCXL_OK == ocxl_afu_open("IBM,Dummy", &afu));
	const ocxl_identifier *identifier = ocxl_afu_get_identifier(afu);
	ASSERT(identifier->afu_index == 0);
	ASSERT(!strcmp(identifier->afu_name, "IBM,Dummy"));
	ASSERT(!strcmp(ocxl_afu_get_device_path(afu), "/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0"));
	char expected_path[PATH_MAX];
	snprintf(expected_path, sizeof(expected_path), "%s/IBM,Dummy.0001:00:00.1.0", ocxl_sysfs_path);
	ASSERT(!strcmp(ocxl_afu_get_sysfs_path(afu), expected_path));

	uint8_t major, minor;
	ocxl_afu_get_version(afu, &major, &minor);
	ASSERT(major == 5);
	ASSERT(minor == 10);
	ASSERT(ocxl_afu_get_event_fd(afu) >= 0);
	ASSERT(ocxl_mmio_size(afu, OCXL_GLOBAL_MMIO) == 32*1024*1024);
	ASSERT(ocxl_mmio_size(afu, OCXL_PER_PASID_MMIO) == 16384);
	ASSERT(ocxl_afu_get_pasid(afu) == 1234);
	ASSERT(ocxl_afu_get_event_fd(afu) != -1);

	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_close(afu);
	}
}

/**
 * Check ocxl_afu_open
 */
static void test_ocxl_afu_open() {
	test_start("AFU", "ocxl_afu_open");

	ocxl_afu_h afu = OCXL_INVALID_AFU;

	ocxl_enable_messages(OCXL_NO_MESSAGES);
	ASSERT(OCXL_NO_DEV == ocxl_afu_open("nonexistent", &afu));
	ocxl_enable_messages(OCXL_ERRORS);

	ASSERT(OCXL_OK == ocxl_afu_open("IBM,Dummy", &afu));
	ocxl_afu *my_afu = (ocxl_afu *)afu;
	ASSERT(my_afu->fd != -1);
	ASSERT(my_afu->epoll_fd != -1);
	ASSERT(!strcmp(ocxl_afu_get_device_path(afu), "/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0"));

	test_stop(SUCCESS);

end:
	ocxl_enable_messages(OCXL_ERRORS);
	if (afu) {
		ocxl_afu_close(afu);
	}
}

/**
 * Check ocxl_afu_attach
 */
static void test_ocxl_afu_attach() {
	test_start("AFU", "ocxl_afu_attach");

	ocxl_afu_h afu = OCXL_INVALID_AFU;

	ASSERT(!afu_is_attached());
	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu_enable_messages(afu, OCXL_ERRORS);
	ASSERT(OCXL_OK == ocxl_afu_attach(afu));
	ASSERT(afu_is_attached());

	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_close(afu);
	}
}

/**
 * Check ocxl_afu_close
 */
static void test_ocxl_afu_close() {
	test_start("AFU", "ocxl_afu_close");

	ocxl_afu_h afu = OCXL_INVALID_AFU;
	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu_enable_messages(afu, OCXL_ERRORS);
	ASSERT(OCXL_OK == ocxl_afu_close(afu));
	ASSERT(afu != 0);

	/* We deliberately dereference the freed ocxl_afu here to ensure the contents have
	 * been cleared appropriately. The data should not have been reallocated and overwritten yet
	 */
	ocxl_afu *my_afu = (ocxl_afu *)afu;
	ASSERT(0 == my_afu->irq_count);
	ASSERT(0 == my_afu->irq_max_count);
	ASSERT(0 == my_afu->epoll_event_count);
	ASSERT(-1 == my_afu->fd);
	ASSERT(-1 == my_afu->epoll_fd);
	ASSERT(NULL == my_afu->device_path);
	ASSERT(NULL == my_afu->sysfs_path);

	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_close(afu);
	}
}

/**
 * Check ocxl_mmio_map/unmap
 */
static void test_ocxl_mmio_map() {
	test_start("MMIO", "ocxl_mmio_map/unmap");

	ocxl_afu_h afu = OCXL_INVALID_AFU;
	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu_enable_messages(afu, OCXL_ERRORS);
	ocxl_afu *my_afu = (ocxl_afu *)afu;

	ASSERT(my_afu->global_mmio_fd != -1);
	ASSERT(my_afu->global_mmio.start == NULL);
	ASSERT(my_afu->global_mmio.length == GLOBAL_MMIO_SIZE);

	ocxl_mmio_h global_mmio;
	ASSERT(OCXL_OK == ocxl_mmio_map(afu, OCXL_GLOBAL_MMIO, &global_mmio));
	ASSERT(my_afu->global_mmio_fd != -1);
	void *addr;
	size_t size;
	ASSERT(OCXL_OK == ocxl_mmio_get_info(global_mmio, &addr, &size));
	ASSERT(addr != NULL);
	ASSERT(size == GLOBAL_MMIO_SIZE);

	ocxl_mmio_unmap(global_mmio);
	ASSERT(my_afu->global_mmio_fd != -1); // FD left open for further use
	ASSERT(my_afu->mmios[0].start == NULL);

	ASSERT(OCXL_OK == ocxl_afu_close(afu));
	ASSERT(my_afu->global_mmio_fd == -1);
	ASSERT(my_afu->global_mmio.start == NULL);
	ASSERT(my_afu->global_mmio.length == GLOBAL_MMIO_SIZE);

	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_close(afu);
	}
}

/**
 * Check mmio_check
 */
static void test_mmio_check() {
	test_start("MMIO", "mmio_check");

	ocxl_afu_h afu = OCXL_INVALID_AFU;
	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu *my_afu = (ocxl_afu *)afu;

	ASSERT(my_afu->global_mmio_fd != -1);
	ASSERT(my_afu->global_mmio.start == NULL);
	ASSERT(my_afu->global_mmio.length == GLOBAL_MMIO_SIZE);

	ocxl_afu_enable_messages(afu, OCXL_NO_MESSAGES);

	ASSERT(OCXL_INVALID_ARGS == mmio_check(0, 0, 4));

	ocxl_mmio_h global_mmio;
	ASSERT(OCXL_OK == ocxl_mmio_map(afu, OCXL_GLOBAL_MMIO, &global_mmio));
	ASSERT(OCXL_OUT_OF_BOUNDS == mmio_check(global_mmio, GLOBAL_MMIO_SIZE + 8, 4));
	ocxl_afu_enable_messages(afu, OCXL_ERRORS);

	ASSERT(OCXL_OK == mmio_check(global_mmio, 0, 4));

	test_stop(SUCCESS);

end:
	ocxl_afu_enable_messages(afu, OCXL_ERRORS);
	if (afu) {
		ocxl_afu_close(afu);
	}
}

/**
 * Check read32/write32_native
 */
static void test_ocxl_mmio_read32_native() {
	test_start("MMIO", "ocxl_mmio_read32/write32_native");

	ocxl_afu_h afu = OCXL_INVALID_AFU;
	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu_enable_messages(afu, OCXL_ERRORS);

	ocxl_mmio_h global_mmio;
	ASSERT(OCXL_OK == ocxl_mmio_map(afu, OCXL_GLOBAL_MMIO, &global_mmio));

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 4) {
		ASSERT(OCXL_OK == mmio_write32_native(global_mmio, offset, offset));
	}

	bool good = true;

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 4) {
		uint32_t val;
		ASSERT(OCXL_OK == mmio_read32_native(global_mmio, offset, &val));
		if (val != offset) {
			good = false;
			break;
		}
	}
	ASSERT(good);

	ocxl_mmio_unmap(global_mmio);

	test_stop(SUCCESS);

end:
	ocxl_afu_enable_messages(afu, OCXL_ERRORS);
	if (afu) {
		ocxl_afu_close(afu);
	}
}

/**
 * Check read64/write64_native
 */
static void test_ocxl_mmio_read64_native() {
	test_start("MMIO", "ocxl_mmio_read64/write64_native");

	ocxl_afu_h afu = OCXL_INVALID_AFU;
	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu_enable_messages(afu, OCXL_ERRORS);

	ocxl_mmio_h global_mmio;
	ASSERT(OCXL_OK == ocxl_mmio_map(afu, OCXL_GLOBAL_MMIO, &global_mmio));

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 8) {
		ASSERT(OCXL_OK == mmio_write64_native(global_mmio, offset, offset));
	}

	bool good = true;

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 8) {
		uint64_t val;
		ASSERT(OCXL_OK == mmio_read64_native(global_mmio, offset, &val));
		if (val != offset) {
			good = false;
			break;
		}
	}
	ASSERT(good);

	ocxl_mmio_unmap(global_mmio);

	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_close(afu);
	}
}

/**
 * Check read32/write32
 */
static void test_ocxl_mmio_read32() {
	test_start("MMIO", "ocxl_mmio_read32/write32");

	ocxl_afu_h afu = OCXL_INVALID_AFU;
	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));

	ocxl_mmio_h global_mmio;
	ASSERT(OCXL_OK == ocxl_mmio_map(afu, OCXL_GLOBAL_MMIO, &global_mmio));
	ocxl_mmio_area *mmio = (ocxl_mmio_area *)global_mmio;

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 4) {
		ASSERT(OCXL_OK == ocxl_mmio_write32(global_mmio, offset, OCXL_MMIO_BIG_ENDIAN, offset));
	}

	bool good = true;

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 4) {
		uint32_t val;
		ASSERT(OCXL_OK == ocxl_mmio_read32(global_mmio, offset, OCXL_MMIO_BIG_ENDIAN, &val));
		if (val != offset) {
			good = false;
			break;
		}
	}
	ASSERT(good);

	uint32_t big = *(uint32_t *)(mmio->start + 4);
	ASSERT(4 == be32toh(big));

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 4) {
		ASSERT(OCXL_OK == ocxl_mmio_write32(global_mmio, offset, OCXL_MMIO_LITTLE_ENDIAN, offset));
	}

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 4) {
		uint32_t val;
		ASSERT(OCXL_OK == ocxl_mmio_read32(global_mmio, offset, OCXL_MMIO_LITTLE_ENDIAN, &val));
		if (val != offset) {
			good = false;
			break;
		}
	}
	ASSERT(good);

	uint32_t little = *(uint32_t *)(mmio->start + 4);
	ASSERT(4 == le32toh(little));
	ASSERT(big != little);

	ocxl_mmio_unmap(global_mmio);

	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_close(afu);
	}
}

/**
 * Check read64/write64
 */
static void test_ocxl_mmio_read64() {
	test_start("MMIO", "ocxl_mmio_read64/write64");

	ocxl_afu_h afu = OCXL_INVALID_AFU;
	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu_enable_messages(afu, OCXL_ERRORS);

	ocxl_mmio_h global_mmio;
	ASSERT(OCXL_OK == ocxl_mmio_map(afu, OCXL_GLOBAL_MMIO, &global_mmio));
	ocxl_mmio_area *mmio = (ocxl_mmio_area *)global_mmio;

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 8) {
		ASSERT(OCXL_OK == ocxl_mmio_write64(global_mmio, offset, OCXL_MMIO_BIG_ENDIAN, offset));
	}

	bool good = true;

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 8) {
		uint64_t val;
		ASSERT(OCXL_OK == ocxl_mmio_read64(global_mmio, offset, OCXL_MMIO_BIG_ENDIAN, &val));
		if (val != offset) {
			good = false;
			break;
		}
	}
	ASSERT(good);

	uint64_t big = *(uint64_t *)(mmio->start + 8);
	ASSERT(8 == be64toh(big));

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 8) {
		ASSERT(OCXL_OK == ocxl_mmio_write64(global_mmio, offset, OCXL_MMIO_LITTLE_ENDIAN, offset));
	}

	for (int offset = 0; offset < GLOBAL_MMIO_SIZE; offset += 8) {
		uint64_t val;
		ASSERT(OCXL_OK == ocxl_mmio_read64(global_mmio, offset, OCXL_MMIO_LITTLE_ENDIAN, &val));
		if (val != offset) {
			good = false;
			break;
		}
	}
	ASSERT(good);

	uint64_t little = *(uint64_t *)(mmio->start + 8);
	ASSERT(8 == le64toh(little));

	ASSERT(big != little);

	ocxl_mmio_unmap(global_mmio);

	test_stop(SUCCESS);

end:
	ocxl_afu_enable_messages(afu, OCXL_ERRORS);
	if (afu) {
		ocxl_afu_close(afu);
	}
}

/**
 * Check read_afu_event
 */
static void test_read_afu_event() {
	test_start("IRQ", "read_afu_event");

	ocxl_afu_h afu = OCXL_INVALID_AFU;

	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu_enable_messages(afu, OCXL_ERRORS);
	ASSERT(OCXL_OK == ocxl_afu_attach(afu));

	ocxl_afu *my_afu = (ocxl_afu *)afu;
	ocxl_event event;
	int last = 0;
	ASSERT(OCXL_EVENT_ACTION_NONE == read_afu_event(my_afu, 0, &event, &last));
	ASSERT(last);

#ifdef _ARCH_PPC64
	force_translation_fault((void *)0xfeeddeadbeeff00d, 0x123456789abcdef0, 16);
#else
	force_translation_fault((void *)0xfeeddeadbeeff00d, 16);
#endif

	last = 0;
	ASSERT(OCXL_EVENT_ACTION_SUCCESS == read_afu_event(my_afu, 0, &event, &last));
	ASSERT(last);
	ASSERT(event.type == OCXL_EVENT_TRANSLATION_FAULT);
	ASSERT(event.translation_fault.addr == (void *)0xfeeddeadbeeff00d);
#ifdef _ARCH_PPC64
	ASSERT(event.translation_fault.dsisr == 0x123456789abcdef0);
#endif
	ASSERT(event.translation_fault.count == 16);

#ifdef _ARCH_PPC64
	force_translation_fault((void *)0xfeeddeadbeeff00d, 0x123456789abcdef0, 16);
#else
	force_translation_fault((void *)0xfeeddeadbeeff00d, 16);
#endif

	last = 0;
	ASSERT(OCXL_EVENT_ACTION_SUCCESS == read_afu_event(my_afu, 0, &event, &last));
	ASSERT(last);
	ASSERT(event.type == OCXL_EVENT_TRANSLATION_FAULT);
	ASSERT(event.translation_fault.addr == (void *)0xfeeddeadbeeff00d);
#ifdef _ARCH_PPC64
	ASSERT(event.translation_fault.dsisr == 0x123456789abcdef0);
#endif
	ASSERT(event.translation_fault.count == 16);

	last = 0;
	ASSERT(OCXL_EVENT_ACTION_NONE == read_afu_event(my_afu, 0, &event, &last));
	ASSERT(last);

	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_close(afu);
	}
}


#ifdef __UNUSED
/**
 * Check ocxl_afu_event_check_versioned (with kernel events)
 */
static void test_ocxl_afu_event_check_versioned() {
	test_start("IRQ", "ocxl_afu_event_check_versioned (kernel)");

	ocxl_afu_h afu = OCXL_INVALID_AFU;

	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ASSERT(OCXL_OK == ocxl_afu_attach(afu));

#define EVENT_COUNT 5
	ocxl_event events[EVENT_COUNT];

	ASSERT(0 == ocxl_afu_event_check_versioned(afu, 10, events, EVENT_COUNT, 0));

#ifdef _ARCH_PPC64
	force_translation_fault((void *)0xfeeddeadbeeff00d, 0x123456789abcdef0, 16);
#else
	force_translation_fault((void *)0xfeeddeadbeeff00d, 16);
#endif

	ASSERT(1 == ocxl_afu_event_check_versioned(afu, 10, events, EVENT_COUNT, 0));
	ASSERT(events[0].type == OCXL_EVENT_TRANSLATION_FAULT);
	ASSERT(events[0].translation_fault.addr == (void *)0xfeeddeadbeeff00d);
#ifdef _ARCH_PPC64
	ASSERT(events[0].translation_fault.dsisr == 0x123456789abcdef0);
#endif
	ASSERT(events[0].translation_fault.count == 16);

	ASSERT(0 == ocxl_afu_event_check_versioned(afu, 0, events, EVENT_COUNT, 0));

	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_close(afu);
	}
}
#endif

#define MAX_MESSAGE_LENGTH 255 // From internal.c
char err_buf[MAX_MESSAGE_LENGTH];
static void copy_to_err_buf(ocxl_err error, const char *message) {
	snprintf(err_buf, sizeof(err_buf), "%s: %s", ocxl_err_to_string(error), message);
}

/**
 * Test the error reporting callback (open calls)
 */
static void test_ocxl_set_error_message_handler() {
	test_start("ERR", "ocxl_set_error_message_handler");

	memset(err_buf, '\0', sizeof(err_buf));
	ocxl_set_error_message_handler(copy_to_err_buf);
	ocxl_enable_messages(OCXL_ERRORS);

	const char *message = "error message test";
	errmsg(NULL, OCXL_INTERNAL_ERROR, "%s", message);

	ASSERT(!strcmp("Internal error: error message test", err_buf));

	test_stop(SUCCESS);

end:
	ocxl_set_error_message_handler(ocxl_default_error_handler);
}

static void copy_to_err_buf_afu(ocxl_afu_h afu, ocxl_err error, const char *message) {
	const ocxl_identifier *id = ocxl_afu_get_identifier(afu);
	uint8_t major, minor;

	ocxl_afu_get_version(afu, &major, &minor);

	snprintf(err_buf, sizeof(err_buf), "%s(%d,%d): %s: %s",
			id->afu_name, major, minor, ocxl_err_to_string(error), message);
}

/**
 * Test the error reporting callback (AFU calls)
 */
static void test_ocxl_set_afu_error_message_handler() {
	test_start("ERR", "ocxl_set_afu_error_message_handler");

	memset(err_buf, '\0', sizeof(err_buf));

	ocxl_afu_h afu = OCXL_INVALID_AFU;
	ASSERT(OCXL_OK == ocxl_afu_open_from_dev("/dev/ocxl-test/IBM,Dummy.0001:00:00.1.0", &afu));
	ocxl_afu *my_afu = (ocxl_afu *)afu;

	ocxl_afu_set_error_message_handler(afu, copy_to_err_buf_afu);
	ocxl_afu_enable_messages(afu, OCXL_ERRORS);

	const char *message = "error message test";
	errmsg(my_afu, OCXL_INTERNAL_ERROR, "%s", message);

	ASSERT(!strcmp("IBM,Dummy(5,10): Internal error: error message test", err_buf));

	test_stop(SUCCESS);

end:
	if (afu) {
		ocxl_afu_close(afu);
	}
}

static void exit_handler() {
	void *ret;

	if (afu_thread) {
		stop_afu();
		pthread_kill(afu_thread, SIGTERM);
		pthread_join(afu_thread, &ret);
		term_afu();
	}
}

int main(int args, const char **argv) {
	struct sigaction sa;
	sa.sa_handler = exit_handler;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;
	(void)sigaction(SIGINT, &sa, NULL);

	ocxl_set_sys_path(ocxl_sysfs_path);
	ocxl_set_dev_path(ocxl_dev_path);

	ocxl_enable_messages(OCXL_ERRORS);

	struct stat sysfs_stat;
	if (stat(ocxl_sysfs_path, &sysfs_stat)) {
		if (mkdir(ocxl_sysfs_path, 0775)) {
			fprintf(stderr, "Could not mkdir '%s': %d: %s\n",
					ocxl_sysfs_path, errno, strerror(errno));
		}
	}

	test_afu_init();
	test_ocxl_afu_alloc();
	test_device_matches();

	create_afu();
	sleep(1);

	test_populate_metadata();
	test_afu_getters();
	test_get_afu_by_path();
	test_afu_open();
	test_ocxl_afu_open_specific();
	test_ocxl_afu_open_from_dev();
	test_ocxl_afu_open();
	test_ocxl_afu_attach();
	test_ocxl_afu_close();

	test_ocxl_set_error_message_handler();
	test_ocxl_set_afu_error_message_handler();

	test_ocxl_mmio_map();
	test_mmio_check();
	test_ocxl_mmio_read32_native();
	test_ocxl_mmio_read64_native();
	test_ocxl_mmio_read32();
	test_ocxl_mmio_read64();

	test_read_afu_event();
	// Disabled as we need epoll support in CUSE to test this
	// test_ocxl_afu_event_check_versioned();

	exit_handler();

	if (test_report()) {
		return 1;
	}

	return 0;
}
