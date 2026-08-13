#!/usr/bin/env python3
"""
Validate fat32.c against real FAT32 volumes.

Formats disk images with mkfs.vfat across several cluster sizes and layouts,
runs the C writer against them, then checks the result three ways:

  1. fsck.fat -n  - the reference implementation's opinion of the volume
  2. an independent FAT32 parser written here, which walks the directory and
     cluster chain and extracts the file
  3. byte-for-byte comparison of the extracted contents against the pattern
     the writer was asked to produce

Point 2 matters: fsck checks structural consistency but will happily pass a
volume where the file's cluster chain is intact and its contents are garbage.

    python tools/test_fat32.py

Needs mkfs.vfat and fsck.fat (dosfstools) and a host C compiler.
"""

from __future__ import annotations

import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HUB = ROOT / "firmware" / "nucleo_hub"

SECTOR = 512

failures: list[str] = []
checks = 0


def check(label: str, got, want) -> None:
    global checks
    checks += 1
    if got != want:
        failures.append(f"{label}\n     got  {got!r}\n     want {want!r}")


def pattern(file_idx: int, off: int) -> int:
    """Mirror of pattern() in test_fat32_harness.c."""
    return (off * 31 + file_idx * 101 + (off >> 8) * 7) & 0xFF


# --------------------------------------------------------------------
# Independent FAT32 reader
# --------------------------------------------------------------------


class Fat32Reader:
    def __init__(self, path: Path) -> None:
        self.data = path.read_bytes()
        self.part = 0

        if not self._is_bpb(0):
            # MBR
            for i in range(4):
                e = self.data[446 + i * 16: 446 + (i + 1) * 16]
                if e[4] in (0x0B, 0x0C, 0x07):
                    lba = struct.unpack("<I", e[8:12])[0]
                    if lba:
                        self.part = lba
                        break
            else:
                raise ValueError("no FAT32 partition found")

        b = self.sector(self.part)
        self.bytes_per_sec = struct.unpack("<H", b[11:13])[0]
        self.sec_per_clus = b[13]
        reserved = struct.unpack("<H", b[14:16])[0]
        self.num_fats = b[16]
        self.fat_size = struct.unpack("<I", b[36:40])[0]
        self.root_clus = struct.unpack("<I", b[44:48])[0]

        self.fat_lba = self.part + reserved
        self.data_lba = self.fat_lba + self.num_fats * self.fat_size

    def _is_bpb(self, lba: int) -> bool:
        s = self.sector(lba)
        return (
            struct.unpack("<H", s[510:512])[0] == 0xAA55
            and struct.unpack("<H", s[11:13])[0] == SECTOR
            and struct.unpack("<H", s[17:19])[0] == 0
            and struct.unpack("<H", s[22:24])[0] == 0
            and s[13] != 0
        )

    def sector(self, lba: int) -> bytes:
        return self.data[lba * SECTOR: (lba + 1) * SECTOR]

    def fat_entry(self, clus: int, which: int = 0) -> int:
        off = clus * 4
        lba = self.fat_lba + which * self.fat_size + off // SECTOR
        s = self.sector(lba)
        return struct.unpack("<I", s[off % SECTOR: off % SECTOR + 4])[0] & 0x0FFFFFFF

    def chain(self, clus: int) -> list[int]:
        out = []
        seen = set()
        while 2 <= clus < 0x0FFFFFF8:
            if clus in seen:
                raise ValueError(f"cluster chain loops at {clus}")
            seen.add(clus)
            out.append(clus)
            clus = self.fat_entry(clus)
        return out

    def cluster_bytes(self, clus: int) -> bytes:
        lba = self.data_lba + (clus - 2) * self.sec_per_clus
        return self.data[lba * SECTOR: (lba + self.sec_per_clus) * SECTOR]

    def root_entries(self) -> list[tuple[str, int, int]]:
        """(name83, first_cluster, size) for real files in the root dir."""
        out = []
        for clus in self.chain(self.root_clus):
            blob = self.cluster_bytes(clus)
            for off in range(0, len(blob), 32):
                e = blob[off: off + 32]
                if not e or e[0] == 0x00:
                    return out
                if e[0] == 0xE5:
                    continue
                attr = e[11]
                if attr & 0x0F == 0x0F or attr & 0x08:
                    continue
                name = e[:11].decode("ascii", "replace")
                first = (struct.unpack("<H", e[20:22])[0] << 16) | struct.unpack("<H", e[26:28])[0]
                size = struct.unpack("<I", e[28:32])[0]
                out.append((name, first, size))
        return out

    def read_file(self, first: int, size: int) -> bytes:
        blob = b""
        for clus in self.chain(first):
            blob += self.cluster_bytes(clus)
            if len(blob) >= size:
                break
        return blob[:size]

    def fats_agree(self) -> bool:
        if self.num_fats < 2:
            return True
        a = self.data[self.fat_lba * SECTOR: (self.fat_lba + self.fat_size) * SECTOR]
        b = self.data[(self.fat_lba + self.fat_size) * SECTOR:
                      (self.fat_lba + 2 * self.fat_size) * SECTOR]
        return a == b


