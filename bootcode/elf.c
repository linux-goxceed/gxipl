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

extern int g_verbose;

#define EI_NIDENT	16
#define ET_NONE		0
#define ET_REL		1
#define ET_EXEC		2
#define ET_DYN		3
#define ET_CORE		4
#define EM_CSKY		252
#define EV_CURRENT	1

#define PT_LOAD		1
#define PT_DYNAMIC	2
#define PT_INTERP	3

#define PF_X		0x1u

/* binutils: EF_CSKY_ABIV1 / EF_CSKY_ABIV2 */
#define EF_CSKY_ABIV1	0x10000000u
#define EF_CSKY_ABIV2	0x20000000u

/* Post-MMU cached DDR VA (start.S); matches 64 MiB Gemini DRAM. */
#define ELF_DDR_BASE	DDR_VIRT_BASE
#define ELF_DDR_SIZE	DDR_SIZE
#define ELF_DDR_END	(ELF_DDR_BASE + ELF_DDR_SIZE)

struct elf32_hdr {
	u8 e_ident[EI_NIDENT];
	u16 e_type;
	u16 e_machine;
	u32 e_version;
	u32 e_entry;
	u32 e_phoff;
	u32 e_shoff;
	u32 e_flags;
	u16 e_ehsize;
	u16 e_phentsize;
	u16 e_phnum;
	u16 e_shentsize;
	u16 e_shnum;
	u16 e_shstrndx;
};

struct elf32_phdr {
	u32 p_type;
	u32 p_offset;
	u32 p_vaddr;
	u32 p_paddr;
	u32 p_filesz;
	u32 p_memsz;
	u32 p_flags;
	u32 p_align;
};

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
		bc_puts("Error: ");
		bc_puts(why);
		bc_puts("\r\n");
	}
	return -1;
}

static int elf_reject_hex(const char *why, u32 value)
{
	if (g_verbose) {
		bc_puts("Error: ");
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
		bc_puts("Error: ");
		bc_puts(why);
		bc_puts(" (");
		bc_put_dec(value);
		bc_puts(")\r\n");
	}
	return -1;
}

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
		return elf_reject("ELF image too small");
	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
		return elf_reject("not an ELF file");
	/* ELFCLASS32, ELFDATA2LSB, EV_CURRENT */
	if (eh->e_ident[4] != 1)
		return elf_reject("not ELF32 (need 32-bit)");
	if (eh->e_ident[5] != 1)
		return elf_reject("not little-endian ELF");
	if (eh->e_ident[6] != 1)
		return elf_reject("unsupported ELF ident version");
	if (eh->e_type == ET_DYN)
		return elf_reject("not a statically linked executable (PIE/ET_DYN)");
	if (eh->e_type == ET_REL)
		return elf_reject("not an executable (relocatable ET_REL)");
	if (eh->e_type == ET_CORE)
		return elf_reject("not an executable (core dump)");
	if (eh->e_type != ET_EXEC)
		return elf_reject_dec("not an executable (bad e_type)", eh->e_type);
	if (eh->e_machine != EM_CSKY)
		return elf_reject_dec("wrong machine type (need EM_CSKY=252)",
				      eh->e_machine);
	if (eh->e_version != EV_CURRENT)
		return elf_reject("unsupported ELF version");
	/* CK610 open IPL / U-Boot toolchain is ABIv1 only. */
	if (eh->e_flags & EF_CSKY_ABIV2)
		return elf_reject_hex("invalid architecture (C-SKY ABIv2)",
				      eh->e_flags);
	if (!(eh->e_flags & EF_CSKY_ABIV1))
		return elf_reject_hex("invalid architecture (need C-SKY ABIv1)",
				      eh->e_flags);
	if (eh->e_ehsize < sizeof(*eh))
		return elf_reject("truncated ELF header");
	if (!eh->e_phnum)
		return elf_reject("no program headers");
	if (eh->e_phentsize < sizeof(struct elf32_phdr))
		return elf_reject("bad program header size");
	if (eh->e_phnum > 0xffffffffu / eh->e_phentsize)
		return elf_reject("program header table overflow");
	{
		u32 ph_end = eh->e_phoff + (u32)eh->e_phnum * eh->e_phentsize;

		if (ph_end < eh->e_phoff || ph_end > size)
			return elf_reject("program header table out of file");
	}
	if (!in_ddr(eh->e_entry, 1))
		return elf_reject_hex("entry address not in DDR", eh->e_entry);

	/* Pass 1: reject dynamic / bad segments before touching DDR. */
	for (i = 0; i < eh->e_phnum; i++) {
		const struct elf32_phdr *ph = phdr_at(elf, eh, i);
		u32 file_end;

		if (ph->p_type == PT_INTERP)
			return elf_reject("not statically linked (has PT_INTERP)");
		if (ph->p_type == PT_DYNAMIC)
			return elf_reject("not statically linked (has PT_DYNAMIC)");
		if (ph->p_type != PT_LOAD)
			continue;
		saw_load = 1;
		if (ph->p_filesz > ph->p_memsz)
			return elf_reject("bad PT_LOAD (filesz > memsz)");
		if (!u32_add_ok(ph->p_offset, ph->p_filesz, &file_end) ||
		    file_end > size)
			return elf_reject("PT_LOAD extends past file");
		if (!in_ddr(ph->p_vaddr, ph->p_memsz))
			return elf_reject_hex("PT_LOAD address not in DDR",
					      ph->p_vaddr);
		if ((ph->p_flags & PF_X) &&
		    eh->e_entry >= ph->p_vaddr &&
		    eh->e_entry < ph->p_vaddr + ph->p_memsz)
			entry_in_x = 1;
	}
	if (!saw_load)
		return elf_reject("no PT_LOAD segments");
	if (!entry_in_x)
		return elf_reject_hex("entry not in an executable PT_LOAD",
				      eh->e_entry);

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
