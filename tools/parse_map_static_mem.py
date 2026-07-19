#!/usr/bin/env python3
"""Print ESP32-C3 static memory breakdown from build map + ELF (nm).

Usage (from repo or any cwd):
  python tools/parse_map_static_mem.py
  python tools/parse_map_static_mem.py --elf build/ESP32_COMM.elf
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MAP = ROOT / "build" / "ESP32_COMM.map"
DEFAULT_ELF = ROOT / "build" / "ESP32_COMM.elf"

# ESP32-C3 DRAM / IRAM windows used by app (after 16KB i-cache)
DRAM_LO, DRAM_HI = 0x3FC80000, 0x3FCE0000
IRAM_LO, IRAM_HI = 0x40380000, 0x403E0000


def demangle(name: str) -> str:
    """Lightweight Itanium demangle for common ESP app symbols."""
    # _ZL13s_status_json -> s_status_json
    m = re.match(r"^_ZL\d+(.+)$", name)
    if m:
        return m.group(1)
    # _ZZ20can_feedback_handler...E15s_feedback_json
    m = re.search(r"E\d+([A-Za-z_][A-Za-z0-9_]*)$", name)
    if m and name.startswith("_ZZ"):
        return m.group(1) + " (fn-static)"
    # _ZN12_GLOBAL__N_1L6s_workE
    m = re.match(r"^_ZN\d+_GLOBAL__N_\d+L\d+(.+)E$", name)
    if m:
        return m.group(1) + " (anon)"
    # _ZZN12Esp32OtaCore23computeShaFromPartitionEvE14s_sha_read_buf
    m = re.search(r"E\d+([A-Za-z_][A-Za-z0-9_]*)$", name)
    if m and name.startswith("_ZZN"):
        return m.group(1) + " (method-static)"
    # singleton: _ZZN8DebugLog8instanceEvE4inst
    m = re.match(r"^_ZZN(\d+)(.+)$", name)
    if m and "instanceEvE" in name:
        # extract class-ish
        body = name
        cm = re.search(r"_ZZN\d+([A-Za-z0-9_]+)\d+instance", body)
        if cm:
            return cm.group(1) + "::instance"
        return "singleton inst"
    return name


def find_nm() -> str | None:
    which = shutil.which("riscv32-esp-elf-nm")
    if which:
        return which
    roots = [
        Path(r"C:\Espressif\tools\riscv32-esp-elf"),
        Path(r"F:\software\.espressif\tools\riscv32-esp-elf"),
    ]
    for root in roots:
        if not root.exists():
            continue
        matches = sorted(root.glob("**/riscv32-esp-elf-nm.exe"))
        if matches:
            return str(matches[-1])
    return None


def parse_syms(map_text: str) -> dict[str, int]:
    out = {}
    for name in (
        "_iram_start",
        "_iram_end",
        "_data_start",
        "_data_end",
        "_bss_start",
        "_bss_end",
        "_heap_start",
    ):
        m = re.search(rf"0x([0-9a-fA-F]+)\s+{name}\s*=", map_text)
        if m:
            out[name] = int(m.group(1), 16)
    return out


def run_nm(nm: str, elf: Path) -> list[tuple[int, int, str, str]]:
    """Return list of (addr, size, type, name)."""
    # -S size, --size-sort, -t x hex
    r = subprocess.run(
        [nm, "-S", "--size-sort", "-t", "x", str(elf)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    rows = []
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) < 4:
            continue
        try:
            addr = int(parts[0], 16)
            size = int(parts[1], 16)
        except ValueError:
            continue
        typ = parts[2]
        name = parts[3]
        rows.append((addr, size, typ, name))
    return rows


def top_in_range(
    rows: list[tuple[int, int, str, str]],
    lo: int,
    hi: int,
    types: set[str],
    n: int = 20,
    min_size: int = 64,
) -> list[tuple[int, str, str]]:
    picked = []
    for addr, size, typ, name in rows:
        if typ not in types:
            continue
        if size < min_size:
            continue
        if not (lo <= addr < hi):
            continue
        # skip linker bookkeeping extremes
        if name.startswith("_") and name.endswith(("_start", "_end", "_length")):
            if size > 0x10000:
                continue
        picked.append((size, typ, name))
    picked.sort(key=lambda x: -x[0])
    return picked[:n]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--map", type=Path, default=DEFAULT_MAP)
    ap.add_argument("--elf", type=Path, default=DEFAULT_ELF)
    ap.add_argument("-n", type=int, default=18, help="top N symbols per section")
    args = ap.parse_args()

    if not args.map.exists():
        print(f"map not found: {args.map}", file=sys.stderr)
        return 1

    text = args.map.read_text(encoding="utf-8", errors="replace")
    syms = parse_syms(text)

    def span(a: str, b: str) -> int:
        if a not in syms or b not in syms:
            return 0
        return max(0, syms[b] - syms[a])

    iram = span("_iram_start", "_iram_end")
    data = span("_data_start", "_data_end")
    bss = span("_bss_start", "_bss_end")
    heap_start = syms.get("_heap_start", 0)
    static_to_heap = max(0, heap_start - DRAM_LO) if heap_start else iram + data + bss

    print("=== Linker section totals (authoritative) ===")
    print(f"  IRAM  : {iram:7d}  ({iram/1024:6.1f} KB)   code in RAM (WiFi/BT/ISR/tlsf…)")
    print(f"  .data : {data:7d}  ({data/1024:6.1f} KB)   initialized globals/singletons")
    print(f"  .bss  : {bss:7d}  ({bss/1024:6.1f} KB)   zeroed static buffers")
    print(f"  static→heap : {static_to_heap:7d}  ({static_to_heap/1024:6.1f} KB)")
    if heap_start:
        print(f"  _heap_start : 0x{heap_start:08x}")
        room = max(0, DRAM_HI - heap_start)
        print(f"  DRAM window after heap_start (to SOC_DRAM_HIGH): {room} ({room/1024:.1f} KB)")
    print(f"  sum check IRAM+data+bss = {iram+data+bss}  (should ≈ static→heap on C3)")

    nm = find_nm()
    if not args.elf.exists():
        print(f"\nELF not found: {args.elf} (skip symbol tops)")
        return 0
    if not nm:
        print("\nriscv32-esp-elf-nm not found (skip symbol tops)")
        return 0

    print(f"\nusing nm: {nm}")
    rows = run_nm(nm, args.elf)

    sections = [
        ("IRAM code (largest functions)", IRAM_LO, IRAM_HI, set("tT")),
        (".data (largest objects)", DRAM_LO, DRAM_HI, set("dD")),
        (".bss (largest objects)", DRAM_LO, DRAM_HI, set("bB")),
    ]
    for title, lo, hi, types in sections:
        print(f"\n=== Top {args.n} {title} ===")
        tops = top_in_range(rows, lo, hi, types, n=args.n)
        # for .bss prefer heap_start range if known
        if "b" in {t.lower() for t in types} and heap_start:
            bss_lo = syms.get("_bss_start", DRAM_LO)
            bss_hi = heap_start
            tops = top_in_range(rows, bss_lo, bss_hi, types, n=args.n)
        if "d" in {t.lower() for t in types} and "_data_start" in syms:
            tops = top_in_range(
                rows,
                syms["_data_start"],
                syms.get("_data_end", DRAM_HI),
                types,
                n=args.n,
            )
        total = sum(s for s, _, _ in tops)
        for size, typ, name in tops:
            print(
                f"  {size:6d}  {size/1024:5.1f}KB  {typ}  "
                f"{demangle(name):40s}  {name}"
            )
        print(f"  (listed sum {total} / {total/1024:.1f} KB)")

    print(
        "\nTip: runtime /system/status memory.{iram,data,bss} matches linker totals; "
        "symbol tops are build-time only."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
