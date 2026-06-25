# PS5 FreeBSD Port Progress

Status date: 2026-06-25

## Goal

Boot FreeBSD `stable/15` directly on PS5 Phat firmwares 3.00-6.02 using a
FreeBSD-native amd64 ELF handoff. Linux PS5 patches and behavior are treated
only as hardware-behavior references. FreeBSD base changes must remain
BSD-compatible and should follow FreeBSD amd64 conventions.

The current strategy is direct kernel entry, not `loader.efi` and not
Multiboot2:

- Load `/PS5/FreeBSD/kernel` as an amd64 FreeBSD ELF64 kernel.
- Copy PT_LOAD segments to the physical addresses expected by FreeBSD.
- Build FreeBSD preload metadata in low physical memory.
- Build initial amd64 page tables with identity and `KERNBASE` mappings.
- Enter FreeBSD `btext` with the same stack contract used by the FreeBSD amd64
  loader path.

## Worktrees And Branches

- Loader worktree: `/home/bizkut/ps5-freebsd-loader`
- Loader branch: `main`
- FreeBSD worktree: `/home/bizkut/freebsd-stable15`
- FreeBSD branch: `ps5-freebsd-15`
- Image builder worktree: `/home/bizkut/ps5-freebsd-image`
- Image builder branch: `main`

## Loader Progress

### Files Added Or Replaced

- Added `include/freebsd.h`
  - FreeBSD ELF64 constants.
  - FreeBSD metadata constants.
  - SMAP entry type.
  - `struct freebsd_info`, shared by payload, kernel shellcode, and HV
    shellcode.
- Added `include/freebsd_wake_beacon.h`.
- Added `source/freebsd_wake_beacon.c`.
- Added `shellcode_hv/boot_freebsd.c` and `shellcode_hv/boot_freebsd.h`.
- Added `shellcode_kernel/boot_freebsd.c` and
  `shellcode_kernel/boot_freebsd.h`.
- Removed Linux-specific handoff files:
  - `include/linux.h`
  - `include/linux_wake_beacon.h`
  - `source/linux_wake_beacon.c`
  - `shellcode_hv/boot_linux.c`
  - `shellcode_hv/boot_linux.h`
  - `shellcode_kernel/boot_linux.c`
  - `shellcode_kernel/boot_linux.h`
  - `shellcode_hv/shellcode_hv_args.h`

### USB File Interface

The loader now searches these prefixes:

- `/mnt/usb0/`
- `/mnt/usb1/`
- `/mnt/usb2/`
- `/mnt/usb3/`
- `/mnt/usb0/PS5/FreeBSD/`
- `/mnt/usb1/PS5/FreeBSD/`
- `/mnt/usb2/PS5/FreeBSD/`
- `/mnt/usb3/PS5/FreeBSD/`

Expected files:

- `kernel`
  - Required.
  - Must be a FreeBSD amd64 ELF64 `ET_EXEC` kernel.
- `kenv.txt`
  - Optional.
  - Newline-delimited `name=value`.
  - Lines beginning with `#` are ignored.
  - Leading and trailing whitespace is stripped.
  - Lines without `=` are ignored.
- `vram.txt`
  - Optional.
  - Hexadecimal VRAM size.
  - Falls back to `VRAM_SIZE`, currently `512 MiB`, if missing or invalid.
- `path-override.txt`
  - Optional development override.
  - Format is `name=/absolute/path`.
  - Supported names include `kernel`, `kenv.txt`, and `vram.txt`.

### ELF Validation

`fetch_freebsd()` validates the kernel before staging:

- File must contain a complete `Elf64_Ehdr`.
- `e_ident` must be ELF magic.
- `EI_CLASS` must be `ELFCLASS64`.
- `EI_DATA` must be little-endian.
- `e_type` must be `ET_EXEC`.
- `e_machine` must be `EM_X86_64`.
- `e_ehsize` must match `sizeof(Elf64_Ehdr)`.
- `e_phentsize` must match `sizeof(Elf64_Phdr)`.
- Program header table must be fully inside the file.
- Entry point must be at or above FreeBSD `KERNSTART`.
- Each `PT_LOAD` must satisfy:
  - `p_memsz >= p_filesz`
  - file range inside the kernel file
  - physical target below 4 GB
  - no integer overflow in `pa + p_memsz`
