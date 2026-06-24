#ifndef __FREEBSD_H__
#define __FREEBSD_H__

#include <stddef.h>
#include <stdint.h>

#define FREEBSD_KERNBASE 0xffffffff80000000ULL
#define FREEBSD_KERNLOAD 0x200000ULL
#define FREEBSD_KERNSTART (FREEBSD_KERNBASE + FREEBSD_KERNLOAD)
#define FREEBSD_X86_PAGE_SIZE 0x1000ULL
#define FREEBSD_METADATA_MAX (64ULL * 1024ULL)
#define FREEBSD_ENV_MAX (16ULL * 1024ULL)

#define FREEBSD_PT_BASE 0x80000ULL
#define FREEBSD_PT_SIZE 0xa000ULL
#define FREEBSD_TRAMP_STACK 0x9f000ULL

#define FREEBSD_ELF_NIDENT 16
#define FREEBSD_EI_CLASS 4
#define FREEBSD_EI_DATA 5
#define FREEBSD_ELFCLASS64 2
#define FREEBSD_ELFDATA2LSB 1
#define FREEBSD_ET_EXEC 2
#define FREEBSD_EM_X86_64 62
#define FREEBSD_PT_LOAD 1

#define FREEBSD_MODINFO_END 0x0000
#define FREEBSD_MODINFO_NAME 0x0001
#define FREEBSD_MODINFO_TYPE 0x0002
#define FREEBSD_MODINFO_ADDR 0x0003
#define FREEBSD_MODINFO_SIZE 0x0004
#define FREEBSD_MODINFO_METADATA 0x8000

#define FREEBSD_MODINFOMD_ELFHDR 0x0002
#define FREEBSD_MODINFOMD_ENVP 0x0006
#define FREEBSD_MODINFOMD_HOWTO 0x0007
#define FREEBSD_MODINFOMD_KERNEND 0x0008
#define FREEBSD_MODINFOMD_SMAP 0x1001
#define FREEBSD_MODINFOMD_MODULEP 0x1006

#define FREEBSD_KERNTYPE "elf kernel"
#define FREEBSD_KERNEL_NAME "/PS5/FreeBSD/kernel"

#define FREEBSD_SMAP_TYPE_MEMORY 1
#define FREEBSD_SMAP_TYPE_RESERVED 2
#define FREEBSD_SMAP_TYPE_ACPI_RECLAIM 3
#define FREEBSD_SMAP_TYPE_ACPI_NVS 4
#define FREEBSD_SMAP_MAX 160

typedef struct {
  uint64_t start;
  uint64_t end;
} tmr;

typedef struct {
  unsigned char e_ident[FREEBSD_ELF_NIDENT];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

struct freebsd_smap_entry {
  uint64_t base;
  uint64_t length;
  uint32_t type;
} __attribute__((packed));

struct freebsd_info {
  uintptr_t freebsd_info;
  uintptr_t kernel;
  size_t kernel_size;
  uintptr_t env;
  size_t env_size;
  uintptr_t modulep;
  uintptr_t kernend;
  uintptr_t kernend_pa;
  uintptr_t entry;
  uintptr_t first_pa;
  uintptr_t last_pa;
  size_t vram_size;
  int kit_type;
  int n_tmrs;
  tmr tmrs[64];
};

#endif
