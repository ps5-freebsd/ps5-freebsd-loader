#ifndef CONFIG_H
#define CONFIG_H

#define PAGE_SIZE 0x4000ULL

// This is used to allocate resources for HV shellcode and FreeBSD boot
#define cave 0x100000000ULL
#define cave_hv_paging cave
#define cave_hv_code                                                           \
  cave_hv_paging + 0x3000ULL // Leave space for 3 pages but we only use 2 for
                             // 1GB 1:1 mapping
#define cave_freebsd_files cave_hv_code + 0x2000ULL
#define cave_freebsd_info cave_freebsd_files
#define cave_freebsd_kernel cave_freebsd_info + PAGE_SIZE
// #define cave_freebsd_env     // Allocated dynamically after kernel

#define hv_base_rsp (cave + 0x10000000ULL)
#define hv_stack_size 0x1000ULL

// This is used as transitional storage from ProsperoOS to Kernel shellcode
#define kernel_cave 0xFFFF800000000000
#define kernel_cave_shellcode kernel_cave
#define kernel_cave_files kernel_cave_shellcode + PAGE_SIZE + PAGE_SIZE
#define kernel_cave_freebsd_info kernel_cave_files
#define kernel_cave_freebsd_kernel kernel_cave_freebsd_info + PAGE_SIZE

// #define kernel_cave_freebsd_env   // Allocated dynamically after kernel

// FreeBSD boot config
#define VRAM_SIZE (512ULL * 1024 * 1024)
#ifndef ENABLE_FREEBSD_WAKE_BEACON
#define ENABLE_FREEBSD_WAKE_BEACON 1
#endif
#ifndef FREEBSD_WAKE_BEACON_TOKEN
#define FREEBSD_WAKE_BEACON_TOKEN "ps5-freebsd"
#endif
#ifndef FREEBSD_WAKE_BEACON_PORT
#define FREEBSD_WAKE_BEACON_PORT 9755
#endif
#ifndef FREEBSD_WAKE_BEACON_REPEATS
#define FREEBSD_WAKE_BEACON_REPEATS 3
#endif
#ifndef FREEBSD_WAKE_BEACON_INTERVAL_US
#define FREEBSD_WAKE_BEACON_INTERVAL_US 300000
#endif

#define DEBUG 0 // Toggle to 0 to disable logs

#endif
