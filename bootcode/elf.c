/* SPDX-License-Identifier: MIT */
/*
 * Minimal little-endian ELF32 loader for static C-SKY CK610 (ABIv1) images.
 *
 * Accepts only ET_EXEC binaries that need no dynamic linker / relocator, with
 * PT_LOAD segments and e_entry inside the post-MMU DDR window (0x90000000,
 * 64 MiB — same map as U-Boot CFG_SYS_SDRAM_SIZE).
 */

#include "gx_types.h"
#include "gx_hw.h"
#include "boot.h"
#include "print.h"
#include "elf.h"

extern int g_verbose;

static void mem_set(u8 *d, u8 v, u32 n)
{
	while (n--)
		*d++ = v;
}

static void mem_cpy(u8 *d, const u8 *s, u32 n)
{
	while (n--)
		*d++ = *s++;
}

static int u32_add_ok(u32 a, u32 b, u32 *sum)
{
	if (a + b < a)
		return 0;
	*sum = a + b;
	return 1;
}

static int in_ddr(u32 addr, u32 len)
{
	u32 end;

	if (addr < ELF_DDR_BASE)
		return 0;
	if (!u32_add_ok(addr, len, &end))
		return 0;
	return end <= ELF_DDR_END;
}

static int elf_reject(const char *why)
{
	if (g_verbose) {
		bc_puts(BC_ERR);
		bc_puts(why);
		bc_puts("\r\n");
	}
	return -1;
}

#ifndef VERBOSE_MINIFY
static int elf_reject_hex(const char *why, u32 value)
{
	if (g_verbose) {
		bc_puts(BC_ERR);
		bc_puts(why);
		bc_puts(" (");
		bc_put_hex(value);
		bc_puts(")\r\n");
	}
	return -1;
}

static int elf_reject_dec(const char *why, u32 value)
{
	if (g_verbose) {
		bc_puts(BC_ERR);
		bc_puts(why);
		bc_puts(" (");
		bc_put_dec(value);
		bc_puts(")\r\n");
	}
	return -1;
}

#endif

static const struct elf32_phdr *phdr_at(const u8 *elf, const struct elf32_hdr *eh,
					u16 i)
{
	return (const struct elf32_phdr *)(elf + eh->e_phoff +
					   (u32)i * eh->e_phentsize);
}

