/* SPDX-License-Identifier: MIT */
#ifndef GX_TYPES_H
#define GX_TYPES_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long ulong;

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BIT(n) (1u << (n))
#define NULL ((void *)0)

#endif
