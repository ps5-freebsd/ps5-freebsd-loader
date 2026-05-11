#include "firmware.h"
#include "cpio.h"
#include "utils.h"
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define FIRMWARE_CPIO_INO_BASE 0x500000
#define INITRAMFS_COPY_SCRIPT_MAX_BLOBS 2
#define PS5_WIFI_FW_CPIO_PATH "usr/lib/firmware/nxp/pcieuartiw620_combo_v1.bin"
#define PS5_WIFI_FW_DISPATCHER_PATH "conf/param.conf"
#define PS5_WIFI_FW_COPY_SCRIPT_UBUNTU_PATH "scripts/ubuntu/ps5-wifi-fw"

#define INCBIN(name, path)                                                   \
  __asm__(".section .rodata\n"                                               \
          ".balign 1\n"                                                      \
          ".globl " #name "\n"                                              \
          #name ":\n"                                                       \
          ".incbin \"" path "\"\n"                                         \
          ".globl " #name "_end\n"                                          \
          #name "_end:\n"                                                   \
          ".previous\n");                                                    \
  extern const uint8_t name[];                                                \
  extern const uint8_t name##_end[]

static_assert(sizeof(PS5_WIFI_FW_CPIO_PATH) <= 256,
              "PS5 WiFi firmware cpio path too long");
static_assert(sizeof(PS5_WIFI_FW_DISPATCHER_PATH) <= 256,
              "PS5 WiFi firmware dispatcher path too long");
static_assert(sizeof(PS5_WIFI_FW_COPY_SCRIPT_UBUNTU_PATH) <= 256,
              "PS5 WiFi firmware script path too long");

INCBIN(initramfs_script_ubuntu_initramfs_tools,
       "scripts/ubuntu/initramfs-tools");
INCBIN(initramfs_script_ubuntu_ps5_wifi_fw, "scripts/ubuntu/ps5-wifi-fw");

struct initramfs_blob {
  const char *path;
  const uint8_t *data;
  size_t size;
};

struct initramfs_copy_script {
  const char *distro;
  struct initramfs_blob blobs[INITRAMFS_COPY_SCRIPT_MAX_BLOBS];
  size_t count;
};

static size_t incbin_size(const uint8_t *start, const uint8_t *end) {
  return (size_t)(end - start);
}

static struct initramfs_copy_script load_initramfs_copy_script(void) {
  struct initramfs_copy_script script = {
      .distro = "ubuntu",
      .count = 2,
  };

  script.blobs[0] = (struct initramfs_blob){
      .path = PS5_WIFI_FW_COPY_SCRIPT_UBUNTU_PATH,
      .data = initramfs_script_ubuntu_ps5_wifi_fw,
      .size = incbin_size(initramfs_script_ubuntu_ps5_wifi_fw,
                          initramfs_script_ubuntu_ps5_wifi_fw_end),
  };
  script.blobs[1] = (struct initramfs_blob){
      .path = PS5_WIFI_FW_DISPATCHER_PATH,
      .data = initramfs_script_ubuntu_initramfs_tools,
      .size = incbin_size(initramfs_script_ubuntu_initramfs_tools,
                          initramfs_script_ubuntu_initramfs_tools_end),
  };

  return script;
}

static size_t initramfs_blob_size(const struct initramfs_blob *blob) {
  return cpio_newc_parent_dirs_size(blob->path) +
         cpio_newc_entry_size(blob->path, blob->size);
}

static uint8_t *write_initramfs_blob(uint8_t *out,
                                     const struct initramfs_blob *blob,
                                     uint32_t *ino) {
  out = cpio_newc_write_parent_dirs(out, blob->path, ino);
  return cpio_newc_write_entry(out, blob->path, CPIO_MODE_FILE, blob->data,
                               blob->size, (*ino)++);
}

static size_t ps5_wifi_firmware_size(void) {
  return (size_t)env_offset.PS5_WIFI_FW_SIZE;
}

