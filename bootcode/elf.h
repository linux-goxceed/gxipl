/* SPDX-License-Identifier: MIT */
#ifndef BC_ELF_H
#define BC_ELF_H

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

/* Minify error codes if VERBOSE_MINIFY is defined */

#ifdef VERBOSE_MINIFY
#define BC_ERR "ELFERR"
#define BC_ELF_ERR_TOO_SMALL "1"
#define BC_ELF_ERR_NOT_ELF "2"
#define BC_ELF_ERR_NOT_ELF32 "3"
#define BC_ELF_ERR_NOT_LE "4"
#define BC_ELF_ERR_IDENT "5"
#define BC_ELF_ERR_NOT_STATIC "6"
#define BC_ELF_ERR_ET_REL "7"
#define BC_ELF_ERR_ET_CORE "8"
#define BC_ELF_ERR_E_TYPE_INVALID "9"
#define BC_ELF_ERR_INVALID_MACHINE "10"
#define BC_ELF_ERR_UNSUPPORTED_ELF "11"
#define BC_ELF_ERR_CSKY_ABIV2 "12"
#define BC_ELF_ERR_INVALID_ARCH "13"
#define BC_ELF_ERR_TRUNCATED_ELF "14"
#define BC_ELF_ERR_NO_PROGRAM_HEADERS "15"
#define BC_ELF_ERR_BAD_PROGRAM_HEADER "16"
#define BC_ELF_ERR_PROGRAM_TABLE_OVERFLOW "17"
#define BC_ELF_ERR_PROGRAM_TABLE_OUT_FILE "18"
#define BC_ELF_ERR_ENTRY_NOT_DDR "19"
#define BC_ELF_ERR_NOT_STATIC_INTERP "20"
#define BC_ELF_ERR_NOT_STATIC_DYNAMIC "21"
#define BC_ELF_ERR_BAD_PT_LOAD "22"
#define BC_ELF_ERR_PT_LOAD_PAST_FILE "23"
#define BC_ELF_ERR_PT_LOAD_NOT_DDR "24"
#define BC_ELF_ERR_NO_PT_LOAD "25"
#define BC_ELF_ERR_ENTRY_NOT_EXEC_PT_LOAD "26"
#else
#define BC_ERR "Error: "
#define BC_ELF_ERR_TOO_SMALL "ELF image too small"
#define BC_ELF_ERR_NOT_ELF "not an ELF file"
#define BC_ELF_ERR_NOT_ELF32 "not ELF32 (need 32-bit)"
#define BC_ELF_ERR_NOT_LE "not little-endian ELF"
#define BC_ELF_ERR_IDENT "unsupported ELF ident version"
#define BC_ELF_ERR_NOT_STATIC "not a statically linked executable (PIE/ET_DYN)"
#define BC_ELF_ERR_ET_REL "not an executable (relocatable ET_REL)"
#define BC_ELF_ERR_ET_CORE "not an executable (core dump)"
#define BC_ELF_ERR_E_TYPE_INVALID "not an executable (bad e_type)"
#define BC_ELF_ERR_INVALID_MACHINE "wrong machine type (need EM_CSKY=252)"
#define BC_ELF_ERR_UNSUPPORTED_ELF "unsupported ELF version"
#define BC_ELF_ERR_CSKY_ABIV2 "invalid architecture (C-SKY ABIv2)"
#define BC_ELF_ERR_INVALID_ARCH "invalid architecture (need C-SKY ABIv1)"
#define BC_ELF_ERR_TRUNCATED_ELF "truncated ELF header"
#define BC_ELF_ERR_NO_PROGRAM_HEADERS "no program headers"
#define BC_ELF_ERR_BAD_PROGRAM_HEADER "bad program header size"
#define BC_ELF_ERR_PROGRAM_TABLE_OVERFLOW "program header table overflow"
#define BC_ELF_ERR_PROGRAM_TABLE_OUT_FILE "program header table out of file"
#define BC_ELF_ERR_ENTRY_NOT_DDR "entry address not in DDR"
#define BC_ELF_ERR_NOT_STATIC_INTERP "not statically linked (has PT_INTERP)"
#define BC_ELF_ERR_NOT_STATIC_DYNAMIC "not statically linked (has PT_DYNAMIC)"
#define BC_ELF_ERR_BAD_PT_LOAD "bad PT_LOAD (filesz > memsz)"
#define BC_ELF_ERR_PT_LOAD_PAST_FILE "PT_LOAD extends past file"
#define BC_ELF_ERR_PT_LOAD_NOT_DDR "PT_LOAD address not in DDR"
#define BC_ELF_ERR_NO_PT_LOAD "no PT_LOAD segments"
#define BC_ELF_ERR_ENTRY_NOT_EXEC_PT_LOAD "entry not in an executable PT_LOAD"
#endif

#endif