- Physical address selection:
  - use `p_paddr` when non-zero and below 4 GB
  - otherwise use `p_vaddr - FREEBSD_KERNBASE`
- First loaded physical address must equal `FREEBSD_KERNLOAD`, currently
  `0x200000`.
- Kernel plus metadata and environment must fit below 4 GB.

Relevant constants in `include/freebsd.h`:

- `FREEBSD_KERNBASE = 0xffffffff80000000`
- `FREEBSD_KERNLOAD = 0x200000`
- `FREEBSD_KERNSTART = FREEBSD_KERNBASE + FREEBSD_KERNLOAD`
- `FREEBSD_METADATA_MAX = 64 KiB`
- `FREEBSD_ENV_MAX = 16 KiB`
- `FREEBSD_PT_BASE = 0x80000`
- `FREEBSD_PT_SIZE = 0xa000`
- `FREEBSD_TRAMP_STACK = 0x9f000`

### Loader Staging Layout

The payload still uses the original PS5 loader idea of staging data first in
the exploited kernel address space, then copying it into HV-visible low
physical memory during resume.

Prospero/kernel-side staging:

- `kernel_cave = 0xffff800000000000`
- `kernel_cave_shellcode = kernel_cave`
- `kernel_cave_files = kernel_cave_shellcode + 2 * PAGE_SIZE`
- `kernel_cave_freebsd_info = kernel_cave_files`
- `kernel_cave_freebsd_kernel = kernel_cave_freebsd_info + PAGE_SIZE`
- staged env starts after the aligned kernel image

HV-side final staging:

- `cave = 0x100000000`
- `cave_hv_paging = cave`
- `cave_hv_code = cave_hv_paging + 0x3000`
- `cave_freebsd_files = cave_hv_code + 0x2000`
- `cave_freebsd_info = cave_freebsd_files`
- `cave_freebsd_kernel = cave_freebsd_info + PAGE_SIZE`
- final env starts after the aligned kernel image

Note that `PAGE_SIZE` in the loader config is `0x4000`, matching the PS5
payload/kernel mapping granularity used by the existing loader, while FreeBSD
x86 page tables are built using `FREEBSD_X86_PAGE_SIZE = 0x1000`.

### Kernel Environment

The loader builds a double-NUL-terminated kenv block and places it after the
kernel metadata area. It injects:

- `hw.ps5.vram_size=0x...`
- `hw.ps5.kit_type=...`
- `hw.ps5.loader=ps5-freebsd-loader`

Then it appends valid lines from `kenv.txt`.

The metadata record stores `MODINFOMD_ENVP` as a physical pointer. FreeBSD
amd64 `native_parse_preload_data()` later adds `KERNBASE` before calling
`init_static_kenv()`.

### FreeBSD Preload Metadata

HV shellcode builds the FreeBSD loader metadata record stream at
`info.modulep`.

Record format matches `stand/common/modinfo.c`:

- 32-bit type
- 32-bit size
- payload
- payload aligned to 8 bytes for amd64

Current records:

- `MODINFO_NAME`
  - value: `/PS5/FreeBSD/kernel`
- `MODINFO_TYPE`
  - value: `elf kernel`
- `MODINFO_ADDR`
  - 64-bit physical `info.first_pa`
- `MODINFO_SIZE`
  - 64-bit `info.last_pa - info.first_pa`
- `MODINFO_METADATA | MODINFOMD_ELFHDR`
  - copied `Elf64_Ehdr`
- `MODINFO_METADATA | MODINFOMD_HOWTO`
  - 32-bit `howto`, currently `0`
- `MODINFO_METADATA | MODINFOMD_ENVP`
  - 64-bit physical env pointer
- `MODINFO_METADATA | MODINFOMD_KERNEND`
  - 64-bit `info.kernend`
- `MODINFO_METADATA | MODINFOMD_MODULEP`
  - 64-bit physical `info.modulep`
