#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
# Match DEBUG_*(tag, "..." PRI... "..." ...)
pat = re.compile(
    r'DEBUG_(ERR|WARN|INFO|DBG)\s*\(\s*([^,]+)\s*,\s*'
    r'((?:"(?:\\.|[^"\\])*"\s*(?:PRIu\d+|PRIx\d+|PRId\d+|PRIX\d+)?\s*)+)',
    re.M,
)

rows = []
for path in (ROOT / "main").rglob("*.cpp"):
    t = path.read_text(encoding="utf-8")
    for m in pat.finditer(t):
        raw = m.group(3)
        parts = re.findall(r'"((?:\\.|[^"\\])*)"', raw)
        if not parts:
            continue
        joined = "".join(parts)
        # replace PRI holes with %u placeholder length 10 each
        pri_count = len(re.findall(r"PRI[udxX]\d+", raw))
        # rough: joined has empty holes between lits around PRI
        body = len(joined.encode()) + pri_count * 10
        # also % in joined
        body += joined.count("%") * 8  # remaining %
        if body < 100:
            continue
        ln = t.count("\n", 0, m.start()) + 1
        rows.append((body, path.relative_to(ROOT).as_posix(), ln, joined[:100], pri_count))

rows.sort(reverse=True)
print(f"Long concat fmt candidates: {len(rows)}")
for b, f, ln, j, pc in rows[:40]:
    print(f"{b:4d} {f}:{ln} pri={pc}")
    print(f"     {j}")
