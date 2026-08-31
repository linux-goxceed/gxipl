/* SPDX-License-Identifier: MIT */
#ifndef GX_SPI_H
#define GX_SPI_H

#include "gx_types.h"

void gx_spi_init(void);
int gx_spi_read(u32 flash_off, void *dst, u32 len);
int gx_spi_read_id(u8 id[3]);
#if defined(SOC_GX6706)
u32 gx_spi_status(void);
u32 gx_spi_wrapper_status(void);
u32 gx_spi_pad_status(void);
u32 gx_spi_debug_reg(u32 offset);
#endif

#endif