# --------------------------------------------------------------------
# Build + run
# --------------------------------------------------------------------


def build(workdir: Path) -> Path:
    cc = next((c for c in ("gcc", "cc", "clang") if shutil.which(c)), None)
    if cc is None:
        raise RuntimeError("no host C compiler found")

    exe = workdir / "fat32_harness"
    cmd = [
        cc, "-std=c99", "-O1", "-Wall", "-Wextra", "-Werror",
        f"-I{HUB}",
        str(ROOT / "tools" / "test_fat32_harness.c"),
        str(HUB / "fat32.c"),
        "-o", str(exe),
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"compile failed:\n{r.stdout}\n{r.stderr}")
    return exe


def make_image(path: Path, mb: int, spc: int, partitioned: bool) -> None:
    size = mb * 1024 * 1024
    with path.open("wb") as fh:
        fh.truncate(size)

    if partitioned:
        # Single FAT32 LBA partition starting at sector 2048, the usual layout.
        start = 2048
        sectors = size // SECTOR - start
        mbr = bytearray(512)
        e = mbr[446:462]
        e = bytearray(16)
        e[0] = 0x00              # not bootable
        e[4] = 0x0C              # FAT32 LBA
        e[8:12] = struct.pack("<I", start)
        e[12:16] = struct.pack("<I", sectors)
        mbr[446:462] = e
        mbr[510:512] = b"\x55\xAA"
        with path.open("r+b") as fh:
            fh.write(bytes(mbr))

        sub = subprocess.run(
            ["mkfs.vfat", "-F", "32", "-s", str(spc), "-S", "512",
             "--offset", str(start), str(path)],
            capture_output=True, text=True,
        )
    else:
        sub = subprocess.run(
            ["mkfs.vfat", "-F", "32", "-s", str(spc), "-S", "512", str(path)],
            capture_output=True, text=True,
        )

    if sub.returncode != 0:
        raise RuntimeError(f"mkfs.vfat failed: {sub.stderr or sub.stdout}")


