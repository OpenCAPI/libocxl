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

// Needed for le32toh() and friends when building against glibc version < 2.20
#define _BSD_SOURCE

#include "libocxl_internal.h"
#include "sys/mman.h"
#include "errno.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

/**
 * @defgroup ocxl_mmio OpenCAPI MMIO Functions
 *
 * The MMIO functions map the global and per-PASID MMIO spaces of the AFU into
 * the address space of the process, as well as moderating access to them.
 *
 * Only 32bit & 64bit accesses are supported.
 *
 * @{
 */

/**
 * @internal
 *
 * Save a mapped MMIO region against an AFU.
 *
 * @param afu the AFU to operate on
 * @param addr the address of the MMIO region
 * @param size the size of the MMIO region
 * @param type the type of the MMIO region
 * @param[out] handle the MMIO region handle
 *
 * @retval OCXL_OK on success
 * @retval OCXL_NO_MEM if there is insufficient memory
 */
static ocxl_err register_mmio(ocxl_afu *afu, void *addr, size_t size, ocxl_mmio_type type, ocxl_mmio_h *handle)
{
	int available_mmio = -1;

	// Look for an available MMIO region that has been unmapped
	for (uint16_t mmio = 0; mmio < afu->mmio_count; mmio++) {
		if (!afu->mmios[mmio].start) {
			available_mmio = mmio;
			break;
		}
	}

	if (available_mmio == -1) {
		if (afu->mmio_count == afu->mmio_max_count) {
			ocxl_err rc = grow_buffer(afu, (void **)&afu->mmios, &afu->mmio_max_count, sizeof(ocxl_mmio_area), INITIAL_MMIO_COUNT);
			if (rc != OCXL_OK) {
				errmsg(afu, rc, "Could not grow MMIO buffer");
				return rc;
			}
		}

		available_mmio = afu->mmio_count++;
	}

	afu->mmios[available_mmio].start = addr;
	afu->mmios[available_mmio].length = size;
	afu->mmios[available_mmio].type = type;
	afu->mmios[available_mmio].afu = afu;

	*handle = &afu->mmios[available_mmio];

	TRACE(afu, "Mapped %ld bytes of %s MMIO at %p",
	      size, type == OCXL_GLOBAL_MMIO ? "Global" : "Per-PASID", addr);

	return OCXL_OK;
}

/**
 * Open the global MMIO descriptor on an AFU.
 *
 * @param afu the AFU
 *
 * @retval OCXL_NO_DEV if the MMIO descriptor could not be opened
 */
ocxl_err global_mmio_open(ocxl_afu *afu)
{
	char path[PATH_MAX + 1];
	int length = snprintf(path, sizeof(path), "%s/global_mmio_area", afu->sysfs_path);
	if (length >= (int)sizeof(path)) {
		ocxl_err rc = OCXL_NO_DEV;
		errmsg(afu, rc, "global MMIO path truncated");
		return rc;
	}

	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ocxl_err rc = OCXL_NO_DEV;
		errmsg(afu, rc, "Could not open global MMIO '%s': Error %d: %s", path, errno, strerror(errno));
		return rc;
	}

	afu->global_mmio_fd = fd;

	return OCXL_OK;
}

/**
 * @internal
 *
 * map the Global MMIO area of an AFU to memory.
 *
 * Map the Global MMIO area of afu to the current process memory. The size and
 * contents of this area are specific each AFU. The size can be discovered with
 * ocxl_mmio_size().
 *
 * @pre the AFU has been opened
 *
 * @param afu the AFU to operate on
 * @param size the size of the MMIO region to map (or 0 to map the full region)
 * @param prot the protection parameters as per mmap/mprotect
 * @param flags Additional flags to modify the map behavior (currently unused, must be 0)
 * @param offset the offset of the MMIO region to map (or 0 to map the full region), should be a multiple of PAGE_SIZE
 * @param[out] region the MMIO region handle
 *
 * @retval OCXL_OK on success
 * @retval OCXL_NO_MEM if there is insufficient memory
 * @retval OCXL_NO_DEV if the MMIO device could not be opened
 * @retval OCXL_INVALID_ARGS if the flags are not valid
 */
