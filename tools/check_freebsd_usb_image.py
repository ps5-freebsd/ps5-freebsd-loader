#!/usr/bin/env python3
"""Validate a PS5 FreeBSD USB image before hardware boot.

This intentionally avoids mounting the image. It reads GPT and the FAT32 boot
partition directly, extracts the files the payload will load, and reuses the
FreeBSD handoff checks on the kernel stored in the image.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import uuid
from dataclasses import dataclass
from pathlib import Path

import check_freebsd_handoff as handoff


LBA_SIZE = 512
GPT_HEADER_STRUCT = struct.Struct("<8sIIIIQQQQ16sQIII")
GPT_ENTRY_MIN_SIZE = 128
GPT_ENTRY_STRUCT = struct.Struct("<16s16sQQQ72s")

EFI_SYSTEM_GUID = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
FREEBSD_UFS_GUID = uuid.UUID("516e7cb6-6ecf-11d6-8ff8-00022d09712b")

FAT_EOC = 0x0FFFFFF8
FAT_ATTR_DIRECTORY = 0x10
FAT_ATTR_LONG_NAME = 0x0F


@dataclass(frozen=True)
class GptPartition:
    type_guid: uuid.UUID
    first_lba: int
    last_lba: int
    name: str

    @property
    def offset(self) -> int:
        return self.first_lba * LBA_SIZE

    @property
    def size(self) -> int:
        return (self.last_lba - self.first_lba + 1) * LBA_SIZE


@dataclass(frozen=True)
class FatEntry:
    name: str
    attr: int
    first_cluster: int
    size: int


class ImageReader:
    def __init__(self, path: Path):
        self.path = path
        self.file = path.open("rb")

    def close(self) -> None:
        self.file.close()

    def read_at(self, offset: int, size: int) -> bytes:
        self.file.seek(offset)
        data = self.file.read(size)
        if len(data) != size:
            raise ValueError(f"short read at 0x{offset:x}")
        return data


class Fat32Reader:
    def __init__(self, image: ImageReader, partition: GptPartition):
        self.image = image
        self.partition = partition
        bpb = image.read_at(partition.offset, LBA_SIZE)
        if bpb[510:512] != b"\x55\xaa":
            raise ValueError("FAT boot sector signature is missing")

        self.bytes_per_sector = struct.unpack_from("<H", bpb, 11)[0]
        self.sectors_per_cluster = bpb[13]
        self.reserved_sectors = struct.unpack_from("<H", bpb, 14)[0]
        self.num_fats = bpb[16]
        fat16_sectors = struct.unpack_from("<H", bpb, 22)[0]
        self.fat_sectors = struct.unpack_from("<I", bpb, 36)[0] if fat16_sectors == 0 else fat16_sectors
        self.root_cluster = struct.unpack_from("<I", bpb, 44)[0]

        if self.bytes_per_sector == 0 or self.bytes_per_sector % LBA_SIZE != 0:
            raise ValueError("unsupported FAT sector size")
        if self.sectors_per_cluster == 0:
            raise ValueError("invalid FAT sectors-per-cluster")
        if self.num_fats == 0 or self.fat_sectors == 0:
            raise ValueError("invalid FAT table geometry")
        if self.root_cluster < 2:
            raise ValueError("invalid FAT32 root cluster")

        self.cluster_size = self.bytes_per_sector * self.sectors_per_cluster
        self.fat_offset = partition.offset + self.reserved_sectors * self.bytes_per_sector
        self.data_offset = (
            partition.offset
            + (self.reserved_sectors + self.num_fats * self.fat_sectors) * self.bytes_per_sector
        )
        self.max_clusters = max(1, partition.size // self.cluster_size)

    def cluster_offset(self, cluster: int) -> int:
        if cluster < 2:
            raise ValueError("invalid FAT cluster number")
        return self.data_offset + (cluster - 2) * self.cluster_size

    def next_cluster(self, cluster: int) -> int:
        raw = self.image.read_at(self.fat_offset + cluster * 4, 4)
        return struct.unpack("<I", raw)[0] & 0x0FFFFFFF

    def read_cluster_chain(self, first_cluster: int) -> bytes:
        data = bytearray()
        cluster = first_cluster
        seen: set[int] = set()
        while cluster < FAT_EOC:
            if cluster < 2 or cluster in seen or len(seen) > self.max_clusters:
                raise ValueError("invalid or looping FAT cluster chain")
            seen.add(cluster)
            data.extend(self.image.read_at(self.cluster_offset(cluster), self.cluster_size))
            cluster = self.next_cluster(cluster)
        return bytes(data)

    def list_dir(self, first_cluster: int) -> list[FatEntry]:
        raw = self.read_cluster_chain(first_cluster)
        entries: list[FatEntry] = []
        for pos in range(0, len(raw), 32):
            entry = raw[pos : pos + 32]
            first = entry[0]
            if first == 0x00:
                break
            if first == 0xE5 or entry[11] == FAT_ATTR_LONG_NAME:
                continue

            name = entry[:8].decode("ascii", errors="ignore").rstrip()
            ext = entry[8:11].decode("ascii", errors="ignore").rstrip()
            if not name or name in {".", ".."}:
                continue
            full_name = f"{name}.{ext}" if ext else name
            high = struct.unpack_from("<H", entry, 20)[0]
            low = struct.unpack_from("<H", entry, 26)[0]
            size = struct.unpack_from("<I", entry, 28)[0]
            entries.append(FatEntry(full_name.upper(), entry[11], (high << 16) | low, size))
        return entries

    def find(self, path: str) -> FatEntry:
        parts = [part.upper() for part in path.strip("/").split("/") if part]
        if not parts:
            raise ValueError("empty FAT path")

        cluster = self.root_cluster
        found: FatEntry | None = None
        for idx, part in enumerate(parts):
            entries = self.list_dir(cluster)
            found = next((entry for entry in entries if entry.name == part), None)
            if found is None:
                raise ValueError(f"missing FAT path: /{'/'.join(parts[: idx + 1])}")
            if idx != len(parts) - 1:
                if (found.attr & FAT_ATTR_DIRECTORY) == 0:
                    raise ValueError(f"FAT path component is not a directory: {part}")
                cluster = found.first_cluster
        return found

    def read_file(self, path: str) -> bytes:
        entry = self.find(path)
        if entry.attr & FAT_ATTR_DIRECTORY:
            raise ValueError(f"FAT path is a directory: {path}")
        data = self.read_cluster_chain(entry.first_cluster)
        return data[: entry.size]


def parse_gpt(image: ImageReader) -> list[GptPartition]:
    header = image.read_at(LBA_SIZE, GPT_HEADER_STRUCT.size)
    (
        signature,
        _revision,
        header_size,
        _header_crc32,
        _reserved,
        _current_lba,
        _backup_lba,
        _first_usable_lba,
        _last_usable_lba,
        _disk_guid,
        entries_lba,
        num_entries,
        entry_size,
        _entries_crc32,
    ) = GPT_HEADER_STRUCT.unpack(header)
    if signature != b"EFI PART":
        raise ValueError("GPT header signature is missing")
    if header_size < GPT_HEADER_STRUCT.size:
        raise ValueError("GPT header is too small")
    if entry_size < GPT_ENTRY_MIN_SIZE:
        raise ValueError("GPT partition entry size is too small")

    partitions: list[GptPartition] = []
    entries_offset = entries_lba * LBA_SIZE
    for index in range(num_entries):
        raw = image.read_at(entries_offset + index * entry_size, entry_size)
        type_raw = raw[:16]
        if type_raw == b"\0" * 16:
            continue
        type_guid, _unique_guid, first_lba, last_lba, _attrs, name_raw = GPT_ENTRY_STRUCT.unpack(
            raw[:GPT_ENTRY_STRUCT.size]
        )
        name = name_raw.decode("utf-16le", errors="ignore").rstrip("\x00")
        if first_lba == 0 or last_lba < first_lba:
            raise ValueError(f"invalid GPT partition range for {name or index}")
        partitions.append(
            GptPartition(uuid.UUID(bytes_le=type_guid), first_lba, last_lba, name)
        )
    return partitions


def find_partition(partitions: list[GptPartition], name: str, type_guid: uuid.UUID) -> GptPartition:
    matches = [part for part in partitions if part.name == name and part.type_guid == type_guid]
    if len(matches) != 1:
        raise ValueError(f"expected one GPT partition named {name}")
    return matches[0]


def ensure_partitions_do_not_overlap(partitions: list[GptPartition]) -> None:
    ordered = sorted(partitions, key=lambda part: part.first_lba)
    for left, right in zip(ordered, ordered[1:]):
        if left.last_lba >= right.first_lba:
            raise ValueError(f"GPT partitions overlap: {left.name} and {right.name}")


def build_kenv_from_text(kenv_text: str, vram_size: int, kit_type: int) -> bytes:
    env = bytearray()
    handoff.append_kenv(env, f"hw.ps5.vram_size=0x{vram_size:x}")
    handoff.append_kenv(env, f"hw.ps5.kit_type={kit_type}")
    handoff.append_kenv(env, "hw.ps5.loader=ps5-freebsd-loader")
    for line in kenv_text.splitlines():
        handoff.append_kenv(env, line)
    env.append(0)
    return bytes(env)


def parse_vram_text(text: str) -> int:
    stripped = text.strip()
    if not stripped:
        raise ValueError("vram.txt is empty")
    return int(stripped, 16)


def run_check(args: argparse.Namespace) -> int:
    image = ImageReader(args.image)
    try:
        partitions = parse_gpt(image)
        ensure_partitions_do_not_overlap(partitions)
        boot = find_partition(partitions, "PS5BOOT", EFI_SYSTEM_GUID)
        root = find_partition(partitions, "ps5root", FREEBSD_UFS_GUID)

        fat = Fat32Reader(image, boot)
        kernel = fat.read_file("/PS5/FREEBSD/KERNEL")
        kenv_text = fat.read_file("/PS5/FREEBSD/KENV.TXT").decode("utf-8")
        vram_text = fat.read_file("/PS5/FREEBSD/VRAM.TXT").decode("ascii")

        layout = handoff.validate_kernel(kernel)
        vram_size = parse_vram_text(vram_text)
        env = build_kenv_from_text(kenv_text, vram_size, args.kit_type)
        metadata = handoff.build_metadata(layout, len(env), args.smap_entries)
    finally:
        image.close()

    image_hash = ""
    if args.sha256:
        sha256 = hashlib.sha256()
        with args.image.open("rb") as image_file:
            for chunk in iter(lambda: image_file.read(1024 * 1024), b""):
                sha256.update(chunk)
        image_hash = sha256.hexdigest()

    print("FreeBSD USB image check passed")
    print(f"  image:         {args.image}")
    if image_hash:
        print(f"  sha256:        {image_hash}")
    print(f"  boot:          {boot.name} lba={boot.first_lba}-{boot.last_lba} size={boot.size}")
    print(f"  root:          {root.name} lba={root.first_lba}-{root.last_lba} size={root.size}")
    print(f"  kernel_size:   {len(kernel)} bytes")
    print(f"  entry:         0x{layout.entry:x}")
    print(f"  PT_LOAD:       {layout.load_segments}")
    print(f"  first_pa:      0x{layout.first_pa:x}")
    print(f"  last_pa:       0x{layout.last_pa:x}")
    print(f"  modulep:       0x{layout.modulep:x}")
    print(f"  vram_size:     0x{vram_size:x}")
    print(f"  kenv_size:     {len(env)} bytes")
    print(f"  metadata_size: {len(metadata)} bytes")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--kit-type", type=int, default=0)
    parser.add_argument("--smap-entries", type=int, default=64)
    parser.add_argument("--sha256", action="store_true")
    args = parser.parse_args(argv)

    try:
        return run_check(args)
    except Exception as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
