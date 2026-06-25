#!/usr/bin/env python3
"""Validate a FreeBSD amd64 kernel for ps5-freebsd-loader handoff.

This is a host-side pre-hardware check. It mirrors the loader's ELF, kenv,
and metadata sizing rules so malformed kernels or oversized loader metadata
are caught before a PS5 boot attempt.
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import astuple, dataclass
from pathlib import Path


KERNBASE = 0xFFFFFFFF80000000
KERNLOAD = 0x200000
KERNSTART = KERNBASE + KERNLOAD
X86_PAGE_SIZE = 0x1000
METADATA_MAX = 64 * 1024
ENV_MAX = 16 * 1024

ELF_NIDENT = 16
EI_CLASS = 4
EI_DATA = 5
ELFCLASS64 = 2
ELFDATA2LSB = 1
ET_EXEC = 2
EM_X86_64 = 62
PT_LOAD = 1

MODINFO_END = 0x0000
MODINFO_NAME = 0x0001
MODINFO_TYPE = 0x0002
MODINFO_ADDR = 0x0003
MODINFO_SIZE = 0x0004
MODINFO_METADATA = 0x8000

MODINFOMD_ELFHDR = 0x0002
MODINFOMD_ENVP = 0x0006
MODINFOMD_HOWTO = 0x0007
MODINFOMD_KERNEND = 0x0008
MODINFOMD_SMAP = 0x1001
MODINFOMD_MODULEP = 0x1006

KERNEL_NAME = b"/PS5/FreeBSD/kernel\0"
KERNEL_TYPE = b"elf kernel\0"

EHDR_STRUCT = struct.Struct("<16sHHIQQQIHHHHHH")
PHDR_STRUCT = struct.Struct("<IIQQQQQQ")


@dataclass(frozen=True)
class Ehdr:
    ident: bytes
    e_type: int
    e_machine: int
    e_version: int
    e_entry: int
    e_phoff: int
    e_shoff: int
    e_flags: int
    e_ehsize: int
    e_phentsize: int
    e_phnum: int
    e_shentsize: int
    e_shnum: int
    e_shstrndx: int


@dataclass(frozen=True)
class Phdr:
    p_type: int
    p_flags: int
    p_offset: int
    p_vaddr: int
    p_paddr: int
    p_filesz: int
    p_memsz: int
    p_align: int


@dataclass(frozen=True)
class KernelLayout:
    ehdr: Ehdr
    entry: int
    first_pa: int
    last_pa: int
    modulep: int
    kernend_pa: int
    kernend: int
    load_segments: int


def align_up(value: int, align: int) -> int:
    return (value + align - 1) & ~(align - 1)


def parse_ehdr(data: bytes) -> Ehdr:
    if len(data) < EHDR_STRUCT.size:
        raise ValueError("kernel is too small for an ELF64 header")
    return Ehdr(*EHDR_STRUCT.unpack_from(data, 0))


def parse_phdrs(data: bytes, ehdr: Ehdr) -> list[Phdr]:
    phdr_bytes = ehdr.e_phnum * PHDR_STRUCT.size
    if ehdr.e_phoff > len(data) or phdr_bytes > len(data) - ehdr.e_phoff:
        raise ValueError("program header table is outside the kernel file")
    return [
        Phdr(*PHDR_STRUCT.unpack_from(data, ehdr.e_phoff + i * PHDR_STRUCT.size))
        for i in range(ehdr.e_phnum)
    ]


def phdr_pa(phdr: Phdr) -> int:
    if phdr.p_paddr != 0 and phdr.p_paddr < 0x100000000:
        return phdr.p_paddr
    if phdr.p_vaddr >= KERNBASE:
        return phdr.p_vaddr - KERNBASE
    raise ValueError("PT_LOAD segment has no usable physical address")


def validate_kernel(data: bytes) -> KernelLayout:
    ehdr = parse_ehdr(data)
    if (
        ehdr.ident[:4] != b"\x7fELF"
        or ehdr.ident[EI_CLASS] != ELFCLASS64
        or ehdr.ident[EI_DATA] != ELFDATA2LSB
        or ehdr.e_type != ET_EXEC
        or ehdr.e_machine != EM_X86_64
        or ehdr.e_ehsize != EHDR_STRUCT.size
        or ehdr.e_phentsize != PHDR_STRUCT.size
        or ehdr.e_phnum == 0
    ):
        raise ValueError("kernel is not an amd64 little-endian ELF64 ET_EXEC")

    if ehdr.e_entry < KERNSTART:
        raise ValueError("kernel entry is below FreeBSD KERNSTART")

    first_pa = (1 << 64) - 1
    last_pa = 0
    load_segments = 0
    for phdr in parse_phdrs(data, ehdr):
        if phdr.p_type != PT_LOAD:
            continue
        if phdr.p_memsz < phdr.p_filesz:
            raise ValueError("PT_LOAD p_memsz is smaller than p_filesz")
        if phdr.p_offset > len(data) or phdr.p_filesz > len(data) - phdr.p_offset:
            raise ValueError("PT_LOAD file range is outside the kernel file")
        pa = phdr_pa(phdr)
        if phdr.p_memsz > 0x100000000 - pa:
            raise ValueError("PT_LOAD segment does not fit below 4 GiB")
        first_pa = min(first_pa, pa)
        last_pa = max(last_pa, pa + phdr.p_memsz)
        load_segments += 1

    if load_segments == 0:
        raise ValueError("kernel has no PT_LOAD segments")
    if first_pa != KERNLOAD:
        raise ValueError(f"first PT_LOAD physical address is 0x{first_pa:x}, not 0x{KERNLOAD:x}")

    modulep = align_up(last_pa, X86_PAGE_SIZE)
    return KernelLayout(
        ehdr=ehdr,
        entry=ehdr.e_entry,
        first_pa=first_pa,
        last_pa=last_pa,
        modulep=modulep,
        kernend_pa=0,
        kernend=0,
        load_segments=load_segments,
    )


def append_kenv(env: bytearray, line: str) -> None:
    line = line.strip(" \t\r\n")
    if not line or line.startswith("#") or "=" not in line:
        return
    encoded = line.encode("utf-8")
    if len(env) + len(encoded) + 2 > ENV_MAX:
        raise ValueError("kenv exceeds FREEBSD_ENV_MAX")
    env.extend(encoded)
    env.append(0)


def build_kenv(kenv_file: Path | None, vram_size: int, kit_type: int) -> bytes:
    env = bytearray()
    append_kenv(env, f"hw.ps5.vram_size=0x{vram_size:x}")
    append_kenv(env, f"hw.ps5.kit_type={kit_type}")
    append_kenv(env, "hw.ps5.loader=ps5-freebsd-loader")
    if kenv_file is not None:
        for line in kenv_file.read_text(encoding="utf-8").splitlines():
            append_kenv(env, line)
    env.append(0)
    return bytes(env)


def md_add(blob: bytearray, md_type: int, payload: bytes) -> None:
    blob.extend(struct.pack("<II", md_type, len(payload)))
    blob.extend(payload)
    blob.extend(b"\0" * (align_up(len(payload), 8) - len(payload)))


def build_metadata(layout: KernelLayout, env_size: int, smap_entries: int) -> bytes:
    modulep = layout.modulep
    envp = modulep + METADATA_MAX
    kernend_pa = align_up(modulep + METADATA_MAX + env_size + 1, X86_PAGE_SIZE)
    if modulep > 0xFFFFFFFF or kernend_pa > 0xFFFFFFFF:
        raise ValueError("kernel plus metadata does not fit below 4 GiB")

    blob = bytearray()
    md_add(blob, MODINFO_NAME, KERNEL_NAME)
    md_add(blob, MODINFO_TYPE, KERNEL_TYPE)
    md_add(blob, MODINFO_ADDR, struct.pack("<Q", layout.first_pa))
    md_add(blob, MODINFO_SIZE, struct.pack("<Q", layout.last_pa - layout.first_pa))
    md_add(blob, MODINFO_METADATA | MODINFOMD_ELFHDR, EHDR_STRUCT.pack(*astuple(layout.ehdr)))
    md_add(blob, MODINFO_METADATA | MODINFOMD_HOWTO, struct.pack("<I", 0))
    md_add(blob, MODINFO_METADATA | MODINFOMD_ENVP, struct.pack("<Q", envp))
    md_add(blob, MODINFO_METADATA | MODINFOMD_KERNEND, struct.pack("<Q", kernend_pa - layout.first_pa))
    md_add(blob, MODINFO_METADATA | MODINFOMD_MODULEP, struct.pack("<Q", modulep))
    md_add(blob, MODINFO_METADATA | MODINFOMD_SMAP, b"\0" * (smap_entries * 20))
    blob.extend(struct.pack("<II", MODINFO_END, 0))
    if len(blob) > METADATA_MAX:
        raise ValueError("metadata exceeds FREEBSD_METADATA_MAX")
    return bytes(blob)


def make_test_kernel(**overrides: int | bytes) -> bytes:
    ident = bytearray(b"\x7fELF" + b"\0" * (ELF_NIDENT - 4))
    ident[EI_CLASS] = ELFCLASS64
    ident[EI_DATA] = ELFDATA2LSB
    ident = overrides.pop("ident", bytes(ident))
    ehdr_values = {
        "e_type": ET_EXEC,
        "e_machine": EM_X86_64,
        "e_version": 1,
        "e_entry": KERNSTART + 0x9000,
        "e_phoff": EHDR_STRUCT.size,
        "e_shoff": 0,
        "e_flags": 0,
        "e_ehsize": EHDR_STRUCT.size,
        "e_phentsize": PHDR_STRUCT.size,
        "e_phnum": 1,
        "e_shentsize": 0,
        "e_shnum": 0,
        "e_shstrndx": 0,
    }
    phdr_values = {
        "p_type": PT_LOAD,
        "p_flags": 5,
        "p_offset": 0x1000,
        "p_vaddr": KERNSTART,
        "p_paddr": KERNLOAD,
        "p_filesz": 16,
        "p_memsz": 32,
        "p_align": X86_PAGE_SIZE,
    }
    for key, value in list(overrides.items()):
        if key in ehdr_values:
            ehdr_values[key] = value
            overrides.pop(key)
        elif key in phdr_values:
            phdr_values[key] = value
            overrides.pop(key)
    if overrides:
        raise ValueError(f"unknown test kernel override(s): {', '.join(overrides)}")

    ehdr = EHDR_STRUCT.pack(bytes(ident), *ehdr_values.values())
    phdr = PHDR_STRUCT.pack(*phdr_values.values())
    data = bytearray(max(0x1010, EHDR_STRUCT.size + PHDR_STRUCT.size))
    data[: len(ehdr)] = ehdr
    data[EHDR_STRUCT.size : EHDR_STRUCT.size + len(phdr)] = phdr
    data[0x1000 : 0x1010] = b"KERNEL_TEST_DATA"
    return bytes(data)


def expect_fail(name: str, kernel: bytes) -> None:
    try:
        validate_kernel(kernel)
    except ValueError:
        return
    raise AssertionError(f"{name} unexpectedly passed")


def run_self_tests() -> int:
    valid = make_test_kernel()
    layout = validate_kernel(valid)
    env = build_kenv(None, 0x20000000, 0)
    metadata = build_metadata(layout, len(env), 2)
    assert layout.first_pa == KERNLOAD
    assert layout.last_pa == KERNLOAD + 32
    assert env.endswith(b"\0\0")
    assert b"hw.ps5.loader=ps5-freebsd-loader\0" in env
    assert len(metadata) <= METADATA_MAX

    bad_ident = bytearray(b"BAD!" + b"\0" * (ELF_NIDENT - 4))
    bad_ident[EI_CLASS] = ELFCLASS64
    bad_ident[EI_DATA] = ELFDATA2LSB
    expect_fail("bad magic", make_test_kernel(ident=bytes(bad_ident)))

    bad_class = bytearray(b"\x7fELF" + b"\0" * (ELF_NIDENT - 4))
    bad_class[EI_CLASS] = 1
    bad_class[EI_DATA] = ELFDATA2LSB
    expect_fail("ELF32", make_test_kernel(ident=bytes(bad_class)))

    bad_endian = bytearray(b"\x7fELF" + b"\0" * (ELF_NIDENT - 4))
    bad_endian[EI_CLASS] = ELFCLASS64
    bad_endian[EI_DATA] = 2
    expect_fail("big endian", make_test_kernel(ident=bytes(bad_endian)))

    expect_fail("non ET_EXEC", make_test_kernel(e_type=3))
    expect_fail("non amd64", make_test_kernel(e_machine=183))
    expect_fail("bad phoff", make_test_kernel(e_phoff=0x2000))
    expect_fail("entry below KERNSTART", make_test_kernel(e_entry=KERNBASE))
    expect_fail("PT_LOAD past EOF", make_test_kernel(p_filesz=0x2000))
    expect_fail("p_memsz smaller than p_filesz", make_test_kernel(p_filesz=32, p_memsz=16))
    expect_fail("load above 4GiB", make_test_kernel(p_paddr=0xFFFFFFF0, p_memsz=0x20))
    expect_fail("wrong KERNLOAD", make_test_kernel(p_paddr=0x300000))

    try:
        build_metadata(layout, len(env), 4096)
    except ValueError:
        pass
    else:
        raise AssertionError("metadata overflow unexpectedly passed")

    oversized = bytearray()
    try:
        append_kenv(oversized, "x=" + ("y" * ENV_MAX))
    except ValueError:
        pass
    else:
        raise AssertionError("kenv overflow unexpectedly passed")

    print("FreeBSD handoff self-tests passed")
    return 0


def parse_vram(args: argparse.Namespace) -> int:
    if args.vram_file is not None:
        text = args.vram_file.read_text(encoding="utf-8").strip()
        return int(text, 16)
    return int(args.vram, 0)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("kernel", type=Path, nargs="?")
    parser.add_argument("--kenv", type=Path)
    parser.add_argument("--vram-file", type=Path)
    parser.add_argument("--vram", default="0x20000000")
    parser.add_argument("--kit-type", type=int, default=0)
    parser.add_argument("--smap-entries", type=int, default=64)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_tests()
    if args.kernel is None:
        parser.error("kernel is required unless --self-test is used")

    try:
        kernel = args.kernel.read_bytes()
        layout = validate_kernel(kernel)
        env = build_kenv(args.kenv, parse_vram(args), args.kit_type)
        metadata = build_metadata(layout, len(env), args.smap_entries)
    except Exception as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    kernend_pa = align_up(layout.modulep + METADATA_MAX + len(env) + 1, X86_PAGE_SIZE)
    print("FreeBSD handoff check passed")
    print(f"  kernel:        {args.kernel}")
    print(f"  size:          {len(kernel)} bytes")
    print(f"  entry:         0x{layout.entry:x}")
    print(f"  PT_LOAD:       {layout.load_segments}")
    print(f"  first_pa:      0x{layout.first_pa:x}")
    print(f"  last_pa:       0x{layout.last_pa:x}")
    print(f"  modulep:       0x{layout.modulep:x}")
    print(f"  kernend_pa:    0x{kernend_pa:x}")
    print(f"  kernend:       0x{kernend_pa - layout.first_pa:x}")
    print(f"  kenv_size:     {len(env)} bytes")
    print(f"  metadata_size: {len(metadata)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
