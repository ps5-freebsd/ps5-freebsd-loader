#include "loader.h"
#include "config.h"
#include "firmware.h"
#include "freebsd.h"
#include "utils.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

const char *file_paths[] = {
    "/mnt/usb0/",
    "/mnt/usb1/",
    "/mnt/usb2/",
    "/mnt/usb3/",
    "/mnt/usb0/PS5/FreeBSD/",
    "/mnt/usb1/PS5/FreeBSD/",
    "/mnt/usb2/PS5/FreeBSD/",
    "/mnt/usb3/PS5/FreeBSD/",
};

long find_and_get_size_of_file(const char *filename, char *found_path);
int find_and_read_file(const char *filename, void *buf, size_t bufsize);

static const char *get_overridden_filename(const char *filename) {
  static int state = 0;
  static char *overrides_start = nullptr;
  static char *overrides_end = nullptr;

  if (state == 0) {
    state = 1;
    char found_path[256];
    ssize_t size = find_and_get_size_of_file("path-override.txt", found_path);
    if (size > 0) {
      overrides_start = malloc(size + 1);
      if (overrides_start != NULL &&
          read_file(found_path, overrides_start, size) == size) {
        overrides_end = overrides_start + size + 1;
        state = 2;
        for (char *p = overrides_start; p < overrides_end; p++)
          if (*p == '\n')
            *p = 0;
        // make sure the last string is null-terminated
        overrides_start[size] = 0;
      }
    }
  }

  // overrides not found, or unreadable, or currently looking for it
  if (state == 1)
    return filename;

  size_t needle_len = strlen(filename);
  for (const char *p = overrides_start; p < overrides_end;) {
    size_t haystack_len = strlen(p);
    if (haystack_len > needle_len && !strncmp(p, filename, needle_len) &&
        p[needle_len] == '=')
      return p + needle_len + 1;
    p += haystack_len + 1;
  }

  // haven't found an override, return original filename
  return filename;
}

static int copy_path(char *dst, size_t dst_size, const char *src) {
  if (strlen(src) >= dst_size)
    return -1;
  strcpy(dst, src);
  return 0;
}

long find_and_get_size_of_file(const char *filename, char *found_path) {
  char full_path[256];
  struct stat st;

  filename = get_overridden_filename(filename);
  if (filename[0] == '/') {
    if (stat(filename, &st) == 0 && copy_path(found_path, 256, filename) == 0) {
      notify("File '%s' found by absolute override\n", filename);
      return st.st_size;
    }
    return -1;
  }
  int num_paths = sizeof(file_paths) / sizeof(file_paths[0]);

  for (int i = 0; i < num_paths; i++) {
    snprintf(full_path, sizeof(full_path), "%s%s", file_paths[i], filename);

    if (stat(full_path, &st) == 0) {
      notify("File '%s' found in '%s'\n", filename, file_paths[i]);
      if (copy_path(found_path, 256, full_path) != 0)
        return -1;
      return st.st_size;
    }
  }

  return -1;
}

int find_and_read_file(const char *filename, void *buf, size_t bufsize) {
  char full_path[256];
  struct stat st;

  filename = get_overridden_filename(filename);
  if (filename[0] == '/') {
    if (stat(filename, &st) == 0) {
      notify("File '%s' found by absolute override\n", filename);
      return read_file(filename, buf, bufsize);
    }
    return -1;
  }
  int num_paths = sizeof(file_paths) / sizeof(file_paths[0]);

  for (int i = 0; i < num_paths; i++) {
    snprintf(full_path, sizeof(full_path), "%s%s", file_paths[i], filename);

    if (stat(full_path, &st) == 0) {
      notify("File '%s' found in '%s'\n", filename, file_paths[i]);
      return read_file(full_path, buf, bufsize);
    }
  }

  return -1;
}

int read_file(const char *path, void *buf, size_t bufsize) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return fd;
  int r = read(fd, buf, bufsize);
  close(fd);
  return r;
}

void trim_newline(char *s) {
  while (*s != '\0') {
    if (*s == '\r' || *s == '\n') {
      *s = '\0';
      break;
    }
    s++;
  }
}

static uint64_t freebsd_phdr_pa(const Elf64_Phdr *phdr) {
  if (phdr->p_paddr != 0 && phdr->p_paddr < 0x100000000ULL)
    return phdr->p_paddr;
  if (phdr->p_vaddr >= FREEBSD_KERNBASE)
    return phdr->p_vaddr - FREEBSD_KERNBASE;
  return UINT64_MAX;
}