- `MODINFO_METADATA | MODINFOMD_SMAP`
  - packed PS5 SMAP entries
- `MODINFO_END`

Important ABI detail:

- `MODINFO_ADDR` is stored physical.
- FreeBSD relocates `MODINFO_ADDR` by `KERNBASE` in
  `preload_bootstrap_relocate()`.
- `MODINFOMD_ENVP` is not relocated there; amd64 startup adds `KERNBASE`
  explicitly after `MD_FETCH()`.

### FreeBSD Entry ABI

FreeBSD amd64 `locore.S` expects this stack on entry to `btext`:

- `0(%rsp)` = 32-bit dummy return address
- `4(%rsp)` = 32-bit `modulep`
- `8(%rsp)` = 32-bit `kernend`

The HV shellcode enters with:

- `modulep = (uint32_t)info.modulep << 32`
- `kernend = (uint32_t)info.kernend`
- push `kernend`
- push `modulep`
- push kernel entry address
- load FreeBSD CR3
- `retq`

This creates the byte layout FreeBSD expects after `retq` transfers control to
`btext`.

`info.kernend` is intentionally an offset from the kernel physical load base.
FreeBSD `hammer_time()` computes:

- `kernphys = amd64_loadaddr()`
- `physfree += kernphys`

so passing an absolute physical address would be wrong.

### Page Tables

Initial FreeBSD page tables are built at `FREEBSD_PT_BASE = 0x80000`.

Current mappings:

- Identity map physical 0-5 GB with 2 MiB pages.
- Map FreeBSD high kernel range through PML4 slot 511:
  - high mapping covers the `KERNBASE` area needed by `btext`
  - backs the first 2 GB of FreeBSD kernel virtual space with physical
    0-2 GB
- Stack is set to `FREEBSD_TRAMP_STACK = 0x9f000` before handoff.

This was checked against FreeBSD `amd64_loadaddr()`, which reads CR3 and walks
the page table entry for `KERNSTART` to derive the physical load address.

### PS5 Hardware State Preserved

The FreeBSD path preserves the existing loader behavior:

- HV defeat for supported firmware ranges.
- Rest-mode handoff.
- VBIOS copy to `0xC0000`.
- MP3 HDMI/HDCP enable sequence.
- TMR discovery from HV shared memory on firmware 5.xx-6.02.
- IOMMU disable before final handoff.
- VRAM programming through the existing AMDGPU MMIO register sequence.
- Retail/devkit memory-tail differences.
- UDP wake beacon flow, renamed to FreeBSD:
  - payload: `PS5FREEBSD_ARMED v1 token=ps5-freebsd fw=...`
  - port: `9755`
  - repeats: `3`

### SMAP Generation

HV shellcode builds a PS5 memory map with:

- low memory usable ranges
- PS5 reserved firmware/HV/device ranges
- FreeBSD page-table reservation
- VRAM reservation
- retail/devkit high memory tail handling
- TMR ranges subtracted from RAM
- ACPI reclaim/NVS types for known reserved ACPI ranges

The SMAP is passed as `MODINFOMD_SMAP` using the FreeBSD `struct bios_smap`
compatible layout:

- `base`
- `length`
- `type`

The remaining work is to validate the exact range list on hardware and make
the FreeBSD side consume PS5-specific reservations consistently.

## FreeBSD Base Progress

Changes are in `/home/bizkut/freebsd-stable15`.

### Kernel Config

Added `sys/amd64/conf/PS5`.

Current shape:

- `include GENERIC`
- `ident PS5`
- `options X86_PS5`
- `options NO_LEGACY_PCIB`
- debug bring-up options:
  - `BOOTVERBOSE`
  - `KDB`
  - `DDB`
  - `GDB`
  - `ALT_BREAK_TO_DEBUGGER`
  - `PRINTF_BUFR_SIZE=256`
- direct-entry, non-EFI policy:
  - `nooptions EFIRT`
  - `nodevice efidev`
  - `nodevice efirtc`
  - `nodevice smbios`
- disabled legacy PC devices:
  - floppy
  - AT keyboard controller
  - AT keyboard
  - PS/2 mouse
  - VGA
  - VGA/EFI vt backends
  - syscons
  - splash
  - parallel port stack
  - `puc`
