#include "boot_freebsd.h"
#include "../include/config.h"
#include "../include/freebsd.h"
#include "utils.h"
#include <stddef.h>

static struct freebsd_info info;

static volatile int exited_cpus = 0;

struct range {
  uint64_t start;
  uint64_t end;
};

static struct freebsd_smap_entry smap[FREEBSD_SMAP_MAX];
static struct range reserved_ranges[96];
static struct range work_ranges[FREEBSD_SMAP_MAX];
static int smap_count;
static int reserved_count;

#define VRAM_MARKER_BYTES (16ULL * 1024ULL * 1024ULL)
#define VRAM_MARKER_BAR_BYTES (1024ULL * 1024ULL)

static void configure_vram(uint64_t fb_start, uint64_t vram_start,
                           uint64_t vram_size) {
  uint64_t vram_end = vram_start + vram_size - 1;
  uint64_t fb_top = fb_start + vram_size - 1;

  *(uint32_t *)(AMDGPU_MMIO_BASE + RCC_CONFIG_MEMSIZE) = vram_size >> 20;

  *(uint32_t *)(AMDGPU_MMIO_BASE + GCMC_VM_FB_OFFSET) = vram_start >> 24;

  *(uint32_t *)(AMDGPU_MMIO_BASE + GCMC_VM_LOCAL_HBM_ADDRESS_START) =
      vram_start >> 24;
  *(uint32_t *)(AMDGPU_MMIO_BASE + GCMC_VM_LOCAL_HBM_ADDRESS_END) =
      vram_end >> 24;
  *(uint32_t *)(AMDGPU_MMIO_BASE + GCMC_VM_FB_LOCATION_BASE) = fb_start >> 24;
  *(uint32_t *)(AMDGPU_MMIO_BASE + GCMC_VM_FB_LOCATION_TOP) = fb_top >> 24;

  *(uint32_t *)(AMDGPU_MMIO_BASE + MMMC_VM_FB_OFFSET) = vram_start >> 24;
  *(uint32_t *)(AMDGPU_MMIO_BASE + MMMC_VM_LOCAL_HBM_ADDRESS_START) =
      vram_start >> 24;
  *(uint32_t *)(AMDGPU_MMIO_BASE + MMMC_VM_LOCAL_HBM_ADDRESS_END) =
      vram_end >> 24;
  *(uint32_t *)(AMDGPU_MMIO_BASE + MMMC_VM_FB_LOCATION_BASE) = fb_start >> 24;
  *(uint32_t *)(AMDGPU_MMIO_BASE + MMMC_VM_FB_LOCATION_TOP) = fb_top >> 24;

  *(uint32_t *)(AMDGPU_MMIO_BASE + MMHUBBUB_WHITELIST_BASE_ADDR_0) =
      vram_start >> 12;
  *(uint32_t *)(AMDGPU_MMIO_BASE + MMHUBBUB_WHITELIST_TOP_ADDR_0) =
      vram_end >> 12;
  *(uint32_t *)(AMDGPU_MMIO_BASE + DCHUBBUB_WHITELIST_BASE_ADDR_0) =
      vram_start >> 12;
  *(uint32_t *)(AMDGPU_MMIO_BASE + DCHUBBUB_WHITELIST_TOP_ADDR_0) =
      vram_end >> 12;
}

static uint32_t marker_color(uint64_t offset, uint32_t stage_color) {
  switch ((offset / VRAM_MARKER_BAR_BYTES) & 7) {
  case 0:
  case 1:
  case 2:
  case 3:
    return stage_color;
  case 4:
    return 0x00ff0000U;
  case 5:
    return 0x0000ff00U;
  case 6:
    return 0x000000ffU;
  default:
    return 0x00ffffffU;
  }
}

