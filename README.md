# ps5-freebsd-loader

Experimental FreeBSD boot payload for PS5 Phat consoles on supported
3.00-6.02 firmwares.

This branch replaces the Linux `bzImage`/`initrd` handoff with a direct
FreeBSD amd64 ELF handoff. The payload loads a FreeBSD `stable/15` kernel from
USB, prepares PS5 hardware state, builds FreeBSD preload metadata, creates the
initial amd64 page tables expected by `btext`, and enters the kernel at its
linked address.

## USB Layout

The loader searches the root of each USB mount and `/PS5/FreeBSD/`:

- `/PS5/FreeBSD/kernel`: required FreeBSD amd64 kernel ELF.
- `/PS5/FreeBSD/kenv.txt`: optional newline-delimited `name=value` kernel
  environment. Lines starting with `#` are ignored.
- `/PS5/FreeBSD/vram.txt`: optional hexadecimal VRAM size. If missing or
  invalid, the loader uses the built-in 512 MiB fallback.
- `path-override.txt`: optional development override file. Each line is
  `name=/absolute/path`, for example `kernel=/mnt/usb0/test/kernel`.

The loader also injects these kernel environment values:

- `hw.ps5.vram_size`
- `hw.ps5.kit_type`
- `hw.ps5.loader=ps5-freebsd-loader`

## Build

The top-level payload requires `ps5-payload-sdk`:

```sh
make
```

If `PS5_PAYLOAD_SDK` is not `/opt/ps5-payload-sdk`, pass it explicitly:

```sh
make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
```

The shellcode subdirectories can be built with a normal x86-64 GCC toolchain:

```sh
make -C shellcode_hv
make -C shellcode_kernel
```

The shellcode Makefiles use `xxd` when available and fall back to `od`/`awk`
for generated C headers.

## Boot Notes

Supported firmware and rest-mode setup requirements are inherited from the
original PS5 HV-defeat loader:

- PS5 Phat firmwares 3.00, 3.10, 3.20, 3.21, 4.00, 4.02, 4.03, 4.50, 4.51,
  5.00, 5.02, 5.10, 5.50, 6.00, and 6.02.
- Rest Mode USB power must be set to `Always`.
- HDMI Device Link should be disabled.

This is a staged FreeBSD bring-up branch. The loader-side handoff is present,
but a matching FreeBSD kernel still needs PS5 platform support and drivers for
full hardware parity.
