/* SPDX-License-Identifier: MIT */
#ifndef BC_PRINT_H
#define BC_PRINT_H

#include "gx_types.h"

void bc_putc(char c);
void bc_puts(const char *s);
void bc_put_hex(u32 v);
void bc_put_dec(u32 v);
void bc_vputs(const char *s);
void bc_banner(void);

#endif
