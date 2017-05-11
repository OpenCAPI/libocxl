/*
 * Copyright 2017 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#ifndef _UAPI_LINUX_USRIRQ_H
#define _UAPI_LINUX_USRIRQ_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* ioctl numbers */
#define USRIRQ_MAGIC 0xCA

#define USRIRQ_ALLOC		_IOR(USRIRQ_MAGIC, 0x40, __u64)
#define USRIRQ_FREE		_IOW(USRIRQ_MAGIC, 0x41, __u64)
#define USRIRQ_SET_EVENTFD	_IOW(USRIRQ_MAGIC, 0x42, __u64)

struct usrirq_event {
	__u64	irq_offset;
	int	eventfd;
};

#endif /* _UAPI_LINUX_USRIRQ_H */
