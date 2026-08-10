/* SPDX-License-Identifier: MIT */
#ifndef USB_MSC_H
#define USB_MSC_H

#include "gx_types.h"

int usb_msc_init(void);
int usb_msc_read_sector(u32 lba, u8 *buf);
int usb_msc_present(void);

#endif
