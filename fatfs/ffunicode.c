/* SPDX-License-Identifier: MIT */

/* stubs for FatFs to skip including entire Unicode bundle */
#include "ff.h"

WCHAR ff_oem2uni(WCHAR oem, WORD cp)
{
	(void)cp;
	return oem < 0x80u ? oem : 0;
}

WCHAR ff_uni2oem(DWORD uni, WORD cp)
{
	(void)cp;
	return uni < 0x80u ? (WCHAR)uni : '?';
}

DWORD ff_wtoupper(DWORD uni)
{
	if (uni >= 'a' && uni <= 'z')
		return uni - ('a' - 'A');
	return uni;
}
