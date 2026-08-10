/* SPDX-License-Identifier: MIT */
#ifndef BC_BOOT_H
#define BC_BOOT_H

#include "gx_types.h"

void bc_jump(u32 entry);
int bc_spi_load_uboot(void);
int bc_uart_load_uboot(void);
void bc_halt(void);
int bc_usb_boot(void);
int bc_elf_load(const u8 *elf, u32 size, u32 *entry);

#endif