static void paint_vram_marker(uint32_t stage_color) {
  if (info.vram_size < sizeof(uint32_t))
    return;

  uint64_t bytes = info.vram_size;
  if (bytes > VRAM_MARKER_BYTES)
    bytes = VRAM_MARKER_BYTES;
  bytes &= ~(uint64_t)(sizeof(uint32_t) - 1);

  volatile uint32_t *fb = (volatile uint32_t *)VRAM_BASE;
  for (uint64_t offset = 0; offset < bytes; offset += sizeof(uint32_t)) {
    fb[offset / sizeof(uint32_t)] = marker_color(offset, stage_color);
  }
  __asm__ volatile("mfence" : : : "memory");
}

static uint64_t freebsd_phdr_pa(const Elf64_Phdr *phdr) {
  if (phdr->p_paddr != 0 && phdr->p_paddr < 0x100000000ULL)
    return phdr->p_paddr;
  return phdr->p_vaddr - FREEBSD_KERNBASE;
}

static void smap_append(uint64_t start, uint64_t end, uint32_t type) {
  if (start >= end || smap_count >= FREEBSD_SMAP_MAX)
    return;
  smap[smap_count].base = start;
  smap[smap_count].length = end - start;
  smap[smap_count].type = type;
  smap_count++;
}

static void reserve_append(uint64_t start, uint64_t end) {
  if (start >= end || reserved_count >=
                          (int)(sizeof(reserved_ranges) /
                                sizeof(reserved_ranges[0])))
    return;
  reserved_ranges[reserved_count].start = start;
  reserved_ranges[reserved_count].end = end;
  reserved_count++;
}

static void smap_append_ram(uint64_t start, uint64_t end) {
  int n = 1;
  work_ranges[0].start = start;
  work_ranges[0].end = end;

  for (int r = 0; r < reserved_count; r++) {
    for (int i = 0; i < n; i++) {
      uint64_t rs = reserved_ranges[r].start;
      uint64_t re = reserved_ranges[r].end;
      uint64_t s = work_ranges[i].start;
      uint64_t e = work_ranges[i].end;

      if (re <= s || rs >= e)
        continue;

      if (rs <= s && re >= e) {
        work_ranges[i] = work_ranges[n - 1];
        n--;
        i--;
        continue;
      }

      if (rs <= s) {
        work_ranges[i].start = re;
        continue;
      }

      if (re >= e) {
        work_ranges[i].end = rs;
        continue;
      }

      if (n < FREEBSD_SMAP_MAX) {
        work_ranges[n].start = re;
        work_ranges[n].end = e;
        n++;
      }
      work_ranges[i].end = rs;
    }
  }

  for (int i = 0; i < n; i++) {
    smap_append(work_ranges[i].start, work_ranges[i].end,
                FREEBSD_SMAP_TYPE_MEMORY);
  }
}