static int validate_freebsd_kernel(void *kernel, size_t kernel_size,
                                   struct freebsd_info *info) {
  if (kernel_size < sizeof(Elf64_Ehdr)) {
    notify("FreeBSD kernel is too small for an ELF header - Aborting\n");
    return -1;
  }

  Elf64_Ehdr *ehdr = (Elf64_Ehdr *)kernel;
  if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
      ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F' ||
      ehdr->e_ident[FREEBSD_EI_CLASS] != FREEBSD_ELFCLASS64 ||
      ehdr->e_ident[FREEBSD_EI_DATA] != FREEBSD_ELFDATA2LSB ||
      ehdr->e_type != FREEBSD_ET_EXEC ||
      ehdr->e_machine != FREEBSD_EM_X86_64 ||
      ehdr->e_ehsize != sizeof(Elf64_Ehdr) ||
      ehdr->e_phentsize != sizeof(Elf64_Phdr) || ehdr->e_phnum == 0) {
    notify("FreeBSD kernel is not an amd64 ELF64 ET_EXEC kernel - Aborting\n");
    return -1;
  }

  uint64_t phdr_bytes = (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr);
  if (ehdr->e_phoff > kernel_size || phdr_bytes > kernel_size - ehdr->e_phoff) {
    notify("FreeBSD kernel program headers are outside the file - Aborting\n");
    return -1;
  }

  if (ehdr->e_entry < FREEBSD_KERNSTART) {
    notify("FreeBSD kernel entry is below KERNSTART - Aborting\n");
    return -1;
  }

  Elf64_Phdr *phdr = (Elf64_Phdr *)((char *)kernel + ehdr->e_phoff);
  uint64_t first_pa = UINT64_MAX;
  uint64_t last_pa = 0;
  int load_segments = 0;

  for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
    if (phdr[i].p_type != FREEBSD_PT_LOAD)
      continue;
    if (phdr[i].p_memsz < phdr[i].p_filesz ||
        phdr[i].p_offset > kernel_size ||
        phdr[i].p_filesz > kernel_size - phdr[i].p_offset) {
      notify("FreeBSD kernel has a malformed PT_LOAD segment - Aborting\n");
      return -1;
    }

    uint64_t pa = freebsd_phdr_pa(&phdr[i]);
    if (pa == UINT64_MAX || phdr[i].p_memsz > 0x100000000ULL - pa) {
      notify("FreeBSD kernel PT_LOAD segment is not below 4GB - Aborting\n");
      return -1;
    }

    if (pa < first_pa)
      first_pa = pa;
    if (pa + phdr[i].p_memsz > last_pa)
      last_pa = pa + phdr[i].p_memsz;
    load_segments++;
  }

  if (load_segments == 0 || first_pa != FREEBSD_KERNLOAD) {
    notify("FreeBSD kernel does not load at amd64 KERNLOAD - Aborting\n");
    return -1;
  }

  info->entry = ehdr->e_entry;
  info->first_pa = first_pa;
  info->last_pa = last_pa;
  return 0;
}

static int append_kenv(char *env, size_t *env_size, const char *line,
                       size_t line_len) {
  while (line_len > 0 && (line[line_len - 1] == '\r' ||
                          line[line_len - 1] == '\n' ||
                          line[line_len - 1] == ' ' ||
                          line[line_len - 1] == '\t')) {
    line_len--;
  }
  while (line_len > 0 && (*line == ' ' || *line == '\t')) {
    line++;
    line_len--;
  }

  if (line_len == 0 || line[0] == '#')
    return 0;

  int has_equals = 0;
  for (size_t i = 0; i < line_len; i++) {
    if (line[i] == '=') {
      has_equals = 1;
      break;
    }
  }
  if (!has_equals)
    return 0;

  if (*env_size + line_len + 2 > FREEBSD_ENV_MAX)
    return -1;

  memcpy(env + *env_size, line, line_len);
  *env_size += line_len;
  env[(*env_size)++] = '\0';
  env[*env_size] = '\0';
  return 0;
}

static int build_kenv(char *env, size_t *env_size, size_t vram_size,
                      int kit_type) {
  *env_size = 0;

  char default_line[96];
  int len = snprintf(default_line, sizeof(default_line),
                     "hw.ps5.vram_size=0x%zx", vram_size);
  if (len <= 0 ||
      append_kenv(env, env_size, default_line, (size_t)len) != 0)
    return -1;

  len = snprintf(default_line, sizeof(default_line), "hw.ps5.kit_type=%d",
                 kit_type);
  if (len <= 0 ||
      append_kenv(env, env_size, default_line, (size_t)len) != 0)
    return -1;

  const char loader_name[] = "hw.ps5.loader=ps5-freebsd-loader";
  if (append_kenv(env, env_size, loader_name, sizeof(loader_name) - 1) != 0)
    return -1;

  char kenv_path[256];
  long kenv_file_size = find_and_get_size_of_file("kenv.txt", kenv_path);
  if (kenv_file_size <= 0)
    return 0;

  char *kenv_file = malloc((size_t)kenv_file_size + 1);
  if (kenv_file == NULL)
    return -1;
  int read_size = read_file(kenv_path, kenv_file, (size_t)kenv_file_size);
  if (read_size < 0) {
    free(kenv_file);
    return -1;
  }
  kenv_file[read_size] = '\0';

  char *line = kenv_file;
  for (int i = 0; i <= read_size; i++) {
    if (kenv_file[i] != '\n' && kenv_file[i] != '\0')
      continue;
    if (append_kenv(env, env_size, line, (size_t)(&kenv_file[i] - line)) !=
        0) {
      free(kenv_file);
      return -1;
    }
    line = &kenv_file[i + 1];
  }

  free(kenv_file);
  return 0;
}