static ocxl_err global_mmio_map(ocxl_afu *afu, size_t size, int prot, uint64_t flags, off_t offset,
                                ocxl_mmio_h *region) // static function extraction hack
{
	if (afu->global_mmio.length == 0) {
		ocxl_err rc = OCXL_NO_MEM;
		errmsg(afu, rc, "Cannot map Global MMIO as there is 0 bytes allocated by the AFU");
		return rc;
	}

	if (flags) {
		ocxl_err rc = OCXL_INVALID_ARGS;
		errmsg(afu, rc, "MMIO flags of 0x%llx is not supported by this version of libocxl", flags);
		return rc;
	}

	void *addr = mmap(NULL, size, prot, MAP_SHARED, afu->global_mmio_fd, offset);
	if (addr == MAP_FAILED) {
		ocxl_err rc = OCXL_NO_MEM;
		errmsg(afu, rc, "Could not map global MMIO, %d: %s", errno, strerror(errno));
		return rc;
	}

	ocxl_mmio_h mmio_region;
	ocxl_err rc = register_mmio(afu, addr, size, OCXL_GLOBAL_MMIO, &mmio_region);
	if (rc != OCXL_OK) {
		errmsg(afu, rc, "Could not register global MMIO region");
		munmap(addr, size);
		return rc;
	}

	*region = mmio_region;

	return OCXL_OK;
}

/**
 * @internal
 *
 * Map the per-PASID MMIO area of an AFU to memory.
 *
 * Map the per-PASID MMIO area of AFU to the current process memory. The size and
 * contents of this area are specific each AFU. The size can be discovered with
 * ocxl_mmio_size().
 *
 * @pre the AFU has been opened and attached
 *
 * @param afu the AFU to operate on
 * @param size the size of the MMIO region to map (or 0 to map the full region)
 * @param prot the protection parameters as per mmap/mprotect
 * @param flags Additional flags to modify the map behavior (currently unused, must be 0)
 * @param offset the offset of the MMIO region to map (or 0 to map the full region), should be a multiple of PAGE_SIZE
 * @param[out] region the MMIO region handle
 *
 * @retval OCXL_OK on success
 * @retval OCXL_NO_MEM if the map failed
 * @retval OCXL_NO_CONTEXT if the AFU has not been opened/attached
 * @retval OCXL_INVALID_ARGS if the flags are not valid
 */
static ocxl_err mmio_map(ocxl_afu *afu, size_t size, int prot, uint64_t flags, off_t offset, ocxl_mmio_h *region)
{
	if (flags) {
		ocxl_err rc = OCXL_INVALID_ARGS;
		errmsg(afu, rc, "MMIO flags of 0x%llx is not supported by this version of libocxl", flags);
		return rc;
	}

	if (afu->fd < 0) {
		ocxl_err rc = OCXL_NO_CONTEXT;
		errmsg(afu, rc, "Could not map per-PASID MMIO as the AFU has not been opened");
		return rc;
	}

	if (!afu->attached) {
		ocxl_err rc = OCXL_NO_CONTEXT;
		errmsg(afu, rc, "Could not map per-PASID MMIO as the AFU has not been attached");
		return rc;
	}

	void *addr = mmap(NULL, size, prot, MAP_SHARED, afu->fd, offset);
	if (addr == MAP_FAILED) {
		ocxl_err rc = OCXL_NO_MEM;
		errmsg(afu, rc, "Could not map per-PASID MMIO: %d: %s", errno, strerror(errno));
		return rc;
	}

	ocxl_mmio_h mmio_region;
	ocxl_err rc = register_mmio(afu, addr, size, OCXL_PER_PASID_MMIO, &mmio_region);
	if (rc != OCXL_OK) {
		errmsg(afu, rc, "Could not register global MMIO region", afu->identifier.afu_name);
		munmap(addr, size);
		return rc;
	}

	*region = mmio_region;

	return OCXL_OK;
}

/**
 * Map an MMIO area of an AFU.
 *
 * Provides finer grain control of MMIO region mapping. Allows for protection parameters
 * to be specified, as well as allowing partial mappings (with PAGE_SIZE granularity).
 *
 * @pre the AFU has been opened
 *
 * @param afu the AFU to operate on
 * @param type the type of MMIO area to map
 * @param size the size of the MMIO region to map (or 0 to map the full region)
 * @param prot the protection parameters as per mmap/mprotect
 * @param flags Additional flags to modify the map behavior (currently unused, must be 0)
 * @param offset the offset of the MMIO region to map (or 0 to map the full region), should be a multiple of PAGE_SIZE
 * @param[out] region the MMIO region handle
 *
 * @retval OCXL_OK on success
 * @retval OCXL_NO_MEM if the map failed
 * @retval OCXL_NO_CONTEXT if the AFU has not been opened
 * @retval OCXL_INVALID_ARGS if the flags are not valid
 */
