/* SPDX-License-Identifier: MIT */
/* Tiny libc stubs for FatFs / freestanding bootcode. */

#include "gx_types.h"

void *memcpy(void *dst, const void *src, ulong n)
{
	u8 *d = dst;
	const u8 *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

void *memset(void *dst, int c, ulong n)
{
	u8 *d = dst;

	while (n--)
		*d++ = (u8)c;
	return dst;
}

int memcmp(const void *a, const void *b, ulong n)
{
	const u8 *x = a, *y = b;

	while (n--) {
		if (*x != *y)
			return (int)*x - (int)*y;
		x++;
		y++;
	}
	return 0;
}

char *strchr(const char *s, int c)
{
	while (*s) {
		if (*s == (char)c)
			return (char *)s;
		s++;
	}
	return (c == 0) ? (char *)s : 0;
}

ulong strlen(const char *s)
{
	ulong n = 0;

	while (s[n])
		n++;
	return n;
}
