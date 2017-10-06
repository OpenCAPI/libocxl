/*
 * Copyright 2017 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#ifndef _UAPI_MISC_OCXL_H
#define _UAPI_MISC_OCXL_H

#include <linux/types.h>
#include <linux/ioctl.h>

enum ocxl_event_type {
	OCXL_AFU_EVENT_XSL_FAULT_ERROR = 0,
};

#define OCXL_KERNEL_EVENT_FLAG_LAST 0x0001  /* This is the last event pending */

struct ocxl_kernel_event_header {
	__u16 type;
	__u16 flags;
	__u32 reserved2;
};

struct ocxl_kernel_event_xsl_fault_error {
	__u64 addr;
	__u64 dsisr;
	__u64 count;
};

struct ocxl_ioctl_attach {
	__u64 amr;
	__u64 reserved1;
	__u64 reserved2;
	__u64 reserved3;
};

/* ioctl numbers */
#define OCXL_MAGIC 0xCA
/* AFU devices */
#define OCXL_IOCTL_ATTACH	_IOW(OCXL_MAGIC, 0x10, struct ocxl_ioctl_attach)

#endif /* _UAPI_MISC_OCXL_H */