ocxl_err ocxl_mmio_map_advanced(ocxl_afu_h afu, ocxl_mmio_type type, size_t size, int prot, uint64_t flags,
                                off_t offset, ocxl_mmio_h *region)
{
	ocxl_err rc = OCXL_INVALID_ARGS;

	if (size == 0) {
		switch (type) {
		case OCXL_PER_PASID_MMIO:
			size = afu->per_pasid_mmio.length;
			break;
		case OCXL_GLOBAL_MMIO:
			size = afu->global_mmio.length;
			break;
		}

		size -= offset;
	}

	switch (type) {
	case OCXL_GLOBAL_MMIO:
		if (offset + size > afu->global_mmio.length) {
			rc = OCXL_NO_MEM;
			errmsg(afu, rc, "Offset(%#x) + size(%#x) of global MMIO map request exceeds available size of %#x",
			       offset, size, afu->global_mmio.length);
			return rc;
		}
		return global_mmio_map(afu, size, prot, flags, offset, region);

	case OCXL_PER_PASID_MMIO:
		if (offset + size > afu->per_pasid_mmio.length) {
			rc = OCXL_NO_MEM;
			errmsg(afu, rc, "Offset(%#x) + size(%#x) of per-pasid MMIO map request exceeds available size of %#x",
			       offset, size, afu->global_mmio.length);
			return rc;
		}
		return mmio_map(afu, size, prot, flags, offset, region);

	default:
		errmsg(afu, rc, "Unknown MMIO type %d", type);
		return rc;
	}
}

/**
 * Map an MMIO area of an AFU.
 *
 * Maps the entire global/per-PASID region of MMIO memory on the AFU with read/write access granted.
 *
 * @pre the AFU has been opened, and if a per-PASID region is to be mapped, the AFU has been attached
 *
 * @see ocxl_afu_attach()
 *
 * @param afu the AFU to operate on
 * @param type the type of MMIO area to map
 * @param region [out] the MMIO region handle
 *
 * @retval OCXL_OK on success
 * @retval OCXL_NO_MEM if the map failed
 * @retval OCXL_NO_CONTEXT if the AFU has not been opened
 * @retval OCXL_INVALID_ARGS if the flags are not valid
 */
ocxl_err ocxl_mmio_map(ocxl_afu_h afu, ocxl_mmio_type type, ocxl_mmio_h *region)
{
	return ocxl_mmio_map_advanced(afu, type, 0, PROT_READ | PROT_WRITE, 0, 0, region);
}

/**
 * Unmap an MMIO region from an AFU.
 *
 * @pre the AFU has been opened, and the MMIO area mapped
 *
 * @param region the MMIO region to unmap
 */
void ocxl_mmio_unmap(ocxl_mmio_h region)
{
	if (!region->start) {
		return;
	}

	munmap(region->start, region->length);
	region->start = NULL;
}

/**
 * Get a file descriptor for an MMIO area of an AFU.
 *
 * Once obtained, the descriptor may be used to manually MMAP a section of the MMIO area.
 *
 * @see ocxl_mmio_size() to get the size of the MMIO areas
 *
 * @pre the AFU has been opened
 *
 * @param afu the AFU to operate on
 * @param type the type of MMIO area to map
 *
 * @return the requested descriptor, or -1 if it is not available
 */
int ocxl_mmio_get_fd(ocxl_afu_h afu, ocxl_mmio_type type)
{
	switch (type) {
	case OCXL_GLOBAL_MMIO:
		return afu->global_mmio_fd;

	case OCXL_PER_PASID_MMIO:
		return afu->fd;

	default:
		errmsg(afu, OCXL_INVALID_ARGS, "Unknown MMIO type %d", type);
		return -1;
	}
}

/**
 * Get the size of an MMIO region for an AFU.
 *
 * @param afu the AFU to get the MMIO size of
 * @param type the type of the MMIO region
 *
 * @return the size of the MMIO region in bytes
 */