static void build_smap(void) {
  uint64_t vram_base = VRAM_BASE;

  smap_count = 0;
  reserved_count = 0;

  reserve_append(0x000000000ULL, 0x000001000ULL);
  reserve_append(0x000070000ULL, 0x000100000ULL);
  reserve_append(FREEBSD_PT_BASE, FREEBSD_PT_BASE + FREEBSD_PT_SIZE);
  reserve_append(0x03fffc000ULL, 0x040000000ULL);
  reserve_append(0x060000000ULL, 0x060800000ULL);
  reserve_append(0x060800000ULL, 0x060c00000ULL);
  reserve_append(0x062800000ULL, 0x064800000ULL);
  reserve_append(0x064800000ULL, 0x064829000ULL);
  reserve_append(0x07f9d0000ULL, 0x07fd5f000ULL);
  reserve_append(0x07fd5f000ULL, 0x07fd63000ULL);
  reserve_append(0x07fd63000ULL, 0x07fd67000ULL);
  reserve_append(0x07fd67000ULL, 0x07fd6f000ULL);
  reserve_append(0x07fd6f000ULL, 0x07fd8f000ULL);
  reserve_append(0x07fd8f000ULL, 0x080000000ULL);
  reserve_append(0x080000000ULL, 0x0c4400000ULL);
  reserve_append(0x0d0000000ULL, 0x0e0700000ULL);
  reserve_append(0x0f0000000ULL, 0x0f8000000ULL);
  reserve_append(vram_base, 0x470000000ULL);

  if (info.kit_type != KIT_DEVKIT)
    reserve_append(0x47f300000ULL, 0x480000000ULL);
  else
    reserve_append(0x87f300000ULL, 0x880000000ULL);

  for (int i = 0; i < info.n_tmrs; i++) {
    reserve_append(info.tmrs[i].start, info.tmrs[i].end);
  }

  smap_append_ram(0x000001000ULL, 0x000070000ULL);
  smap_append_ram(0x000100000ULL, 0x03fffc000ULL);
  smap_append_ram(0x040000000ULL, 0x060000000ULL);
  smap_append_ram(0x060c00000ULL, 0x062800000ULL);
  smap_append_ram(0x064829000ULL, 0x07f9d0000ULL);
  smap_append_ram(0x100000000ULL, vram_base);

  if (info.kit_type != KIT_DEVKIT)
    smap_append_ram(0x470000000ULL, 0x47f300000ULL);
  else
    smap_append_ram(0x470000000ULL, 0x87f300000ULL);

  for (int i = 0; i < reserved_count; i++) {
    uint32_t type = FREEBSD_SMAP_TYPE_RESERVED;
    if (reserved_ranges[i].start == 0x07fd67000ULL)
      type = FREEBSD_SMAP_TYPE_ACPI_NVS;
    else if (reserved_ranges[i].start == 0x07fd6f000ULL)
      type = FREEBSD_SMAP_TYPE_ACPI_RECLAIM;
    smap_append(reserved_ranges[i].start, reserved_ranges[i].end, type);
  }
}

static uint64_t sc_strlen(const char *s) {
  uint64_t len = 0;
  while (s[len] != 0)
    len++;
  return len;
}

static uint8_t *md_add(uint8_t *p, uint32_t type, const void *data,
                       uint32_t size) {
  *(uint32_t *)p = type;
  p += sizeof(uint32_t);
  *(uint32_t *)p = size;
  p += sizeof(uint32_t);
  memcpy(p, (void *)data, size);
  p += ALIGN_UP(size, sizeof(uint64_t));
  return p;
}

static uint8_t *md_add_string(uint8_t *p, uint32_t type, const char *s) {
  return md_add(p, type, s, sc_strlen(s) + 1);
}

static uint8_t *md_add_u64(uint8_t *p, uint32_t type, uint64_t value) {
  return md_add(p, type, &value, sizeof(value));
}

static uint8_t *md_add_u32(uint8_t *p, uint32_t type, uint32_t value) {
  return md_add(p, type, &value, sizeof(value));
}

static void build_metadata(void) {
  uint8_t *p = (uint8_t *)info.modulep;
  uint64_t envp = info.modulep + FREEBSD_METADATA_MAX;
  uint32_t howto = 0;
  Elf64_Ehdr *ehdr = (Elf64_Ehdr *)info.kernel;

  build_smap();

  p = md_add_string(p, FREEBSD_MODINFO_NAME, FREEBSD_KERNEL_NAME);
  p = md_add_string(p, FREEBSD_MODINFO_TYPE, FREEBSD_KERNTYPE);
  p = md_add_u64(p, FREEBSD_MODINFO_ADDR, info.first_pa);
  p = md_add_u64(p, FREEBSD_MODINFO_SIZE, info.last_pa - info.first_pa);
  p = md_add(p, FREEBSD_MODINFO_METADATA | FREEBSD_MODINFOMD_ELFHDR, ehdr,
             sizeof(*ehdr));
  p = md_add_u32(p, FREEBSD_MODINFO_METADATA | FREEBSD_MODINFOMD_HOWTO,
                 howto);
  p = md_add_u64(p, FREEBSD_MODINFO_METADATA | FREEBSD_MODINFOMD_ENVP, envp);
  p = md_add_u64(p, FREEBSD_MODINFO_METADATA | FREEBSD_MODINFOMD_KERNEND,
                 info.kernend);
  p = md_add_u64(p, FREEBSD_MODINFO_METADATA | FREEBSD_MODINFOMD_MODULEP,
                 info.modulep);
  p = md_add(p, FREEBSD_MODINFO_METADATA | FREEBSD_MODINFOMD_SMAP, smap,
             smap_count * sizeof(smap[0]));

  *(uint32_t *)p = FREEBSD_MODINFO_END;
  p += sizeof(uint32_t);
  *(uint32_t *)p = 0;
}

