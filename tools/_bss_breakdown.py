#!/usr/bin/env python3
from pathlib import Path
import re
from collections import defaultdict

ROOT = Path(__file__).resolve().parents[1]
text = (ROOT / "build" / "ESP32_COMM.map").read_text(encoding="utf-8", errors="replace")
rows = []
lines = text.splitlines()
i = 0
while i < len(lines):
    line = lines[i]
    m = re.match(r"^\s*\.bss\.(\S+)\s*$", line)
    m2 = re.match(r"^\s*\.bss\.(\S+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)", line)
    if m2:
        rows.append((int(m2.group(3), 16), m2.group(1), ""))
    elif m:
        name = m.group(1)
        j = i + 1
        while j < len(lines) and j < i + 5:
            l2 = lines[j]
            mm = re.match(r"^\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(\S+)", l2)
            if mm:
                rows.append((int(mm.group(2), 16), name, mm.group(3)))
                break
            if re.match(r"^\s*\.", l2) and not l2.strip().startswith(".bss"):
                break
            j += 1
    i += 1


def demangle(n: str) -> str:
    n = n.replace("_ZN12_GLOBAL__N_1L", "")
    n = n.replace("_ZL", "")
    if n.startswith("_ZZ"):
        n = "local:" + n[3:50]
    return n[:55]


cats = defaultdict(int)
app = []
for size, name, obj in rows:
    o = (obj or "").lower()
    if "libmain" in o:
        cat = "app(main)"
        app.append((size, name, obj))
    elif "libfreertos" in o:
        cat = "freertos"
    elif "liblwip" in o:
        cat = "lwip"
    elif any(x in o for x in ("libnet80211", "libpp", "libwpa", "libmesh")):
        cat = "wifi/mesh"
    elif any(x in o for x in ("libnimble", "libbt", "bt.", "ble_")):
        cat = "ble"
    elif any(
        x in name
        for x in ("g_cnx", "gWpa", "g_ic", "g_pm", "gChm", "gScan", "g_mesh", "mesh_")
    ):
        cat = "wifi/mesh(sym)"
    else:
        cat = "other/idf"
    cats[cat] += size

print("=== category ===")
for k, v in sorted(cats.items(), key=lambda x: -x[1]):
    print(f"{v/1024:6.2f} KB  {k}")
print(f"{sum(cats.values())/1024:6.2f} KB  TOTAL")

print("\n=== app(main) ===")
app.sort(reverse=True)
s = 0
for size, name, obj in app:
    s += size
    print(f"{size:5d}  {demangle(name)}")
print(f"app sum {s} ({s/1024:.2f} KB)")

print("\n=== top 20 overall ===")
rows.sort(reverse=True)
for size, name, obj in rows[:20]:
    short = obj.split("/")[-1] if obj else "?"
    print(f"{size:5d} ({size/1024:5.2f}K)  {demangle(name):45s}  {short[:28]}")
