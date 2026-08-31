/* SPDX-License-Identifier: MIT */
#include "gx_hw.h"
#include "ff.h"
#include "diskio.h"
#include "usb/usb_msc.h"
#include "boot.h"
#include "print.h"

extern int g_verbose;

#define ELF_MAX (8u * 1024u * 1024u)

static FATFS fs;
static FIL fil;

static int read_file_to(const char *path, u8 *dst, u32 max, u32 *out_len)
{
	FRESULT fr;
	UINT br;
	u32 total = 0;

	fr = f_open(&fil, path, FA_READ);
	if (fr != FR_OK)
		return -1;
	while (total < max) {
		UINT chunk = (max - total > 4096u) ? 4096u : (UINT)(max - total);

		fr = f_read(&fil, dst + total, chunk, &br);
		if (fr != FR_OK) {
			f_close(&fil);
			return -1;
		}
		total += br;
		if (br < chunk)
			break;
	}
	f_close(&fil);
	*out_len = total;
	return 0;
}

static int parse_start_file(char *out, u32 out_sz)
{
	char line[128];
	UINT br, i, n;
	FRESULT fr;
	int in_gx = 0;

	fr = f_open(&fil, "config.txt", FA_READ);
	if (fr != FR_OK)
		return -1;

	n = 0;
	for (;;) {
		char c;
		fr = f_read(&fil, &c, 1, &br);
		if (fr != FR_OK || br == 0)
			break;
		if (c == '\r')
			continue;
		if (c == '\n' || n + 1 >= sizeof(line)) {
			line[n] = 0;
			if (line[0] == '[') {
				in_gx = 0;
				if (line[1] == 'g' && line[2] == 'x')
					in_gx = 1;
			} else if (in_gx) {
				const char *key = "start_file=";
				u32 k;
				int match = 1;

				for (k = 0; key[k]; k++) {
					if (line[k] != key[k]) {
						match = 0;
						break;
					}
				}
				if (match) {
					for (i = 0; line[k] && i + 1 < out_sz; i++, k++)
						out[i] = line[k];
					out[i] = 0;
					f_close(&fil);
					return 0;
				}
			}
			n = 0;
			continue;
		}
		line[n++] = c;
	}
	f_close(&fil);
	return -1;
}

int bc_usb_boot(void)
{
	char start_file[64];
	u8 *dst = (u8 *)STAGE_BUF;
	u32 len = 0;
	u32 entry = 0;
	FRESULT fr;

	bc_vputs("Attempting to boot from USB...\r\n");

	if (usb_msc_init()) {
		bc_vputs("No USB device detected, trying SPI...\r\n");
		return -1;
	}

	fr = f_mount(&fs, "", 1);
	if (fr != FR_OK) {
		bc_vputs("No USB device detected, trying SPI...\r\n");
		return -1;
	}

	if (parse_start_file(start_file, sizeof(start_file))) {
		start_file[0] = 's';
		start_file[1] = 't';
		start_file[2] = 'a';
		start_file[3] = 'r';
		start_file[4] = 't';
		start_file[5] = '6';
		start_file[6] = '7';
		start_file[7] = '0';
		start_file[8] =
#if defined(SOC_GX6706)
			'6';
#else
			'2';
#endif
		start_file[9] = '.';
		start_file[10] = 'e';
		start_file[11] = 'l';
		start_file[12] = 'f';
		start_file[13] = 0;
	}

	{
		u32 cfg_sz = 0;
		u8 tmp[1];
		UINT br;

		if (f_open(&fil, "config.txt", FA_READ) == FR_OK) {
			for (;;) {
				if (f_read(&fil, tmp, 1, &br) != FR_OK || br == 0)
					break;
				cfg_sz++;
			}
			f_close(&fil);
		}
		if (g_verbose) {
			bc_puts("Reading: config.txt, ");
			bc_put_dec(cfg_sz);
			bc_puts("\r\n");
		}
	}

	if (read_file_to(start_file, dst, ELF_MAX, &len)) {
		bc_vputs("No USB device detected, trying SPI...\r\n");
		f_mount(0, "", 0);
		return -1;
	}
	if (g_verbose) {
		bc_puts("Reading: ");
		bc_puts(start_file);
		bc_puts(", ");
		bc_put_dec(len);
		bc_puts(" (bytes)\r\n");
	}

	if (bc_elf_load(dst, len, &entry)) {
		f_mount(0, "", 0);
		return -1;
	}
	f_mount(0, "", 0);
	bc_jump(entry);
	return 0;
}
