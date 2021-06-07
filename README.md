# Introduction
LibOCXL provides an access library which allows the user to implement a userspace
driver for an [OpenCAPI](http://opencapi.org/about/) accelerator.

# Features
## Initialization
AFUs may be opened using the device path or AFU name. If an multiple AFU instances
with the same name are available, the first AFU found with available contexts will
be used.

## Operation
LibOCXL provides functions to attach to an AFU and transfer data to & from the MMIO areas
on the AFU. AFU IRQs can be queried either by IRQ handles, or by associating a callback
to the IRQ.

## MMIO
Functions are provide to allow 32 & 64 bit access to the global and per-PASID MMIO
areas on the the AFU. Endian conversion is handled automatically.

## Installation
LibOCXL is available in popular Linux distributions for PPC64le. To install:
### Redhat
`yum install libocxl-devel`

### Fedora
`dnf install libocxl-devel`

### Ubuntu/Debian
`apt install libocxl-dev`

# Building
## Prerequisites
1. A GCC toolchain with libc (if cross compiling), crosstool-ng can build a suitable toolchain
   if your cross compiler does not include libc.
2. [Doxygen](http://www.stack.nl/~dimitri/doxygen/) 1.8.14 or later. Earlier versions will work, but don't generate enums in the man
   pages correctly.
3. [Astyle](http://astyle.sourceforge.net/), if you plan on submitting patches.
4. [CPPCheck](http://cppcheck.sourceforge.net/), if you plan on submitting patches.
5. [libFUSE](https://github.com/libfuse/libfuse), to run tests

## Included Dependencies
- OCXL headers from the [Linux kernel](https://www.kernel.org/)

## Build Instructions (Local build)
- `make`
- `PREFIX=/usr/local make install`

## Build Instructions (Cross compilation)
- `export CROSS_COMPILE=/path/to/compiler/bin/powerpc64le-unknown-linux-gnu-`
- `make`


# Usage
A typical use of libocxl will follow this pattern:

1. **Setup:** optionally turn on error reporting for the open calls: ocxl\_enable\_messages().
2. **Open the device:** ocxl\_afu\_open() if an AFU name is used, or ocxl\_afu\_open\_from\_dev() if
   a device path is used. Optionally turn on error reporting for the AFU: ocxl\_afu\_enable\_messages().
3. **Allocate IRQs:** ocxl\_irq\_alloc(). This returns a sequential per-AFU IRQ number.
   An opaque pointer is associated with the
   handle in which the caller can store additional information. This is not used by OpenCAPI,
   but is passed as part of the event information to provide additional context to the IRQ handler.
4. **Configure global MMIO:** Some AFUs may have a global MMIO area, which will contain configuration
   information that will affect all PASIDs on the AFU. Use ocxl\_mmio\_map() to make the area available,
   then use ocxl\_mmio\_write32() and ocxl\_mmio\_write64() to write the information.
5. **Configure the per-PASID MMIO:** Some AFUs support multiple contexts, and each context will
   get it's own MMIO area for configuration and communication. Typical information that may
   be communicated across the MMIO interface include IRQ handles (obtained with
   ocxl\_irq\_get\_handle()), and pointers to AFU-specific
   data structures. Use ocxl\_mmio\_map to make the area available, then use
   ocxl\_mmio\_write32() and ocxl\_mmio\_write64() to write the information.
6. **Attach the AFU context to the process:** Use ocxl\_afu\_attach() to make the process's address space available
   to the AFU context, allowing it to read & write to the process's memory.
7. **Signal the AFU to do some work:** This is typically done via a write into the per-PASID MMIO area.
8. **Handle AFU IRQs:** Pending IRQs can be queried using ocxl\_afu\_event\_check(). An IRQ event
   contains the IRQ number, the info pointer assigned when activated, the 64 bit IRQ handle, and
   the number of times the IRQ has been triggered since last checked.
9. **Read results:** Work completion may be signalled by the AFU via an IRQ, or by writing to
   the MMIO area. Typically, bulk data should be written to a pointer passed to the AFU, however,
   small quantities of data may be read from an MMIO area using ocxl\_mmio\_read32() and
   ocxl\_mmio\_read64().
10. **Termination:** ocxl\_afu\_close() will free all resources associated with an AFU handle.

# Documentation

API documentation is generated using [Doxygen](http://www.stack.nl/~dimitri/doxygen/) in both HTML and man page format.

To build the documentation, run `make docs`.

An online version of the libocxl HTML documentation can be found at [opencapi.github.io](https://opencapi.github.io).

# Development
The following environment variables may be set (to 1 or "YES") to assist with development:

**LIBOCXL_INFO** Print information about the LibOCXL build to stderr. This should be included in any bug reports.

**LIBOCXL_TRACE_ALL** Force AFU interaction trace messages to be emitted for all AFUs unless explicitly disabled.

**LIBOCXL_VERBOSE_ERRORS_ALL** Force verbose errors to be emitted for any failed LibOCXL calls, unless explicitly disabled.


For testing and/or assist in various environments, the following environment variable can also be set:

**LIBOCXL_SYSPATH** Override the default path (/sys/class/ocxl) used by the library to read driver information.

Patches may be submitted via Github pull requests. Please prepare your patches
by running `make precommit` before committing your work, and addressing any warnings & errors reported.
Patches must compile cleanly with the latest stable version of GCC to be accepted.
