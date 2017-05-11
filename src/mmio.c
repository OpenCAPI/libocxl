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
#include "sys/mman.h"
#include "errno.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/**
 * @defgroup ocxl_mmio OpenCAPI MMIO Functions
 *
 * The MMIO functions map the global and per-PASID MMIO spaces of the AFU into
 * the address space of the process, as well as moderating access to them.
 *
 * The endianess of an MMIO space is defined when mapped, and access via libocxl
 * will automatically translate host-endian data to the appropriate format.
 *
 * Only 32bit & 64bit accesses are supported.
 *
 * @{
 */

/**
 * map the Global MMIO area of an AFU to memory
 *
 * Map the Global MMIO area of afu to the current process memory, and declare
 * the AFU endianness. The size  and contents of this area are specific each
 * AFU. The size can be discovered with ocxl_get_global_mmio_size().
 *
 * Subsequent ocxl_global_mmio_read32(), ocxl_global_mmio_read64(),
 * ocxl_global_mmio_write32() and ocxl_global_mmio_write64() will honor the
 * endianess and swap bytes on behalf of the application when required.
 *
 * @param afu the AFU to operate on
 * @param endian the endianess of the MMIO area
 * @pre the AFU has been opened and attached
 * @post the area struct is populated with the MMIO information
 * @retval OCXL_OK on success
 * @retval OCXL_NO_MEM if there is insufficient memory
 * @retval OCXL_NO_DEV if the MMIO device could not be opened
 * @retval OCXL_NO_CONTEXT if the AFU has not been opened & attached
 * @retval OCXL_ALREADY_DONE if the global MMIO area has already been mapped
 */
ocxl_err ocxl_global_mmio_map(ocxl_afu_h afu, ocxl_endian endian)
{
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	if (my_afu->global_mmio.length == 0) {
		return OCXL_OK;
	}

	if (my_afu->global_mmio_fd != -1) {
		return OCXL_ALREADY_DONE;
	}

	char path[PATH_MAX + 1];
	int length = snprintf(path, sizeof(path), "%s/global_mmio_area", my_afu->sysfs_path);
	if (length >= sizeof(path)) {
		errmsg("global MMIO path truncated for AFU '%s'", my_afu->identifier.afu_name);
		return OCXL_NO_DEV;
	}

	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		errmsg("Could not open global MMIO for AFU '%s': Error %d: %s", path, errno, strerror(errno));
		return OCXL_NO_DEV;
	}

	my_afu->global_mmio_fd = fd;

	void *addr = mmap(NULL, my_afu->per_pasid_mmio.length, PROT_READ | PROT_WRITE,
	                  MAP_SHARED, my_afu->global_mmio_fd, 0);
	if (addr == MAP_FAILED) {
		close(my_afu->global_mmio_fd);
		my_afu->global_mmio_fd = -1;
		errmsg("Could not map global MMIO on AFU '%s', %d: %s",
		       my_afu->identifier.afu_name, errno, strerror(errno));
		return OCXL_NO_MEM;
	}

	my_afu->global_mmio.start = addr;
	my_afu->global_mmio.endianess = endian;

	return OCXL_OK;
}

/**
 * map the per-PASID MMIO area of an AFU to memory
 *
 * Map the per-PASID MMIO area of afu to the current process memory, and
 * declares AFU endianness according to flag. The size  and contents of this
 * area are specific each AFU.
 *
 * Subsequent ocxl_mmio_read32(), ocxl_mmio_read64(), ocxl_mmio_write32()
 * and ocxl_mmio_write64() will honor the endianess and swap bytes on behalf of
 * the application when required.
 *
 * @param afu the AFU to operate on
 * @param endian the endianess of the MMIO area
 * @pre the AFU has been opened and attached
 * @post the area struct is populated with the MMIO information
 * @retval OCXL_OK on success
 * @retval OCXL_NO_MEM if the map failed
 * @retval OCXL_NO_CONTEXT if the AFU has not been opened & attached
 */