- disabled legacy hints:
  - `hint.atkbdc.0.disabled=1`
  - `hint.atrtc.0.disabled=1`
  - `hint.attimer.0.disabled=1`
  - `hint.fd.0.disabled=1`
  - `hint.sc.0.disabled=1`
  - `hint.uart.0.disabled=1`
  - `hint.uart.1.disabled=1`
  - `hw.ps5.platform=1`

The config intentionally derives from `GENERIC` so USB, xHCI, AHCI, NVMe,
PCI, LinuxKPI, Ethernet, firmware, and normal storage support remain built in
for initial bring-up unless later PS5-specific constraints require trimming.

### Build System Registration

Changed `sys/conf/options.amd64`:

- added `X86_PS5 opt_x86_ps5.h`

Changed `sys/conf/files.amd64`:

- added `x86/x86/ps5_machdep.c optional x86_ps5`

### Initial Platform Glue

Added `sys/x86/x86/ps5_machdep.c`.

Current behavior:

- Declares `hw.ps5` sysctl node.
- Exposes:
  - `hw.ps5.platform`
  - `hw.ps5.vram_size`
  - `hw.ps5.kit_type`
  - `hw.ps5.loader`
- Reads loader kenv through tunables:
  - `TUNABLE_UINT64_FETCH("hw.ps5.vram_size", ...)`
  - `TUNABLE_INT_FETCH("hw.ps5.kit_type", ...)`
  - `TUNABLE_STR_FETCH("hw.ps5.loader", ...)`
- Installs placeholder SYSINIT hooks:
  - `SI_SUB_TUNABLES` for loader environment capture
  - `SI_SUB_CPU` for later fixed TSC/APIC/CPU policy
  - `SI_SUB_INTRINSIC` for shutdown event registration
  - `SI_SUB_CONFIGURE` for a late verbose status banner
- Installs a `shutdown_final` event handler placeholder for later PS5
  power/reboot/rest-mode service plumbing.

The file is intentionally minimal. It is not the final PS5 platform
implementation.

## Image Builder Progress

Changes are in `/home/bizkut/ps5-freebsd-image`.

### FreeBSD Target

`build_image.sh` now has a `--distro freebsd` path separate from the existing
Linux kernel/distro image pipeline.

New FreeBSD-specific command-line interface:

- `--freebsd-src`
  - FreeBSD source tree used for `buildworld` and `buildkernel`.
  - Defaults to `../freebsd-stable15` when that tree exists.
- `--freebsd-root`
  - FreeBSD installed DESTDIR/root tree.
  - Defaults to `work/freebsd-root`.
- `--freebsd-kernel`
  - Kernel ELF copied to `/PS5/FreeBSD/kernel`.
  - Defaults to `$freebsd_root/boot/kernel/kernel`.
- `--freebsd-build-backend`
  - `auto`, `host`, or `qemu`.
  - `auto` selects `host` on FreeBSD and `qemu` elsewhere.
- `--freebsd-vm-image`
  - QEMU base FreeBSD image for Linux-host builds.
  - Required for the `qemu` backend.
- `--freebsd-vm-user`
  - SSH user for the QEMU FreeBSD VM.
  - Defaults to `freebsd`.
- `--freebsd-vm-ssh-key`
  - Optional SSH private key for the QEMU FreeBSD VM.
- `--skip-freebsd-build`
  - Reuses an existing `--freebsd-root` and only assembles the final image.

FreeBSD images default to `8000 MiB` instead of the Linux target default of
`12000 MiB`.

### FreeBSD Build Backends

Added `scripts/build-freebsd-root.sh` for native FreeBSD hosts.

It runs:

```sh
make -C "$SRC" -j"$JOBS" TARGET=amd64 TARGET_ARCH=amd64 buildworld
make -C "$SRC" -j"$JOBS" TARGET=amd64 TARGET_ARCH=amd64 KERNCONF=PS5 buildkernel
make -C "$SRC" TARGET=amd64 TARGET_ARCH=amd64 DESTDIR="$DESTDIR" installworld
make -C "$SRC" TARGET=amd64 TARGET_ARCH=amd64 DESTDIR="$DESTDIR" distribution
make -C "$SRC" TARGET=amd64 TARGET_ARCH=amd64 KERNCONF=PS5 DESTDIR="$DESTDIR" installkernel
```