size_t ocxl_mmio_size(ocxl_afu_h afu, ocxl_mmio_type type)
{
	switch(type) {
	case OCXL_GLOBAL_MMIO:
		return afu->global_mmio.length;

	case OCXL_PER_PASID_MMIO:
		return afu->per_pasid_mmio.length;

	default:
		errmsg(afu, OCXL_INVALID_ARGS, "Invalid MMIO area requested '%d'", type);
		return 0;
	}
}


/**
 * Get the address & size of a mapped MMIO region.
 *
 * @pre the AFU has been opened, and the MMIO area mapped
 *
 * @param region the MMIO region to get the details for
 * @param address [out] The address of the MMIO region
 * @param size [out] the size of the MMIO region in bytes
 *
 * @retval OCXL_OK if the details were retrieved
 * @retval OCXL_INVALID_ARGS if the region is invalid
 */
ocxl_err ocxl_mmio_get_info(ocxl_mmio_h region, void **address, size_t *size)
{
	if (!region->start) {
		ocxl_err rc = OCXL_INVALID_ARGS;
		errmsg(region->afu, rc, "MMIO region has already been unmapped");
		return rc;
	}

	*address = region->start;
	*size = region->length;

	return OCXL_OK;
}


/**
 * Validate an MMIO operation.
 *
 * @param region the MMIO region
 * @param offset the offset within the MMIO area
 * @param size the size of the operation
 *
 * @retval OCXL_OK if the operation can proceed
 * @retval OCXL_INVALID_ARGS if the MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
inline static ocxl_err mmio_check(ocxl_mmio_h region, off_t offset, size_t size)
{
	if (!region) {
		ocxl_err rc = OCXL_INVALID_ARGS;
		errmsg(NULL, rc, "MMIO region is invalid");
		return rc;
	}

	if (!region->start) {
		ocxl_err rc = OCXL_INVALID_ARGS;
		errmsg(region->afu, rc, "MMIO region has already been unmapped");
		return rc;
	}

	if (offset >= (off_t)(region->length - (size - 1))) {
		ocxl_err rc = OCXL_OUT_OF_BOUNDS;
		errmsg(region->afu, rc, "%s MMIO access of 0x%016lx exceeds limit of 0x%016lx",
		       region->type == OCXL_GLOBAL_MMIO ? "Global" : "Per-PASID",
		       offset, region->length);
		return rc;
	}

	return OCXL_OK;
}

/**
 * Read a 32-bit value from an AFU's MMIO region.
 *
 * Read the 32-bit value at offset from the address of the mapped MMIO space,
 * no endianness conversion will be performed.
 * Memory barriers are inserted before and after the MMIO operation.
 *
 * @pre the AFU has been opened, and the per-PASID MMIO area mapped
 *
 * @param region the MMIO area to operate on
 * @param offset A byte address that is aligned on a word (4 byte) boundary. It
 * must be lower than the MMIO size (-4 bytes) reported by ocxl_mmio_size()
 * @param[out] out the value that was read
 *
 * @retval OCXL_OK if the value was read
 * @retval OCXL_NO_CONTEXT if the MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
inline static ocxl_err mmio_read32_native(ocxl_mmio_h region, off_t offset, uint32_t *out)
{
	ocxl_err ret = mmio_check(region, offset, 4);
	if (ret != OCXL_OK) {
		return ret;
	}

	__sync_synchronize();
	*out = *(volatile uint32_t *)(region->start + offset);
	__sync_synchronize();

	TRACE(region->afu, "%s MMIO Read32@0x%04lx=0x%08x",
	      region->type == OCXL_GLOBAL_MMIO ? "Global" : "Per-PASID",
	      offset, *out);

	return OCXL_OK;
}

/**
 * Read a 64-bit value from an AFU's MMIO region.
 *
 * Read the 64-bit value at offset from the address of the mapped MMIO space,
 * no endianness conversion will be performed.
 * Memory barriers are inserted before and after the MMIO operation.
 *
 * @pre the AFU has been opened, and the per-PASID MMIO area mapped
 *
 * @param region the MMIO area to operate on
 * @param offset A byte address that is aligned on an 8 byte boundary. It
 * must be lower than the MMIO size (-8 bytes) reported by ocxl_afu_get_mmio_size()
 * @param[out] out the value that was read
 *
 * @retval OCXL_OK if the value was read
 * @retval OCXL_NO_CONTEXT if the MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
inline static ocxl_err mmio_read64_native(ocxl_mmio_h region, off_t offset, uint64_t *out)
{
	ocxl_err ret = mmio_check(region, offset, 8);
	if (ret != OCXL_OK) {
		return ret;
	}

	__sync_synchronize();
	*out = *(volatile uint64_t *)(region->start + offset);
	__sync_synchronize();

	TRACE(region->afu, "%s MMIO Read64@0x%04lx=0x%016lx",
	      region->type == OCXL_GLOBAL_MMIO ? "Global" : "Per-PASID",
	      offset, *out);

	return OCXL_OK;
}

/**
 * Write a 32-bit value to an AFU's MMIO region.
 *
 * Write the 32-bit word at offset from the address of the mapped MMIO space,
 * no endianness conversion will be performed.
 *
 * Memory barriers are inserted before and after the MMIO operation.
 *
 * @pre the AFU has been opened, and the MMIO area mapped
 *
 * @param afu the AFU to operate on
 * @param region the MMIO area to operate on
 * @param offset A byte address that is aligned on a 4 byte boundary. It
 * must be lower than the MMIO size (-4 bytes) reported by ocxl_mmio_size()
 * @param value the value to write
 *
 * @retval OCXL_OK if the value was written
 * @retval OCXL_NO_CONTEXT if the MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
inline static ocxl_err mmio_write32_native(ocxl_mmio_h region, off_t offset, uint32_t value)
{
	ocxl_err ret = mmio_check(region, offset, 4);
	if (ret != OCXL_OK) {
		return ret;
	}

	TRACE(region->afu, "%s MMIO Write32@0x%04lx=0x%08x",
	      region->type == OCXL_GLOBAL_MMIO ? "Global" : "Per-PASID",
	      offset, value);

	volatile uint32_t *addr = (uint32_t *)(region->start + offset);

	__sync_synchronize();
	*addr = value;
	__sync_synchronize();

	return OCXL_OK;
}

/**
 * Write a 64-bit value to the mapped AFU per-PASID MMIO space.
 *
 * Write the 64-bit value at offset from the address of the mapped MMIO space,
 * no endianness conversion will be performed.
 * Memory barriers are inserted before and after the MMIO operation.
 *
 * @pre the AFU has been opened, and the MMIO area mapped
 *
 * @param region the MMIO area to operate on
 * @param offset A byte address that is aligned on an 8 byte boundary. It
 * must be lower than the MMIO size (-8 bytes) reported by ocxl_afu_get_mmio_size()
 * @param value the value to write
 *
 * @retval OCXL_OK if the value was written
 * @retval OCXL_NO_CONTEXT if the MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
inline static ocxl_err mmio_write64_native(ocxl_mmio_h region, off_t offset, uint64_t value)
{
	ocxl_err ret = mmio_check(region, offset, 8);
	if (ret != OCXL_OK) {
		return ret;
	}

	TRACE(region->afu, "%s MMIO Write64@0x%04lx=0x%016lx",
	      region->type == OCXL_GLOBAL_MMIO ? "Global" : "Per-PASID",
	      offset, value);

	volatile uint64_t *addr = (uint64_t *)(region->start + offset);

	__sync_synchronize();
	*addr = value;
	__sync_synchronize();

	return OCXL_OK;
}

/**
 * Read a 32-bit value from an AFU's MMIO region & convert endianness.
 *
 * Read the 32-bit value at offset from the address of the mapped MMIO space,
 * and convert endianness as specified by the endian parameter.
 * Memory barriers are inserted before and after the MMIO operation.
 *
 * @pre the AFU has been opened, and the MMIO area mapped
 * @param mmio the MMIO area to operate on
 * @param offset A byte address that is aligned on a word (4 byte) boundary. It
 * must be lower than the MMIO size (-4 bytes) reported by ocxl_mmio_size()
 * @param endian the endianness of the stored data (will be converted to native)
 * @param[out] out the value that was read
 *
 * @retval OCXL_OK if the value was read
 * @retval OCXL_NO_CONTEXT if the MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
ocxl_err ocxl_mmio_read32(ocxl_mmio_h mmio, off_t offset, ocxl_endian endian, uint32_t *out)
{
	uint32_t val;
	ocxl_err ret = mmio_read32_native(mmio, offset, &val);

	if (UNLIKELY(ret != OCXL_OK)) {
		return ret;
	}

	switch (endian) {
	case OCXL_MMIO_BIG_ENDIAN:
		*out = be32toh(val);
		break;

	case OCXL_MMIO_LITTLE_ENDIAN:
		*out = le32toh(val);
		break;
	default:
		*out = val;
		break;
	}

	return OCXL_OK;
}

/**
 * Read a 64-bit value from an AFU's MMIO region & convert endianness.
 *
 * Read the 64-bit value at offset from the address of the mapped MMIO space,
 * and convert endianness as specified by the endian parameter.
 * Memory barriers are inserted before and after the MMIO operation.
 *
 * @pre the AFU has been opened, and the MMIO area mapped
 *
 * @param mmio the MMIO area to operate on
 * @param offset A byte address that is aligned on an 8 byte boundary. It
 * must be lower than the MMIO size (-8 bytes) reported by ocxl_afu_get_mmio_size()
 * @param endian the endianness of the stored data (will be converted to native)
 * @param[out] out the value that was read
 *
 * @retval OCXL_OK if the value was read
 * @retval OCXL_NO_CONTEXT if the MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
ocxl_err ocxl_mmio_read64(ocxl_mmio_h mmio, off_t offset, ocxl_endian endian, uint64_t *out)
{
	uint64_t val;
	ocxl_err ret = mmio_read64_native(mmio, offset, &val);

	if (UNLIKELY(ret != OCXL_OK)) {
		return ret;
	}

	switch (endian) {
	case OCXL_MMIO_BIG_ENDIAN:
		*out = be64toh(val);
		break;

	case OCXL_MMIO_LITTLE_ENDIAN:
		*out = le64toh(val);
		break;
	default:
		*out = val;
		break;
	}

	return OCXL_OK;
}

/**
 * Convert endianness and write a 32-bit value to an AFU's MMIO region.
 *
 * Convert endianness and write the 32-bit word at offset from the address of the mapped MMIO space.
 * Memory barriers are inserted before and after the MMIO operation.
 *
 * @pre the AFU has been opened, and the MMIO area mapped
 *
 * @param mmio the MMIO area to operate on
 * @param offset A byte address that is aligned on a 4 byte boundary. It
 * must be lower than the MMIO size (-4 bytes) reported by ocxl_mmio_size()
 *
 * @param endian the endianness of the stored data (value will be converted to this before storing it)
 * @param value the value to write
 *
 * @retval OCXL_OK if the value was written
 * @retval OCXL_NO_CONTEXT if the MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
ocxl_err ocxl_mmio_write32(ocxl_mmio_h mmio, off_t offset, ocxl_endian endian, uint32_t value)
{
	switch (endian) {
	case OCXL_MMIO_BIG_ENDIAN:
		value = htobe32(value);
		break;

	case OCXL_MMIO_LITTLE_ENDIAN:
		value = htole32(value);
		break;
	default:
		break;
	}

	return mmio_write32_native(mmio, offset, value);
}

/**
 * Convert endianness and write a 64-bit value to an AFU's MMIO region.
 *
 * Convert endianness and write the 32-bit word at offset from the address of the mapped MMIO space.
 * Memory barriers are inserted before and after the MMIO operation.
 *
 * @pre the AFU has been opened, and the MMIO area mapped
 *
 * @param mmio the MMIO area to operate on
 * @param offset A byte address that is aligned on an 8 byte boundary. It
 * must be lower than the MMIO size (-8 bytes) reported by ocxl_mmio_size()
 * @param endian the endianness of the stored data (value will be converted to this before storing it)
 * @param value the value to write
 *
 * @retval OCXL_OK if the value was written
 * @retval OCXL_NO_CONTEXT if the MMIO area is not mapped
 * @retval OCXL_OUT_OF_BOUNDS if the offset exceeds the available area
 */
ocxl_err ocxl_mmio_write64(ocxl_mmio_h mmio, off_t offset, ocxl_endian endian, uint64_t value)
{
	switch (endian) {
	case OCXL_MMIO_BIG_ENDIAN:
		value = htobe64(value);
		break;

	case OCXL_MMIO_LITTLE_ENDIAN:
		value = htole64(value);
		break;
	default:
		break;
	}

	return mmio_write64_native(mmio, offset, value);
}


/**
 * @}
 */
