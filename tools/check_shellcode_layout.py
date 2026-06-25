#!/usr/bin/env python3
"""Validate generated shellcode sizes against the fixed staging layout."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


HEX_DEFINE = re.compile(r"^#define\s+([A-Za-z0-9_]+)\s+(0x[0-9a-fA-F]+)ULL\b")


def read_hex_defines(path: Path) -> dict[str, int]:
    values: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = HEX_DEFINE.match(line.strip())
        if match:
            values[match.group(1)] = int(match.group(2), 16)
    return values


def required_define(values: dict[str, int], name: str) -> int:
    try:
        return values[name]
    except KeyError as exc:
        raise ValueError(f"missing hex define {name}") from exc


def run_check(args: argparse.Namespace) -> int:
    values = read_hex_defines(args.config)
    cave = required_define(values, "cave")
    page_size = required_define(values, "PAGE_SIZE")
    hv_paging_size = required_define(values, "cave_hv_paging_size")
    hv_code_size = required_define(values, "cave_hv_code_size")
    hv_bin_size = args.hv_bin.stat().st_size

    hv_paging = cave
    hv_code = hv_paging + hv_paging_size
    freebsd_files = hv_code + hv_code_size

    if hv_paging_size < 0x3000:
        raise ValueError("cave_hv_paging_size is smaller than the expected 0x3000")
    if hv_code_size < page_size:
        raise ValueError("cave_hv_code_size is smaller than PAGE_SIZE")
    if hv_bin_size > hv_code_size:
        raise ValueError(
            f"HV shellcode is {hv_bin_size} bytes, larger than reserved 0x{hv_code_size:x}"
        )
    if hv_code < hv_paging + 0x3000:
        raise ValueError("HV shellcode overlaps HV paging area")
    if hv_code + hv_bin_size > freebsd_files:
        raise ValueError("HV shellcode overlaps FreeBSD staging area")

    print("Shellcode layout check passed")
    print(f"  hv_bin:        {args.hv_bin}")
    print(f"  hv_bin_size:   {hv_bin_size} bytes")
    print(f"  hv_paging:     0x{hv_paging:x}")
    print(f"  hv_paging_sz:  0x{hv_paging_size:x}")
    print(f"  hv_code:       0x{hv_code:x}")
    print(f"  hv_code_size:  0x{hv_code_size:x}")
    print(f"  freebsd_files: 0x{freebsd_files:x}")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, default=Path("include/config.h"))
    parser.add_argument("--hv-bin", type=Path, default=Path("shellcode_hv/shellcode_hv.bin"))
    args = parser.parse_args(argv)

    try:
        return run_check(args)
    except Exception as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