Added `docker/freebsd-vm-builder/` and
`scripts/build-freebsd-root-qemu.sh` for Linux hosts.

The Docker+QEMU backend:

- Builds an Ubuntu-based helper container with QEMU and OpenSSH client tools.
- Runs a FreeBSD VM inside that container.
- Uses a qcow2 overlay on top of the supplied FreeBSD VM image, leaving the
  base image unchanged.
- Forwards host/container TCP port `10022` to guest SSH port `22` by default.
- Copies the FreeBSD source tree into the guest over SSH/tar.
- Runs native FreeBSD `buildworld`, `buildkernel KERNCONF=PS5`,
  `installworld`, `distribution`, and `installkernel` inside the guest.
- Copies the resulting DESTDIR tarball back to `work/freebsd-root`.
- Requires the selected FreeBSD VM user to have passwordless sudo for the make
  commands.

This is the preferred Linux-host path because normal Linux Docker containers
share the Linux host kernel. FreeBSD Docker Hub OCI images can be useful later
as native FreeBSD container roots or rootfs seeds, but they do not replace the
FreeBSD VM requirement for Linux-host `buildworld`/`buildkernel` execution.

### FreeBSD USB Image Layout

Added `docker/freebsd-image-builder/`.

The image assembler runs on Linux and creates `output/ps5-freebsd.img` with:

- GPT partition 1:
  - starts at `1 MiB`
  - size `499 MiB`
  - type `ef00`
  - label `PS5BOOT`
  - FAT32 filesystem
  - contains:
    - `/PS5/FreeBSD/kernel`
    - `/PS5/FreeBSD/kenv.txt`
    - `/PS5/FreeBSD/vram.txt`
- GPT partition 2:
  - starts at `500 MiB`
  - consumes the rest of the image
  - type `a503`
  - label `ps5root`
  - UFS/FFSv2 filesystem built with Linux `makefs`
  - contains the FreeBSD root tree

The root image receives:

```text
/etc/fstab: /dev/gpt/ps5root / ufs rw 1 1
```

If `/etc/rc.conf` is absent in the staged root, the image builder adds a small
default with hostname `ps5-freebsd`, DHCP on default interfaces, `sshd`
disabled, and `dumpdev=NO`.

Added default loader-side files:

- `boot/freebsd/kenv.txt`
  - sets `vfs.root.mountfrom=ufs:/dev/gpt/ps5root`
  - enables `boot_verbose=YES`
  - sets `hw.ps5.platform=1`
- `boot/freebsd/vram.txt`
  - defaults to `20000000`

### Image Builder Documentation

Updated `/home/bizkut/ps5-freebsd-image/README.md` with:

- FreeBSD target overview.
- FreeBSD CLI flags.
- prebuilt DESTDIR build example.
- native FreeBSD host build example.
- Docker+QEMU FreeBSD VM build example.
- FreeBSD image partition table.
- Docker Hub FreeBSD OCI image caveat.
- new directory layout entries.

## Verification Completed

### Loader

Passed:

```sh
make -B -C shellcode_hv shellcode_hv.h
make -B -C shellcode_kernel shellcode_kernel.h
make PS5_PAYLOAD_SDK=/tmp/ps5-payload-sdk
make check-freebsd-handoff-selftest
make check-freebsd-handoff \
  FREEBSD_KERNEL=/home/bizkut/ps5-freebsd-image/work/freebsd-root/boot/kernel/kernel \
  FREEBSD_KENV=/home/bizkut/ps5-freebsd-image/boot/freebsd/kenv.txt \
  FREEBSD_VRAM=/home/bizkut/ps5-freebsd-image/boot/freebsd/vram.txt
git diff --check
```

The shellcode compile/link path completed with the local x86-64 GCC toolchain.
Generated headers were regenerated successfully.