ocxl_err ocxl_mmio_map(ocxl_afu_h afu, ocxl_endian endian)
{
	if (afu == OCXL_INVALID_AFU) {
		return OCXL_NO_CONTEXT;
	}
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	if (my_afu->fd < 0) {
		errmsg("Could not map per-PASID MMIO on AFU '%s' as it has not been opened",
		       my_afu->identifier.afu_name);
		return OCXL_NO_CONTEXT;
	}

	void *addr = mmap(NULL, my_afu->per_pasid_mmio.length, PROT_READ | PROT_WRITE,
	                  MAP_SHARED, my_afu->fd, 0);
	if (addr == MAP_FAILED) {
		errmsg("Could not map per-PASID MMIO on AFU '%s', %d: %s",
		       my_afu->identifier.afu_name, errno, strerror(errno));
		return OCXL_NO_MEM;
	}

	my_afu->per_pasid_mmio.start = addr;
	my_afu->per_pasid_mmio.endianess = endian;

	return OCXL_OK;
}

/**
 * Unmap an AFU Global MMIO area
 *
 * @param afu the AFU whose global MMIO should be unmapped
 */
void ocxl_global_mmio_unmap(ocxl_afu_h afu)
{
	if (afu == OCXL_INVALID_AFU) {
		return;
	}
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	if (my_afu->global_mmio.start) {
		munmap(my_afu->global_mmio.start, my_afu->global_mmio.length);
		my_afu->global_mmio.start = NULL;
	}

	if (my_afu->global_mmio_fd >= 0) {
		close(my_afu->global_mmio_fd);
		my_afu->global_mmio_fd = -1;
	}
}

/**
 * Unmap an AFU per-PASID MMIO area
 *
 * @param afu the AFU whose per-PASID MMIO should be unmapped
 */
void ocxl_mmio_unmap(ocxl_afu_h afu)
{
	if (afu == OCXL_INVALID_AFU) {
		return;
	}
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	if (my_afu->per_pasid_mmio.start) {
		munmap(my_afu->per_pasid_mmio.start, my_afu->per_pasid_mmio.length);
		my_afu->per_pasid_mmio.start = NULL;
	}
}

/**
 * Validate an MMIO operation
 * @param afu the AFU to operate on
 * @param global true for global, false for per-pasid MMIO
 * @param offset the offset within the MMIO area
 * @param size the size of the operation
 * @retval OCXL_OK if the operation can proceed
 * @retval OCXL_NO_CONTEXT if the MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
inline static ocxl_err mmio_check(ocxl_afu * afu, bool global, size_t offset, size_t size)
{
	ocxl_mmio_area *mmio = global ? &afu->global_mmio : &afu->per_pasid_mmio;

	if (mmio->start == NULL) {
		errmsg("%s MMIO area for AFU '%s' has not been mapped",
		       global ? "Global" : "Per-PASID", afu->identifier.afu_name);
		return OCXL_NO_CONTEXT;
	}

	if (offset >= mmio->length - (size - 1)) {
		errmsg("%s MMIO access of 0x%lx for AFU '%s' exceeds limit of 0x%lx",
		       global ? "Global" : "Per-PASID", offset, afu->identifier.afu_name, mmio->length);
		return OCXL_OUT_OF_BOUNDS;
	}

	return OCXL_OK;
}

/**
 * Read a 32 bit unsigned int from an MMIO area
 * @param mmio the MMIO area to read from
 * @param offset the offset in the MMIO area
 */
inline static uint32_t read32(ocxl_mmio_area * mmio, size_t offset)
{
	uint32_t val;

	__asm__ __volatile__("lwz%U1%X1 %0,%1; sync"
	                     : "=r"(val)
	                     : "m"(*(__u64 *)(mmio->start + offset)));

	switch (mmio->endianess) {
	case OCXL_MMIO_HOST_ENDIAN:
		break;
	case OCXL_MMIO_LITTLE_ENDIAN:
		val = le32toh(val);
		break;
	case OCXL_MMIO_BIG_ENDIAN:
		val = be32toh(val);
		break;
	}

	return val;
}

