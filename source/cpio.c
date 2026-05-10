#include "cpio.h"
#include <stdio.h>
#include <string.h>

#define CPIO_NEWC_HEADER_LEN 110

static size_t cpio_newc_align4(size_t n) { return (n + 3) & ~3ULL; }

size_t cpio_newc_entry_size(const char *name, size_t data_size) {
  return cpio_newc_align4(CPIO_NEWC_HEADER_LEN + strlen(name) + 1) +
         cpio_newc_align4(data_size);
}

uint8_t *cpio_newc_data(uint8_t *entry, const char *name) {
  return (uint8_t *)cpio_newc_align4((uintptr_t)entry + CPIO_NEWC_HEADER_LEN +
                                     strlen(name) + 1);
}

uint8_t *cpio_newc_write_entry(uint8_t *out, const char *name, uint32_t mode,
                               const void *data, size_t data_size,
                               uint32_t ino) {
  size_t name_size = strlen(name) + 1;

  out = (uint8_t *)cpio_newc_align4((uintptr_t)out);

  sprintf((char *)out,
          "070701%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X",
          ino, mode, 0, 0, 1, 0, (uint32_t)data_size, 0, 0, 0, 0,
          (uint32_t)name_size, 0);
  out += CPIO_NEWC_HEADER_LEN;

  memcpy(out, name, name_size);
  out += name_size;
  out = (uint8_t *)cpio_newc_align4((uintptr_t)out);

  if (data_size != 0 && data != out) {
    memcpy(out, data, data_size);
  }
  out += data_size;
  out = (uint8_t *)cpio_newc_align4((uintptr_t)out);

  return out;
}

size_t cpio_newc_parent_dirs_size(const char *path) {
  char tmp[256];
  size_t total = 0;

  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = strchr(tmp, '/'); p != NULL; p = strchr(p + 1, '/')) {
    *p = '\0';
    if (tmp[0] != '\0')
      total += cpio_newc_entry_size(tmp, 0);
    *p = '/';
  }

  return total;
}

uint8_t *cpio_newc_write_parent_dirs(uint8_t *out, const char *path,
                                     uint32_t *ino) {
  char tmp[256];

  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = strchr(tmp, '/'); p != NULL; p = strchr(p + 1, '/')) {
    *p = '\0';
    if (tmp[0] != '\0') {
      out = cpio_newc_write_entry(out, tmp, CPIO_MODE_DIR, NULL, 0, (*ino)++);
    }
    *p = '/';
  }

  return out;
}