int bc_elf_load(const u8 *elf, u32 size, u32 *entry)
{
	const struct elf32_hdr *eh = (const struct elf32_hdr *)elf;
	u16 i;
	int saw_load = 0;
	int entry_in_x = 0;

	if (size < sizeof(*eh))
		return elf_reject(BC_ELF_ERR_TOO_SMALL);
	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
		return elf_reject(BC_ELF_ERR_NOT_ELF);
	/* ELFCLASS32, ELFDATA2LSB, EV_CURRENT */
	if (eh->e_ident[4] != 1)
		return elf_reject(BC_ELF_ERR_NOT_ELF32);
	if (eh->e_ident[5] != 1)
		return elf_reject(BC_ELF_ERR_NOT_LE);
	if (eh->e_ident[6] != 1)
		return elf_reject(BC_ELF_ERR_IDENT);
	if (eh->e_type == ET_DYN)
		return elf_reject(BC_ELF_ERR_NOT_STATIC);
	if (eh->e_type == ET_REL)
		return elf_reject(BC_ELF_ERR_ET_REL);
	if (eh->e_type == ET_CORE)
		return elf_reject(BC_ELF_ERR_ET_CORE);
	if (eh->e_type != ET_EXEC)
                #ifdef VERBOSE_MINIFY
		return elf_reject(BC_ELF_ERR_E_TYPE_INVALID);
                #else
  		return elf_reject_dec(BC_ELF_ERR_E_TYPE_INVALID, eh->e_type);
                #endif
	if (eh->e_machine != EM_CSKY)
                #ifdef VERBOSE_MINIFY
		return elf_reject(BC_ELF_ERR_INVALID_MACHINE);
                #else
		return elf_reject_dec(BC_ELF_ERR_INVALID_MACHINE,
				      eh->e_machine);
                #endif
	if (eh->e_version != EV_CURRENT)
		return elf_reject(BC_ELF_ERR_UNSUPPORTED_ELF);
	/* CK610 open IPL / U-Boot toolchain is ABIv1 only. */
	if (eh->e_flags & EF_CSKY_ABIV2)
                #ifdef VERBOSE_MINIFY
		return elf_reject(BC_ELF_ERR_CSKY_ABIV2);
                #else
  		return elf_reject_hex(BC_ELF_ERR_CSKY_ABIV2,
				      eh->e_flags);
                #endif
	if (!(eh->e_flags & EF_CSKY_ABIV1))
                #ifdef VERBOSE_MINIFY
		return elf_reject(BC_ELF_ERR_INVALID_ARCH);
                #else
		return elf_reject_hex(BC_ELF_ERR_INVALID_ARCH,
				      eh->e_flags);
                #endif
	if (eh->e_ehsize < sizeof(*eh))
		return elf_reject(BC_ELF_ERR_TRUNCATED_ELF);
	if (!eh->e_phnum)
		return elf_reject(BC_ELF_ERR_NO_PROGRAM_HEADERS);
	if (eh->e_phentsize < sizeof(struct elf32_phdr))
		return elf_reject(BC_ELF_ERR_BAD_PROGRAM_HEADER);
	if (eh->e_phnum > 0xffffffffu / eh->e_phentsize)
		return elf_reject(BC_ELF_ERR_PROGRAM_TABLE_OVERFLOW);
	{
		u32 ph_end = eh->e_phoff + (u32)eh->e_phnum * eh->e_phentsize;

		if (ph_end < eh->e_phoff || ph_end > size)
			return elf_reject(BC_ELF_ERR_PROGRAM_TABLE_OUT_FILE);
	}
	if (!in_ddr(eh->e_entry, 1))
                #ifdef VERBOSE_MINIFY
		return elf_reject(BC_ELF_ERR_ENTRY_NOT_DDR);
                #else
		return elf_reject_hex(BC_ELF_ERR_ENTRY_NOT_DDR, eh->e_entry);
                #endif

	/* Pass 1: reject dynamic / bad segments before touching DDR. */
	for (i = 0; i < eh->e_phnum; i++) {
		const struct elf32_phdr *ph = phdr_at(elf, eh, i);
		u32 file_end;

		if (ph->p_type == PT_INTERP)
			return elf_reject(BC_ELF_ERR_NOT_STATIC_INTERP);
		if (ph->p_type == PT_DYNAMIC)
			return elf_reject(BC_ELF_ERR_NOT_STATIC_DYNAMIC);
		if (ph->p_type != PT_LOAD)
			continue;
		saw_load = 1;
		if (ph->p_filesz > ph->p_memsz)
			return elf_reject(BC_ELF_ERR_BAD_PT_LOAD);
		if (!u32_add_ok(ph->p_offset, ph->p_filesz, &file_end) ||
		    file_end > size)
			return elf_reject(BC_ELF_ERR_PT_LOAD_PAST_FILE);
		if (!in_ddr(ph->p_vaddr, ph->p_memsz))
                        #ifdef VERBOSE_MINIFY           
			return elf_reject(BC_ELF_ERR_PT_LOAD_NOT_DDR);
                        #else
                        return elf_reject_hex(BC_ELF_ERR_PT_LOAD_NOT_DDR,
					      ph->p_vaddr);
                        #endif
		if ((ph->p_flags & PF_X) &&
		    eh->e_entry >= ph->p_vaddr &&
		    eh->e_entry < ph->p_vaddr + ph->p_memsz)
			entry_in_x = 1;
	}
	if (!saw_load)
		return elf_reject(BC_ELF_ERR_NO_PT_LOAD);
	if (!entry_in_x)
                #ifdef VERBOSE_MINIFY           
		return elf_reject(BC_ELF_ERR_ENTRY_NOT_EXEC_PT_LOAD);
                #else
                return elf_reject_hex(BC_ELF_ERR_ENTRY_NOT_EXEC_PT_LOAD,
				      eh->e_entry);
                #endif           

	/* Pass 2: install segments. */
	for (i = 0; i < eh->e_phnum; i++) {
		const struct elf32_phdr *ph = phdr_at(elf, eh, i);

		if (ph->p_type != PT_LOAD)
			continue;
		mem_cpy((u8 *)ph->p_vaddr, elf + ph->p_offset, ph->p_filesz);
		if (ph->p_memsz > ph->p_filesz)
			mem_set((u8 *)ph->p_vaddr + ph->p_filesz, 0,
				ph->p_memsz - ph->p_filesz);
	}
	*entry = eh->e_entry;
	return 0;
}
