#ifndef FIRMWARE_H
#define FIRMWARE_H

<<<<<<< dev-wifi-fw
int dump_device_firmwares(const char *boot_file_path);
=======
#include <stddef.h>

int resolve_device_firmwares(void **initrd, size_t *initrd_size);
>>>>>>> main

#endif