The top-level payload now builds successfully after installing LLVM/Clang and
installing the adjacent SDK checkout into a temporary SDK root:

```sh
sudo apt-get install -y clang lld llvm-dev llvm
make -C ../ps5-payload-sdk DESTDIR=/tmp/ps5-payload-sdk install
make PS5_PAYLOAD_SDK=/tmp/ps5-payload-sdk
```

Build output:

```text
bin/ps5-freebsd-loader.elf
```

The resulting payload is a 64-bit x86-64 FreeBSD PIE executable and is not
stripped.

The host-side FreeBSD handoff checker validates the built PS5 kernel before a
hardware boot attempt:

- ELF64 amd64 `ET_EXEC` shape.
- `PT_LOAD` file ranges and below-4GB physical placement.
- first physical load at `0x200000`.
- loader kenv formatting and size.
- preload metadata record packing and size.

Latest checked kernel:

```text
entry:         0xffffffff80389000
PT_LOAD:       5
first_pa:      0x200000
last_pa:       0x2200000
modulep:       0x2200000
kernend_pa:    0x2211000
kernend:       0x2011000
kenv_size:     155 bytes
metadata_size: 1520 bytes
```

### FreeBSD Tree

Passed:

```sh
git -C /home/bizkut/freebsd-stable15 diff --check
```

Checked locally against FreeBSD source:

- metadata constants in `sys/sys/linker.h`
- amd64 metadata constants in `sys/x86/include/metadata.h`
- amd64 entry stack contract in `sys/amd64/amd64/locore.S`
- `native_parse_preload_data()` and `hammer_time()` behavior in
  `sys/amd64/amd64/machdep.c`
- `md_copymodules()` record format in `stand/common/modinfo.c`

FreeBSD `buildworld`, `buildkernel KERNCONF=PS5`, `installworld`,
`distribution`, and `installkernel` now complete inside a native FreeBSD 15.0
QEMU VM driven by the image-builder QEMU backend.

The first kernel build failure was:

```text
ld: error: undefined symbol: active_efi_ops
```

Root cause: `sys/amd64/conf/PS5` disabled `EFIRT`, `efidev`, and `efirtc`, but
still inherited `device smbios` from `GENERIC`. FreeBSD `sys/x86/conf/NOTES`
marks `smbios` as requiring `EFIRT`, so the PS5 kernel config now also has:

```text
nodevice smbios
```

### Image Builder

Passed:

```sh
bash -n /home/bizkut/ps5-freebsd-image/build_image.sh
sh -n /home/bizkut/ps5-freebsd-image/scripts/build-freebsd-root.sh
sh -n /home/bizkut/ps5-freebsd-image/scripts/build-freebsd-root-qemu.sh
bash -n /home/bizkut/ps5-freebsd-image/docker/freebsd-image-builder/entrypoint.sh
git -C /home/bizkut/ps5-freebsd-image diff --check
```

Checked package availability for the Linux image-assembly helper on the local
Ubuntu package index:

- `makefs`
- `gdisk`
- `mtools`
- `qemu-system-x86`

Not yet run:

- PS5 hardware boot test of `output/ps5-freebsd.img`.

Completed:

- `docker build` for the FreeBSD QEMU helper image.
- end-to-end `--distro freebsd --freebsd-build-backend qemu` build through
  native FreeBSD `buildworld`/`buildkernel`/install.
- recovery of the generated FreeBSD DESTDIR to `work/freebsd-root`.
- `docker build` for the FreeBSD image assembler.
- end-to-end `--distro freebsd --skip-freebsd-build` image assembly.

Final artifact:

```text
output/ps5-freebsd.img
size:   7.9G
sha256: 2a6d700ae062fccc74bbcc7af0eeff9cbc76b7b8b6ea9f2321668eb95c0640e5
```

## Known Technical Gaps

- No PS5 hardware boot test has been run.
- The FreeBSD base tree contains only initial platform scaffolding, not full
  PS5 platform support.
- Direct handoff currently assumes the kernel is linked and loadable with the
  standard FreeBSD amd64 `KERNLOAD = 0x200000` contract.