def fsck(path: Path, offset_sectors: int) -> tuple[int, str]:
    cmd = ["fsck.fat", "-n", "-v"]
    if offset_sectors:
        # fsck.fat cannot take an offset; carve the partition out to a temp file.
        sub = Path(str(path) + ".part")
        data = path.read_bytes()[offset_sectors * SECTOR:]
        sub.write_bytes(data)
        target = sub
    else:
        target = path

    r = subprocess.run(cmd + [str(target)], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


# --------------------------------------------------------------------


def run_case(exe: Path, tmp: Path, label: str, mb: int, spc: int,
             partitioned: bool, n_files: int, nbytes: int,
             chunk: int, sync_every: int) -> None:
    img = tmp / f"{label}.img"
    make_image(img, mb, spc, partitioned)

    r = subprocess.run(
        [str(exe), str(img), str(n_files), str(nbytes), str(chunk), str(sync_every)],
        capture_output=True, text=True,
    )

    if r.returncode != 0:
        failures.append(f"[{label}] harness failed:\n{r.stdout}\n{r.stderr}")
        return

    lines = r.stdout.strip().splitlines()
    created = [l.split()[1] for l in lines if l.startswith("CREATE ")]
    check(f"[{label}] files created", len(created), n_files)
    check(f"[{label}] first name", created[0] if created else None, "LOG0001.TLM")
    check(f"[{label}] remount sees LOG0001",
          next((l for l in lines if l.startswith("EXISTS_LOG0001")), ""), "EXISTS_LOG0001 1")
    check(f"[{label}] remount rejects LOG9999",
          next((l for l in lines if l.startswith("EXISTS_LOG9999")), ""), "EXISTS_LOG9999 0")

    # --- reference check ---
    rc, out = fsck(img, 2048 if partitioned else 0)
    if rc != 0:
        failures.append(f"[{label}] fsck.fat reported errors (rc={rc}):\n{out.strip()[:900]}")
    else:
        global checks
        checks += 1

    # --- independent parse ---
    reader = Fat32Reader(img)
    check(f"[{label}] FAT copies identical", reader.fats_agree(), True)

    entries = [e for e in reader.root_entries() if e[0].startswith("LOG")]
    check(f"[{label}] dir entries", len(entries), n_files)

    for idx, (name, first, size) in enumerate(entries):
        check(f"[{label}] {name} size", size, nbytes)
        blob = reader.read_file(first, size)
        check(f"[{label}] {name} length", len(blob), nbytes)

        expect = bytes(pattern(idx, o) for o in range(nbytes))
        if blob != expect:
            bad = next(i for i in range(len(blob)) if blob[i] != expect[i])
            failures.append(
                f"[{label}] {name} contents differ at byte {bad} "
                f"(got 0x{blob[bad]:02X}, want 0x{expect[bad]:02X})"
            )
        else:
            checks += 1


def main() -> int:
    print("Validating fat32.c against real FAT32 volumes\n")

    for tool in ("mkfs.vfat", "fsck.fat"):
        if not shutil.which(tool):
            print(f"  SKIP: {tool} not available (install dosfstools)")
            return 0

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        exe = build(tmp)

        cases = [
            # label,          MB,  spc, part,  files, bytes,  chunk, sync
            ("spc1_super",     64,   1, False,     3,  20000,    52,   20),
            ("spc8_super",     64,   8, False,     2,  70000,    52,   20),
            ("spc8_mbr",      128,   8, True,      2,  70000,    52,   20),
            ("spc64_mbr",     512,  64, True,      1, 300000,   512,    0),
            ("tiny_writes",    64,   4, False,     1,   5000,     1,    7),
            ("no_sync",        64,   4, False,     1,  40000,   103,    0),
            ("many_files",     64,   4, False,    40,    520,    52,    5),
        ]

        for c in cases:
            print(f"  {c[0]:<14} {c[1]:>4} MB  spc={c[2]:<3} "
                  f"{'MBR' if c[3] else 'superfloppy':<12} "
                  f"{c[4]} file(s) x {c[5]} B, chunk {c[6]}")
            try:
                run_case(exe, tmp, *c)
            except Exception as exc:  # noqa: BLE001
                failures.append(f"[{c[0]}] exception: {exc}")

    print(f"\n  {checks - len(failures)}/{checks} checks passed")

    if failures:
        print(f"\n  {len(failures)} FAILURE(S):\n")
        for f in failures:
            print(f"   - {f}\n")
        return 1

    print("\n  fsck.fat is happy and every byte read back matches what was written.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