/**
 * Read a 64 bit unsigned int from an MMIO area
 * @param mmio the MMIO area to read from
 * @param offset the offset in the MMIO area
 */
inline static uint64_t read64(ocxl_mmio_area * mmio, size_t offset)
{
	uint64_t val;

	__asm__ __volatile__("ld%U1%X1 %0,%1; sync"
	                     : "=r"(val)
	                     : "m"(*(__u64 *)(mmio->start + offset)));

	switch (mmio->endianess) {
	case OCXL_MMIO_HOST_ENDIAN:
		break;
	case OCXL_MMIO_LITTLE_ENDIAN:
		val = le64toh(val);
		break;
	case OCXL_MMIO_BIG_ENDIAN:
		val = be64toh(val);
		break;
	}

	return val;
}

/**
 * Write a 32 bit unsigned int to an MMIO area
 * @param mmio the MMIO area to read from
 * @param offset the offset in the MMIO area
 * @param value the value to write
 */
inline static void write32(ocxl_mmio_area * mmio, size_t offset, uint32_t value)
{
	switch (mmio->endianess) {
	case OCXL_MMIO_HOST_ENDIAN:
		break;
	case OCXL_MMIO_LITTLE_ENDIAN:
		value = htole32(value);
		break;
	case OCXL_MMIO_BIG_ENDIAN:
		value = htobe32(value);
		break;
	}

	__asm__ __volatile__("sync ; stw%U0%X0 %1,%0"
	                     : "=m"(*(__u64 *)(mmio->start + offset))
	                     : "r"(value));
}

/**
 * Write a 64 bit unsigned int to an MMIO area
 * @param mmio the MMIO area to read from
 * @param offset the offset in the MMIO area
 * @param value the value to write
 */
inline static void write64(ocxl_mmio_area * mmio, size_t offset, uint64_t value)
{
	switch (mmio->endianess) {
	case OCXL_MMIO_HOST_ENDIAN:
		break;
	case OCXL_MMIO_LITTLE_ENDIAN:
		value = htole64(value);
		break;
	case OCXL_MMIO_BIG_ENDIAN:
		value = htobe64(value);
		break;
	}

	__asm__ __volatile__("sync ; std%U0%X0 %1,%0"
	                     : "=m"(*(__u64 *)(mmio->start + offset))
	                     : "r"(value));
}

