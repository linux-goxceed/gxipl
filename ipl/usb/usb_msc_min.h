/* SPDX-License-Identifier: MIT */
#ifndef IPL_USB_MSC_MIN_H
#define IPL_USB_MSC_MIN_H

#include "gx_types.h"

/*
 * Minimal USB MSC block reader for the stage-1 IPL.  Read-only: it can
 * enumerate a high-speed bulk-only mass-storage device and issue READ(10).
 *
 * It deliberately does NOT parse configuration descriptors.  The standard
 * bulk-only topology is assumed (address 1, config 1, interface 0, EP 1 IN,
 * EP 2 OUT, 512-byte MPS), which is what every USB flash stick presents.
 * A device that differs, or one that enumerates at full/low speed, will not
 * work -- that trade buys the descriptor parser and its 512-byte buffer out
 * of an 8 KiB budget.
 *
 * Both entry points return 0 on success and -1 on failure; the IPL treats USB
 * as best-effort and falls through to the next boot source.
 */

/* Up to this many sectors per READ(10) (see MSC_MAX_XFER_SECTORS). */
#define USB_MSC_MIN_MAX_SECTORS	4

int usb_msc_min_init(void);

/* Stop the controller and clear the port before handing off to stage 2.
 * No-op unless stage 1 reached us over USB.  See the implementation. */
void usb_msc_min_quiesce(void);

/*
 * Note the two-argument form.  The bootcode core this was ported from takes
 * a sector `count`, but the IPL only ever issues the fixed-size window read
 * in disk_readp(), so the parameter was dropped.  Keep this header in sync
 * with the definition in usb_msc_min.c: a stale prototype here is only a
 * warning at LTO time, and under USB_DEBUG=1 it becomes a hard link error
 * once the 8 KiB window overflows and the strict check re-runs.
 */
int usb_msc_min_read_sectors(u32 lba, u8 *buf);

#endif