int fetch_freebsd(struct freebsd_info *info) {
  char kernel_path[256];

  long kernel_file_size = find_and_get_size_of_file("kernel", kernel_path);
  if (kernel_file_size < 0) {
    notify("File kernel not found at default FreeBSD paths - Aborting\n");
    return -1;
  }

  void *kernel =
      mmap(NULL, ALIGN_UP((size_t)kernel_file_size, PAGE_SIZE),
           PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (kernel == MAP_FAILED) {
    notify("[-] Error could not allocate FreeBSD kernel buffer.\n");
    return -1;
  }

  int kernel_size =
      read_file(kernel_path, kernel, ALIGN_UP((size_t)kernel_file_size,
                                             PAGE_SIZE));
  if (kernel_size < 0) {
    notify("Something went wrong while reading FreeBSD kernel - Aborting\n");
    return -1;
  }

  memset(info, 0, sizeof(*info));
  if (validate_freebsd_kernel(kernel, (size_t)kernel_size, info) != 0)
    return -1;

  size_t vram_size;
  char buf_vram[16] = {};
  int ret = find_and_read_file("vram.txt", buf_vram, sizeof(buf_vram) - 1);
  if (ret < 0) {
    printf(
        "File vram.txt not found at default paths - Using static fallback\n");
    vram_size = VRAM_SIZE;
  } else {
    trim_newline(buf_vram);
    vram_size = strtoull(buf_vram, NULL, 16);
    if (vram_size == 0) {
      printf("Seems like the configured vram value is wrong - Using static "
             "fallback\n");
      vram_size = VRAM_SIZE;
    }
  }

  int kit_type = (int)get_kit_type();

  char *env =
      mmap(NULL, ALIGN_UP(FREEBSD_ENV_MAX, PAGE_SIZE), PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (env == MAP_FAILED) {
    notify("[-] Error could not allocate FreeBSD environment buffer.\n");
    return -1;
  }

  size_t env_size;
  if (build_kenv(env, &env_size, vram_size, kit_type) != 0) {
    notify("Something went wrong while building FreeBSD kenv - Aborting\n");
    return -1;
  }

  info->freebsd_info = kernel_cave_freebsd_info;
  info->kernel = kernel_cave_freebsd_kernel;
  info->kernel_size = (size_t)kernel_size;
  info->env = kernel_cave_freebsd_kernel + ALIGN_UP((size_t)kernel_size,
                                                    PAGE_SIZE);
  info->env_size = env_size;
  info->modulep = ALIGN_UP(info->last_pa, FREEBSD_X86_PAGE_SIZE);
  info->kernend_pa = ALIGN_UP(info->modulep + FREEBSD_METADATA_MAX +
                                  info->env_size + 1,
                              FREEBSD_X86_PAGE_SIZE);
  if (info->modulep > UINT32_MAX || info->kernend_pa > UINT32_MAX) {
    notify("FreeBSD kernel plus metadata does not fit below 4GB - Aborting\n");
    return -1;
  }
  info->kernend = info->kernend_pa - info->first_pa;
  info->vram_size = vram_size;
  info->kit_type = kit_type;

  uint64_t page = alloc_page();
  kwrite(pa_to_dmap(page), info, sizeof(struct freebsd_info));
  install_page_syscore(kernel_cave_freebsd_info, page, 0);

  for (size_t i = 0; i < (size_t)kernel_size; i += PAGE_SIZE) {
    install_page_syscore(info->kernel + i,
                         vtophys_user((uintptr_t)kernel + i), 0);
  }

  for (size_t i = 0; i < env_size + 1; i += PAGE_SIZE) {
    install_page_syscore(info->env + i, vtophys_user((uintptr_t)env + i), 0);
  }

  notify("FreeBSD kernel staged. entry=%lx modulep=%lx kernend=%lx\n",
         info->entry, info->modulep, info->kernend);
  return 0;
}