static uint64_t ps5_wifi_firmware_va(void) {
  return get_offset_va(env_offset.PS5_WIFI_FW_OFFSET);
}

static size_t
ps5_wifi_firmware_overlay_size(const struct initramfs_copy_script *script) {
  size_t total = cpio_newc_parent_dirs_size(PS5_WIFI_FW_CPIO_PATH) +
                 cpio_newc_entry_size(PS5_WIFI_FW_CPIO_PATH,
                                      ps5_wifi_firmware_size());

  for (size_t i = 0; i < script->count; i++)
    total += initramfs_blob_size(&script->blobs[i]);

  return total + cpio_newc_entry_size("TRAILER!!!", 0);
}

static void read_kernel_blob(uint64_t va, uint8_t *out, size_t size) {
  for (size_t copied = 0; copied < size; copied += 0x4000) {
    size_t chunk = size - copied;
    if (chunk > 0x4000)
      chunk = 0x4000;
    kread(va + copied, out + copied, chunk);
  }
}

static size_t
write_ps5_wifi_firmware_overlay(void *overlay,
                                const struct initramfs_copy_script *script) {
  uint8_t *out = (uint8_t *)overlay;
  uint8_t *payload;
  uint32_t ino = FIRMWARE_CPIO_INO_BASE;
  size_t fw_size = ps5_wifi_firmware_size();

  out = cpio_newc_write_parent_dirs(out, PS5_WIFI_FW_CPIO_PATH, &ino);
  payload = cpio_newc_data(out, PS5_WIFI_FW_CPIO_PATH);
  read_kernel_blob(ps5_wifi_firmware_va(), payload, fw_size);
  out = cpio_newc_write_entry(out, PS5_WIFI_FW_CPIO_PATH, CPIO_MODE_FILE,
                              payload, fw_size, ino++);

  for (size_t i = 0; i < script->count; i++)
    out = write_initramfs_blob(out, &script->blobs[i], &ino);

  out = cpio_newc_write_entry(out, "TRAILER!!!", CPIO_MODE_FILE, NULL, 0,
                              ino++);

  return (size_t)(out - (uint8_t *)overlay);
}

static int resolve_ps5_wifi_firmware(void **initrd, size_t *initrd_size) {
  size_t old_initrd_size = *initrd_size;
  struct initramfs_copy_script script = load_initramfs_copy_script();
  size_t overlay_size;
  void *extended_initrd;

  if (env_offset.PS5_WIFI_FW_OFFSET == 0 || env_offset.PS5_WIFI_FW_SIZE == 0) {
    notify("PS5 WiFi firmware offset missing for firmware %04x\n", fw);
    return -1;
  }

  overlay_size = ps5_wifi_firmware_overlay_size(&script);
  extended_initrd =
      mmap(NULL, ALIGN_UP(old_initrd_size + overlay_size, 0x1000),
           PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (extended_initrd == MAP_FAILED) {
    notify("[-] Error could not allocate extended initrd.\n");
    return -1;
  }

  overlay_size = write_ps5_wifi_firmware_overlay(extended_initrd, &script);
  memcpy((uint8_t *)extended_initrd + overlay_size, *initrd, old_initrd_size);
  munmap(*initrd, ALIGN_UP(old_initrd_size, 0x1000));

  *initrd = extended_initrd;
  *initrd_size = overlay_size + old_initrd_size;

  notify("Loaded PS5 WiFi firmware into initrd as /%s (%llu bytes, %s)\n",
         PS5_WIFI_FW_CPIO_PATH,
         (unsigned long long)ps5_wifi_firmware_size(),
         script.distro);
  return 0;
}

//
// Wrapper to resolve future firmwares images using the kernel, at the moment only Marvell WiFi driver
//
int resolve_device_firmwares(void **initrd, size_t *initrd_size) {
  return resolve_ps5_wifi_firmware(initrd, initrd_size);
}