/**
 * read a 32-bit word from the mapped AFU Global MMIO space
 *
 * Read the 32-bit word at offset from the address of the mapped MMIO
 * space. The return value will include byte swapping if the AFU endianness
 * declared to ocxl_global_mmio_map() differs from the host endianness.
 *
 * @param afu the AFU to operate on
 * @param offset A byte address that is aligned on a word (4 byte) boundary. It
 * must be lower than the MMIO size (-4 bytes) reported by ocxl_afu_get_global_mmio_size
 * @param[out] out the value that was read
 *
 * @retval OCXL_OK if the value was read
 * @retval OCXL_NO_CONTEXT if the global MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
ocxl_err ocxl_global_mmio_read32(ocxl_afu_h afu, size_t offset, uint32_t * out)
{
	if (afu == OCXL_INVALID_AFU) {
		return OCXL_NO_CONTEXT;
	}
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	ocxl_err ret = mmio_check(my_afu, true, offset, 4);
	if (ret != OCXL_OK) {
		return ret;
	}

	*out = read32(&my_afu->global_mmio, offset);

	return OCXL_OK;
}

/**
 * read a 64-bit word from the mapped AFU Global MMIO space
 *
 * Read the 64-bit word at offset from the address of the mapped Global MMIO
 * space. The return value will include byte swapping if the AFU endianness
 * declared to ocxl_global_mmio_map() differs from the host endianness.
 *
 * @param afu the AFU to operate on
 * @param offset A byte address that is aligned on a double word (8 byte) boundary. It
 * must be lower than the MMIO size (-8 bytes) reported by ocxl_afu_get_mmio_size()
 * @param[out] out the value that was read
 *
 * @retval OCXL_OK if the value was read
 * @retval OCXL_NO_CONTEXT if the global MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
ocxl_err ocxl_global_mmio_read64(ocxl_afu_h afu, size_t offset, uint64_t * out)
{
	if (afu == OCXL_INVALID_AFU) {
		return OCXL_NO_CONTEXT;
	}
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	ocxl_err ret = mmio_check(my_afu, true, offset, 8);
	if (ret != OCXL_OK) {
		return ret;
	}

	*out = read64(&my_afu->global_mmio, offset);

	return OCXL_OK;
}

/**
 * write a 32-bit word to the mapped AFU Global MMIO space
 *
 * Write the 32-bit word at offset from the address of the mapped Global MMIO
 * space. The write will include byte swapping if the AFU endianness declared to
 * ocxl_global_mmio_map() differs from the host endianness.
 *
 * @param afu the AFU to operate on
 * @param offset A byte address that is aligned on a word (4 byte) boundary. It
 * must be lower than the MMIO size (-4 bytes) reported by ocxl_afu_get_global_mmio_size
 * @param value the value to write
 *
 * @retval OCXL_OK if the value was written
 * @retval OCXL_NO_CONTEXT if the global MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
ocxl_err ocxl_global_mmio_write32(ocxl_afu_h afu, size_t offset, uint32_t value)
{
	if (afu == OCXL_INVALID_AFU) {
		return OCXL_NO_CONTEXT;
	}
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	ocxl_err ret = mmio_check(my_afu, true, offset, 4);
	if (ret != OCXL_OK) {
		return ret;
	}

	write32(&my_afu->global_mmio, offset, value);

	return OCXL_OK;
}

/**
 * write a 64-bit word to the mapped AFU Global MMIO space
 *
 * Write the 64-bit word at offset from the address of the mapped Global MMIO
 * space. The write will include byte swapping if the AFU endianness declared to
 * ocxl_global_mmio_map() differs from the host endianness.
 *
 * @param afu the AFU to operate on
 * @param offset A byte address that is aligned on a double word (8 byte) boundary. It
 * must be lower than the MMIO size (-8 bytes) reported by ocxl_afu_get_mmio_size()
 * @param value the value to write
 *
 * @retval OCXL_OK if the value was written
 * @retval OCXL_NO_CONTEXT if the global MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
ocxl_err ocxl_global_mmio_write64(ocxl_afu_h afu, size_t offset, uint64_t value)
{
	if (afu == OCXL_INVALID_AFU) {
		return OCXL_NO_CONTEXT;
	}
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	ocxl_err ret = mmio_check(my_afu, true, offset, 8);
	if (ret != OCXL_OK) {
		return ret;
	}

	write64(&my_afu->global_mmio, offset, value);

	return OCXL_OK;
}

/**
 * read a 32-bit word from the mapped AFU per-PASID MMIO space
 *
 * Read the 32-bit word at offset from the address of the mapped MMIO
 * space. The return value will include byte swapping if the AFU endianness
 * declared to ocxl_mmio_map() differs from the host endianness.
 *
 * @param afu the AFU to operate on
 * @param offset A byte address that is aligned on a word (4 byte) boundary. It
 * must be lower than the MMIO size (-4 bytes) reported by ocxl_afu_get_global_mmio_size
 * @param[out] out the value that was read
 *
 * @retval OCXL_OK if the value was read
 * @retval OCXL_NO_CONTEXT if the MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
ocxl_err ocxl_mmio_read32(ocxl_afu_h afu, size_t offset, uint32_t * out)
{
	if (afu == OCXL_INVALID_AFU) {
		return OCXL_NO_CONTEXT;
	}
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	ocxl_err ret = mmio_check(my_afu, true, offset, 4);
	if (ret != OCXL_OK) {
		return ret;
	}

	*out = read32(&my_afu->global_mmio, offset);

	return OCXL_OK;
}

/**
 * read a 64-bit word from the mapped AFU per-PASID MMIO space
 *
 * Read the 64-bit word at offset from the address of the mapped MMIO
 * space. The return value will include byte swapping if the AFU endianness
 * declared to ocxl_mmio_map() differs from the host endianness.
 *
 * @param afu the AFU to operate on
 * @param offset A byte address that is aligned on a double word (8 byte) boundary. It
 * must be lower than the MMIO size (-8 bytes) reported by ocxl_afu_get_mmio_size()
 * @param[out] out the value that was read
 *
 * @retval OCXL_OK if the value was read
 * @retval OCXL_NO_CONTEXT if the MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
ocxl_err ocxl_mmio_read64(ocxl_afu_h afu, size_t offset, uint64_t * out)
{
	if (afu == OCXL_INVALID_AFU) {
		return OCXL_NO_CONTEXT;
	}
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	ocxl_err ret = mmio_check(my_afu, true, offset, 8);
	if (ret != OCXL_OK) {
		return ret;
	}

	*out = read64(&my_afu->global_mmio, offset);

	return OCXL_OK;
}

/**
 * write a 32-bit word to the mapped AFU per-PASID MMIO space
 *
 * Write the 32-bit word at offset from the address of the mapped MMIO
 * space. The write will include byte swapping if the AFU endianness declared to
 * ocxl_global_mmio_map() differs from the host endianness.
 *
 * @param afu the AFU to operate on
 * @param offset A byte address that is aligned on a word (4 byte) boundary. It
 * must be lower than the MMIO size (-4 bytes) reported by ocxl_afu_get_global_mmio_size
 * @param value the value to write
 *
 * @retval OCXL_OK if the value was written
 * @retval OCXL_NO_CONTEXT if the MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
ocxl_err ocxl_mmio_write32(ocxl_afu_h afu, size_t offset, uint32_t value)
{
	if (afu == OCXL_INVALID_AFU) {
		return OCXL_NO_CONTEXT;
	}
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	ocxl_err ret = mmio_check(my_afu, true, offset, 4);
	if (ret != OCXL_OK) {
		return ret;
	}

	write32(&my_afu->global_mmio, offset, value);

	return OCXL_OK;
}

/**
 * write a 64-bit word to the mapped AFU per-PASID MMIO space
 *
 * Write the 64-bit word at offset from the address of the mapped MMIO
 * space. The write will include byte swapping if the AFU endianness declared to
 * ocxl_mmio_map() differs from the host endianness.
 *
 * @param afu the AFU to operate on
 * @param offset A byte address that is aligned on a double word (8 byte) boundary. It
 * must be lower than the MMIO size (-8 bytes) reported by ocxl_afu_get_mmio_size()
 * @param value the value to write
 *
 * @retval OCXL_OK if the value was written
 * @retval OCXL_NO_CONTEXT if the MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
ocxl_err ocxl_mmio_write64(ocxl_afu_h afu, size_t offset, uint64_t value)
{
	if (afu == OCXL_INVALID_AFU) {
		return OCXL_NO_CONTEXT;
	}
	ocxl_afu *my_afu = (ocxl_afu *) afu;

	ocxl_err ret = mmio_check(my_afu, true, offset, 8);
	if (ret != OCXL_OK) {
		return ret;
	}

	write64(&my_afu->global_mmio, offset, value);

	return OCXL_OK;
}

/**
 * @}
 */