- The loader currently passes one kernel only. Kernel modules, symbols, and
  `/boot` module preload support are not implemented.
- SMAP ranges need hardware validation and may need adjustment before memory
  sizing is trusted.
- FreeBSD does not yet have PS5-specific PCI config routing, page-table NDA
  support, storage quirks, service drivers, network, Bluetooth, or graphics
  support.
- The current image artifact has not yet been booted on PS5 hardware.

## Remaining Technical Plan

### 1. Build Tooling And Baseline Builds

- Keep using the temporary SDK root or install the SDK permanently:

```sh
make PS5_PAYLOAD_SDK=/tmp/ps5-payload-sdk
```

The loader currently builds with that SDK root.

The Linux-host Docker+QEMU baseline is working. Rebuild it with:

```sh
cd /home/bizkut/ps5-freebsd-image
./build_image.sh --distro freebsd \
  --freebsd-build-backend qemu \
  --freebsd-src ../freebsd-stable15 \
  --freebsd-vm-image /path/to/freebsd-build-vm.qcow2 \
  --freebsd-vm-user freebsd \
  --freebsd-vm-ssh-key ~/.ssh/freebsd-build
```

The equivalent native FreeBSD host path remains:

```sh
cd /home/bizkut/freebsd-stable15
make buildkernel KERNCONF=PS5
```

- Keep fixing first-order build errors from unsupported device assumptions
  inherited from `GENERIC` as new PS5-specific constraints are found.

### 2. Loader Test Harness

Added `tools/check_freebsd_handoff.py`, wired through:

```sh
make check-freebsd-handoff-selftest
make check-freebsd-handoff FREEBSD_KERNEL=/path/to/kernel
```

It currently covers:

- ELF reject cases:
  - wrong magic
  - ELF32
  - big-endian
  - non-amd64
  - non-`ET_EXEC`
  - malformed program header table
  - `PT_LOAD` past EOF
  - `p_memsz < p_filesz`
  - load address above 4 GB
  - first physical load not equal to `0x200000`
- kenv formatting:
  - comments ignored
  - invalid lines ignored
  - double-NUL terminator present
  - `FREEBSD_ENV_MAX` overflow fails cleanly
- preload metadata:
  - each record has correct type and size
  - 8-byte alignment is preserved
  - `MODINFO_END` terminator present
  - `MODINFOMD_ENVP`, `KERNEND`, `MODULEP`, and `SMAP` records decode
    correctly

Still needed:

- SMAP generation:
  - TMR ranges are subtracted from RAM
  - VRAM is not reported usable
  - loader page-table area is not reported usable
  - known ACPI ranges use the expected type
- page table generation:
  - CR3 points at `FREEBSD_PT_BASE`
  - `KERNSTART` maps to `FREEBSD_KERNLOAD`
  - low identity ranges cover all physical areas used before
    `pmap_bootstrap()`

### 3. First Hardware Milestone

Target: prove the loader reaches FreeBSD `btext`.

Tasks:

- Add minimal UART-visible breadcrumbs before final `retq`.
- Add early FreeBSD trace in `btext` or immediately after `hammer_time()`
  when practical.
- Confirm:
  - CPU 0 is the only CPU entering FreeBSD
  - non-boot CPUs are halted before handoff
  - CR3 contains the FreeBSD bootstrap page tables
  - `KERNSTART` maps to physical `0x200000`
  - stack bytes match FreeBSD amd64 loader ABI

### 4. `hammer_time()` And Memory Milestone

Target: FreeBSD parses preload metadata and reaches early amd64 startup.

Tasks:

- Confirm `native_parse_preload_data()` finds the kernel metadata record.
- Confirm `boothowto` is sane.
- Confirm `envp` points at the loader-generated kenv.
- Confirm `hw.ps5.*` tunables are visible to `ps5_machdep.c`.
- Confirm `MODINFOMD_SMAP` is found by amd64 memory sizing.
- Confirm `phys_avail[]` excludes:
  - kernel image
  - metadata
  - env block
  - loader page tables
  - HV reserved ranges
  - TMRs
  - VRAM
- Fix any early allocation collision with the loader trampoline page tables.