static void load_kernel_segments(void) {
  Elf64_Ehdr *ehdr = (Elf64_Ehdr *)info.kernel;
  Elf64_Phdr *phdr = (Elf64_Phdr *)(info.kernel + ehdr->e_phoff);

  for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
    if (phdr[i].p_type != FREEBSD_PT_LOAD)
      continue;

    uint64_t pa = freebsd_phdr_pa(&phdr[i]);
    memcpy((void *)pa, (void *)(info.kernel + phdr[i].p_offset),
           phdr[i].p_filesz);
    if (phdr[i].p_memsz > phdr[i].p_filesz) {
      memset((void *)(pa + phdr[i].p_filesz), 0,
             phdr[i].p_memsz - phdr[i].p_filesz);
    }
  }
}

static void build_freebsd_page_tables(void) {
  uint64_t *pml4 = (uint64_t *)FREEBSD_PT_BASE;
  uint64_t *pdpt_l = (uint64_t *)(FREEBSD_PT_BASE + 0x1000);
  uint64_t *pd_l0 = (uint64_t *)(FREEBSD_PT_BASE + 0x2000);
  uint64_t *pd_l1 = (uint64_t *)(FREEBSD_PT_BASE + 0x3000);
  uint64_t *pd_l2 = (uint64_t *)(FREEBSD_PT_BASE + 0x4000);
  uint64_t *pd_l3 = (uint64_t *)(FREEBSD_PT_BASE + 0x5000);
  uint64_t *pd_l4 = (uint64_t *)(FREEBSD_PT_BASE + 0x6000);
  uint64_t *pdpt_u = (uint64_t *)(FREEBSD_PT_BASE + 0x7000);
  uint64_t *pd_u0 = (uint64_t *)(FREEBSD_PT_BASE + 0x8000);
  uint64_t *pd_u1 = (uint64_t *)(FREEBSD_PT_BASE + 0x9000);
  uint64_t *lower_pds[] = {pd_l0, pd_l1, pd_l2, pd_l3, pd_l4};
  const uint64_t pg_v = 0x001ULL;
  const uint64_t pg_rw = 0x002ULL;
  const uint64_t pg_ps = 0x080ULL;

  memset((void *)FREEBSD_PT_BASE, 0, FREEBSD_PT_SIZE);

  pml4[0] = (uint64_t)pdpt_l | pg_v | pg_rw;
  for (uint64_t gb = 0; gb < 5; gb++) {
    pdpt_l[gb] = (uint64_t)lower_pds[gb] | pg_v | pg_rw;
    for (uint64_t i = 0; i < 512; i++) {
      lower_pds[gb][i] = (gb * 0x40000000ULL + i * 0x200000ULL) | pg_v |
                         pg_rw | pg_ps;
    }
  }

  pml4[511] = (uint64_t)pdpt_u | pg_v | pg_rw;
  pdpt_u[510] = (uint64_t)pd_u0 | pg_v | pg_rw;
  pdpt_u[511] = (uint64_t)pd_u1 | pg_v | pg_rw;
  for (uint64_t i = 0; i < 512; i++) {
    pd_u0[i] = (i * 0x200000ULL) | pg_v | pg_rw | pg_ps;
    pd_u1[i] = (0x40000000ULL + i * 0x200000ULL) | pg_v | pg_rw | pg_ps;
  }
}

