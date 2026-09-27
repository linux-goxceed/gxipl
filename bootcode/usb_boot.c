/* SPDX-License-Identifier: MIT */
#include "gx_hw.h"
#include "ff.h"
#include "diskio.h"
#include "usb/usb_msc.h"
#include "boot.h"
#include "print.h"
#include "usb_debug.h"

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
	if (fr != FR_OK) {
#if USB_DEBUG
		if (g_verbose) {
			bc_puts("USBERR OPEN ");
			bc_puts(path);
			bc_puts("=");
			bc_put_dec((u32)fr);
			bc_puts("\r\n");
		}
#endif
		return -1;
	}
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

/* Filter start_file by specificity; equal-specificity entries use file order. */
static int cfg_space(char c)
{
	return c == ' ' || c == '\t';
}

static int cfg_equal_ci(const char *a, const char *b)
{
	while (*a && *b) {
		char ca = *a++;
		char cb = *b++;

		if (ca >= 'A' && ca <= 'Z')
			ca += 'a' - 'A';
		if (cb >= 'A' && cb <= 'Z')
			cb += 'a' - 'A';
		if (ca != cb)
			return 0;
	}
	return *a == *b;
}

static int cfg_hex(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int cfg_serial_matches(const char *s)
{
	u32 high = 0, low = 0;
	u32 id_low = readl(GX_PUBLIC_ID_VIRT);
	u32 id_high = readl(GX_PUBLIC_ID_VIRT + 4u);
	u32 i;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	/* Match the full eight-byte ID printed by the bootcode banner. */
	for (i = 0; i < 16; i++) {
		int nibble = cfg_hex(s[i]);

		if (nibble < 0)
			return 0;
		if (i < 8)
			high = (high << 4) | (u32)nibble;
		else
			low = (low << 4) | (u32)nibble;
	}
	if (s[16] || (!id_low && !id_high) ||
	    (id_low == 0xffffffffu && id_high == 0xffffffffu))
		return 0;
	return high == id_high && low == id_low;
}

static int cfg_section_priority(const char *section)
{
	if (cfg_equal_ci(section, "all") || cfg_equal_ci(section, "gx"))
		return 0;

#if defined(SOC_GX6706)
	if (cfg_equal_ci(section, "gx6706"))
		return 1;
#else
	if (cfg_equal_ci(section, "gx6702"))
		return 1;
#endif
	return cfg_serial_matches(section) ? 2 : -1;
}

static void cfg_parse_line(char *line, int *active_priority, char *out,
			   u32 out_sz, int *best_priority)
{
	char *p = line, *end, *eq, *value;
	u32 n, i;

	/* Strip comments, then trim surrounding spaces and tabs. */
	for (p = line; *p; p++) {
		if (*p == '#' || *p == ';') {
			*p = 0;
			break;
		}
	}
	p = line;
	while (cfg_space(*p))
		p++;
	end = p;
	while (*end)
		end++;
	while (end > p && cfg_space(end[-1]))
		end--;
	*end = 0;
	if (!*p)
		return;

	if (*p == '[') {
		if (end[-1] != ']') {
			*active_priority = -1;
			return;
		}
		end[-1] = 0;
		p++;
		while (cfg_space(*p))
			p++;
		end = p;
		while (*end)
			end++;
		while (end > p && cfg_space(end[-1]))
			end--;
		*end = 0;
		*active_priority = cfg_section_priority(p);
		return;
	}

	if (*active_priority < 0)
		return;
	eq = p;
	while (*eq && *eq != '=')
		eq++;
	if (!*eq)
		return;
	*eq = 0;
	end = p;
	while (*end)
		end++;
	while (end > p && cfg_space(end[-1]))
		end--;
	*end = 0;
	if (!cfg_equal_ci(p, "start_file"))
		return;

	value = eq + 1;
	while (cfg_space(*value))
		value++;
	end = value;
	while (*end)
		end++;
	while (end > value && cfg_space(end[-1]))
		end--;
	*end = 0;
	n = (u32)(end - value);
	if (!n || n >= out_sz || *active_priority < *best_priority)
		return;
	for (i = 0; i < n; i++)
		out[i] = value[i];
	out[n] = 0;
	*best_priority = *active_priority;
}

static int parse_start_file(char *out, u32 out_sz)
{
	char line[128];
	UINT br, n = 0;
	FRESULT fr;
	int active_priority = 0, best_priority = -1, overflow = 0;

	fr = f_open(&fil, "config.txt", FA_READ);
	if (fr != FR_OK)
		return -1;

	for (;;) {
		char c;

		fr = f_read(&fil, &c, 1, &br);
		if (fr != FR_OK)
			break;
		if (!br) {
			if (n && !overflow) {
				line[n] = 0;
				cfg_parse_line(line, &active_priority, out, out_sz,
					       &best_priority);
			}
			break;
		}
		if (c == '\r')
			continue;
		if (c == '\n') {
			if (!overflow) {
				line[n] = 0;
				cfg_parse_line(line, &active_priority, out, out_sz,
					       &best_priority);
			} else {
				char *p = line;

				while (cfg_space(*p))
					p++;
				if (*p == '[')
					active_priority = -1;
			}
			n = 0;
			overflow = 0;
			continue;
		}
		if (overflow)
			continue;
		if (n + 1 < sizeof(line))
			line[n++] = c;
		else {
			overflow = 1;
			line[n] = 0;
		}
	}
	f_close(&fil);
	return best_priority >= 0 ? 0 : -1;
}

int bc_usb_boot(void)
{
	char start_file[64];
	u8 *dst = (u8 *)STAGE_BUF;
	u32 len = 0;
	u32 entry = 0;
	FRESULT fr;
	int default_file = 0;
        #ifdef VERBOSE_MINIFY
	bc_vputs("USBBOOT\r\n");
        #else
	bc_vputs("Attempting to boot from USB...\r\n");
        #endif

	if (usb_msc_init()) {
                #ifdef VERBOSE_MINIFY
		bc_vputs("ENOUSB\r\nSPIBOOT\r\n");
                #else
		bc_vputs("No USB device detected, trying SPI...\r\n");
                #endif
		return -1;
	}

	fr = f_mount(&fs, "", 1);
	if (fr != FR_OK) {
#if USB_DEBUG
		if (g_verbose) {
			bc_puts("USBERR FAT mount=");
			bc_put_dec((u32)fr);
			bc_puts("\r\n");
		}
#endif
                #ifdef VERBOSE_MINIFY
		bc_vputs("EUSBMNT\r\nSPIBOOT\r\n");
                #else
		bc_vputs("Could not mount USB device, trying SPI...\r\n");
                #endif
		return -1;
	}

	if (parse_start_file(start_file, sizeof(start_file))) {
		default_file = 1;
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
	if (g_verbose) {
		bc_puts("USBFILE ");
		bc_puts(start_file);
		bc_puts("\r\n");
	}

	{
	        #ifndef VERBOSE_MINIFY
		u32 cfg_sz = 0;
		#endif
		u8 tmp[1];
		UINT br;

		if (f_open(&fil, "config.txt", FA_READ) == FR_OK) {
			for (;;) {
				if (f_read(&fil, tmp, 1, &br) != FR_OK || br == 0)
					break;
				#ifndef VERBOSE_MINIFY
				cfg_sz++;
				#endif
			}
			f_close(&fil);
		}
		if (g_verbose) {
                        #ifdef VERBOSE_MINIFY
			bc_puts("READ config.txt");
                        #else
			bc_puts("Reading: config.txt, ");
			bc_put_dec(cfg_sz);
			bc_puts("\r\n");
                        #endif
		}
	}

	if (read_file_to(start_file, dst, ELF_MAX, &len)) {
		/* Keep a short-name fallback for FAT volumes without usable LFN data. */
		if (default_file && start_file[0] == 's' && start_file[1] == 't' &&
		    start_file[5] == '6' && start_file[9] == '.') {
			start_file[5] = '.';
			start_file[6] = 'e';
			start_file[7] = 'l';
			start_file[8] = 'f';
			start_file[9] = 0;
			if (g_verbose) {
				bc_puts("USBFILE fallback ");
				bc_puts(start_file);
				bc_puts("\r\n");
			}
			if (!read_file_to(start_file, dst, ELF_MAX, &len))
				goto elf_loaded;
		}
                #ifdef VERBOSE_MINIFY
		bc_vputs("EELFREAD\r\nSPIBOOT\r\n");
                #else
		bc_vputs("Could not read ELF file from USB, trying SPI...\r\n");
                #endif
		f_mount(0, "", 0);
		return -1;
	}

elf_loaded:
	if (g_verbose) {
                #ifdef VERBOSE_MINIFY
		bc_puts("READ ");
		bc_puts(start_file);
                #else
		bc_puts("Reading: ");
		bc_puts(start_file);
		bc_puts(", ");
		bc_put_dec(len);
		bc_puts(" (bytes)\r\n");
                #endif
	}

	if (bc_elf_load(dst, len, &entry)) {
		f_mount(0, "", 0);
		return -1;
	}
	f_mount(0, "", 0);
	bc_jump(entry);
	return 0;
}