### 5. Core PS5 Platform Layer

Replace `ps5_machdep.c` placeholders with real platform behavior:

- Disable or bypass legacy PIC/PIT/RTC assumptions.
- Add fixed TSC fallback and calibration policy if PS5 firmware state does not
  provide what FreeBSD expects.
- Add PS5-specific reboot, poweroff, and rest-mode hooks.
- Add early debug console path.
- Add a durable platform data structure populated from loader metadata or kenv.
- Add PS5 memory reservation registration that survives beyond early SMAP
  parsing.

### 6. AMD NDA Page-Table Support

Add FreeBSD amd64 support for PS5-required AMD non-decrypted attributes:

- Identify the exact EFER NDA bit behavior on PS5.
- Add PTE bit definitions for NDA.
- Ensure new bits do not collide with FreeBSD pmap software bits.
- Audit:
  - `sys/amd64/include/pmap.h`
  - `sys/amd64/amd64/pmap.c`
  - direct-map setup
  - device-memory mappings
  - DMA mappings
- Add targeted mappings where PS5 hardware requires NDA.

### 7. PS5 PCI Config-Space Routing

Port the PS5 SFC-style PCI configuration access behavior in FreeBSD-native
form:

- Identify PCI config mechanism used by PS5 firmware.
- Add PS5-specific config read/write routing.
- Avoid legacy PC ECAM/IO-port assumptions when `X86_PS5` is enabled.
- Validate enumeration of:
  - Salina southbridge devices
  - xHCI
  - AHCI
  - NVMe
  - AMDGPU
  - Ethernet
  - Bluetooth USB device path

### 8. USB And Storage Bring-Up

USB:

- Add Salina xHCI PHY/reset sequencing.
- Add xHCI poller quirks if interrupts are not available early.
- Validate USB keyboard and mass-storage enumeration.
- Validate root mount from USB first.

SATA/AHCI:

- Add Salina AHCI PHY/init quirks.
- Validate disk detection and DMA stability.

NVMe/M.2:

- Add PS5 GPT offset handling.
- Validate M.2 namespace detection.
- Validate root mount from M.2 after USB boot works.

### 9. Platform Services

Add PS5 service drivers in FreeBSD style:

- SPCIE/ICC transport.
- TPCIE UART console.
- Power service:
  - poweroff
  - reboot
  - rest mode
- Fan service.
- LED service.
- HDMI/audio service plumbing.

Each service should expose a small, testable kernel interface before being
integrated with generic FreeBSD subsystems.

### 10. Network And Bluetooth

Network:

- Add Ethernet PHY support.
- Add or adapt the Ethernet MAC driver path.
- Validate link up/down, DHCP, static IP, and sustained traffic.

Bluetooth:

- Add required USB quirks.
- Validate enumeration.
- Validate basic controller attach and input pairing path.

### 11. Graphics And HDMI

Keep accelerated graphics outside FreeBSD base:

- Do not port PS5 AMDGPU/display quirks into old base DRM2.
- Implement PS5 quirks in the FreeBSD `drm-kmod`/LinuxKPI path.
- Ensure loader/base exports enough platform information:
  - VRAM size
  - framebuffer/VRAM base assumptions
  - firmware state
  - kit type
  - platform detection
- Validate in stages:
  - HDMI stays lit through boot
  - framebuffer console path
  - drm-kmod attach
  - modeset
  - acceleration

### 12. Integration And Commit Plan

Keep commits staged by bring-up boundary:

1. Loader FreeBSD ELF handoff.
2. FreeBSD `X86_PS5` config and minimal platform glue.
3. Early memory/platform fixes.
4. PCI config routing.
5. USB and storage.
6. Platform services.
7. Network and Bluetooth.
8. drm-kmod AMDGPU/HDMI.
9. Power/fan/rest-mode polish.
10. Documentation and first-boot guide.

Before each stage lands:

- build loader where applicable
- build `KERNCONF=PS5`
- boot-test on target firmware
- document the exact firmware and hardware used
- keep Linux-derived behavior as reference-only and avoid copying GPL code into
  the loader or FreeBSD tree
