#ifndef CPIO_H
#define CPIO_H

#include <stddef.h>
#include <stdint.h>

#define CPIO_MODE_DIR 0040755
#define CPIO_MODE_FILE 0100644

size_t cpio_newc_entry_size(const char *name, size_t data_size);
size_t cpio_newc_parent_dirs_size(const char *path);
uint8_t *cpio_newc_data(uint8_t *entry, const char *name);
uint8_t *cpio_newc_write_entry(uint8_t *out, const char *name, uint32_t mode,
                               const void *data, size_t data_size,
                               uint32_t ino);
uint8_t *cpio_newc_write_parent_dirs(uint8_t *out, const char *path,
                                     uint32_t *ino);

#endif
