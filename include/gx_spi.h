/* SPDX-License-Identifier: MIT */
#ifndef GX_SPI_H
#define GX_SPI_H

#include "gx_types.h"

void gx_spi_init(void);
int gx_spi_read(u32 flash_off, void *dst, u32 len);

#endif
