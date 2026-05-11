#ifndef FIRMWARE_H
#define FIRMWARE_H

#include <stddef.h>

int resolve_device_firmwares(void **initrd, size_t *initrd_size);

#endif