__attribute__((noreturn)) static void enter_freebsd(void) {
  uint64_t modulep = (uint64_t)(uint32_t)info.modulep << 32;
  uint64_t kernend = (uint64_t)(uint32_t)info.kernend;

  paint_vram_marker(0x0000ff00U);
  printf("[freebsd] final handoff\n");
  print_hex64((const uint8_t *)"[freebsd] entry=", info.entry);
  print_hex64((const uint8_t *)"[freebsd] cr3=", FREEBSD_PT_BASE);
  print_hex64((const uint8_t *)"[freebsd] stack=", FREEBSD_TRAMP_STACK);
  print_hex64((const uint8_t *)"[freebsd] modulep=", info.modulep);
  print_hex64((const uint8_t *)"[freebsd] kernend=", info.kernend);

  __asm__ volatile("movq %[stack], %%rsp\n\t"
                   "pushq %[kernend]\n\t"
                   "pushq %[modulep]\n\t"
                   "pushq %[entry]\n\t"
                   "movq %[cr3], %%cr3\n\t"
                   "retq\n\t"
                   :
                   : [stack] "r"(FREEBSD_TRAMP_STACK),
                     [kernend] "r"(kernend), [modulep] "r"(modulep),
                     [entry] "r"(info.entry), [cr3] "r"(FREEBSD_PT_BASE)
                   : "memory");
  __builtin_unreachable();
}

void boot_freebsd(void) {
  printf("[freebsd] load kernel segments\n");
  load_kernel_segments();
  printf("[freebsd] copy kenv\n");
  memcpy((void *)(info.modulep + FREEBSD_METADATA_MAX), (void *)info.env,
         info.env_size + 1);
  printf("[freebsd] build metadata\n");
  build_metadata();
  print_hex64((const uint8_t *)"[freebsd] smap_count=", smap_count);
  printf("[freebsd] build page tables\n");
  build_freebsd_page_tables();
  printf("[freebsd] enter btext\n");
  enter_freebsd();
}

void entry(void) {
  disable_intr();

  // Set global interrupt flag.
  __asm__ volatile("stgi\n");

  // Clear SVM flag.
  wrmsr(MSR_EFER, rdmsr(MSR_EFER) & ~EFER_SVM);

  // Disable INIT redirection.
  wrmsr(MSR_VM_CR, rdmsr(MSR_VM_CR) & ~VM_CR_R_INIT);

  // Clean up mtrr.
  wrmsr(MSR_MTRR4kBase + 0, 0);
  wrmsr(MSR_MTRR4kBase + 1, 0);
  wrmsr(MSR_MTRRVarBase + 7 * 2 + 1, 0);

  atomic_add_32(&exited_cpus, 1);

  while (atomic_cmpset_32(&exited_cpus, MAXCPU, MAXCPU) == 0)
    ;

  if (get_cpu() != 0) {
    while (1) {
      halt();
    }
  }

  // Disable IOMMU.
  *(volatile uint64_t *)(AMDIOMMU_MMIO_BASE + AMDIOMMU_CTRL) &= ~1;

  memcpy(&info, (void *)(cave_freebsd_info), sizeof(struct freebsd_info));

  configure_vram(FB_BASE, VRAM_BASE, info.vram_size);
  paint_vram_marker(0x00ff0000U);

  printf("[*] Booting FreeBSD in bare metal...\n");
  print_hex64((const uint8_t *)"[freebsd] kernel=", info.kernel);
  print_hex64((const uint8_t *)"[freebsd] kernel_size=", info.kernel_size);
  print_hex64((const uint8_t *)"[freebsd] env=", info.env);
  print_hex64((const uint8_t *)"[freebsd] env_size=", info.env_size);
  print_hex64((const uint8_t *)"[freebsd] first_pa=", info.first_pa);
  print_hex64((const uint8_t *)"[freebsd] last_pa=", info.last_pa);
  print_hex64((const uint8_t *)"[freebsd] vram_size=", info.vram_size);
  print_hex64((const uint8_t *)"[freebsd] n_tmrs=", info.n_tmrs);
  boot_freebsd();
}
