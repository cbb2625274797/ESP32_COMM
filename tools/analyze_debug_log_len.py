#!/usr/bin/env python3
"""Scan DEBUG_* format strings and estimate max expanded line length (not LOG_RING_SIZE)."""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

PAT = re.compile(
    r"DEBUG_(ERR|WARN|INFO|DBG)\s*\(\s*([^,]+)\s*,\s*\"((?:\\.|[^\"\\])*)\"",
    re.M,
)


def unescape_fmt(fmt: str) -> str:
    try:
        return bytes(fmt, "utf-8").decode("unicode_escape")
    except Exception:
        return (
            fmt.replace("\\n", "\n")
            .replace("\\r", "\r")
            .replace("\\t", "\t")
            .replace('\\"', '"')
        )


def fmt_max_len(fmt: str) -> tuple[int, bool]:
    """Return (max_len, unbounded_string) for fmt body (without our [L][tag] prefix)."""
    i = 0
    n = len(fmt)
    out = 0
    unbounded = False
    while i < n:
        if fmt[i] != "%":
            j = fmt.find("%", i)
            if j < 0:
                out += n - i
                break
            out += j - i
            i = j
            continue
        if i + 1 < n and fmt[i + 1] == "%":
            out += 1
            i += 2
            continue
        i += 1
        while i < n and fmt[i] in "-+ #0":
            i += 1
        width = 0
        if i < n and fmt[i] == "*":
            i += 1
        else:
            while i < n and fmt[i].isdigit():
                width = width * 10 + int(fmt[i])
                i += 1
        prec = None
        if i < n and fmt[i] == ".":
            i += 1
            if i < n and fmt[i] == "*":
                prec = 0
                i += 1
            else:
                prec = 0
                while i < n and fmt[i].isdigit():
                    prec = prec * 10 + int(fmt[i])
                    i += 1
        while i < n and fmt[i] in "hlLzjtq":
            i += 1
        if i >= n:
            break
        conv = fmt[i]
        i += 1
        if conv in "diouxX":
            est = 20 if conv in "diou" else 16
            if prec is not None:
                est = max(est, prec)
            if width:
                est = max(est, width)
            out += est
        elif conv in "fFeEgG":
            est = 48  # long double-ish
            if prec is not None:
                est = max(est, prec + 16)
            if width:
                est = max(est, width)
            out += est
        elif conv == "s":
            if prec is not None:
                est = prec
            else:
                # no precision: could be long; use 128 as aggressive scan bound + flag
                est = 128
                unbounded = True
            if width:
                est = max(est, width)
            out += est
        elif conv == "c":
            out += max(1, width or 1)
        elif conv == "p":
            out += 18
        elif conv == "PRIu32" or True:
            # leftover / unknown after PRI macros already expanded in source rarely
            if conv not in "diouxXfFeEgGscp%":
                out += 12
            else:
                out += 8
        else:
            out += 8
    return out, unbounded


def tag_len(tag_expr: str) -> tuple[int, str]:
    tag_expr = tag_expr.strip()
    m = re.match(r'^"([^"]*)"$', tag_expr)
    if m:
        return len(m.group(1)), m.group(1)
    # TAG macro / variable: measure common names
    if tag_expr in ("TAG", "kTag", "tag"):
        return 12, tag_expr
    return max(len(tag_expr), 8), tag_expr


def main() -> int:
    rows = []
    for path in ROOT.rglob("*"):
        if path.suffix not in {".cpp", ".c", ".h"}:
            continue
        parts = path.parts
        if "build" in parts or ".git" in parts:
            continue
        if "components" in parts and "cjson" in parts:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        rel = path.relative_to(ROOT).as_posix()
        for m in PAT.finditer(text):
            level, tag_expr, fmt_raw = m.group(1), m.group(2), m.group(3)
            fmt = unescape_fmt(fmt_raw)
            # strip trailing newline in fmt for body math; we add +1 for forced \n
            body_src = fmt.rstrip("\n\r")
            body, unb = fmt_max_len(body_src)
            tl, tag = tag_len(tag_expr)
            prefix = 6 + tl  # [L][tag]_  e.g. [I][WIFI] 
            total = prefix + body + 1  # + newline
            # line number
            line_no = text.count("\n", 0, m.start()) + 1
            rows.append(
                {
                    "total": total,
                    "body": body,
                    "prefix": prefix,
                    "unbounded": unb,
                    "level": level,
                    "tag": tag,
                    "fmt": body_src,
                    "file": rel,
                    "line": line_no,
                }
            )

    rows.sort(key=lambda r: (-r["total"], r["file"], r["line"]))
    print(f"DEBUG_* sites: {len(rows)}")
    print(f"design cap LOG_RING_SIZE=128 → usable max line = 127 bytes\n")
    print("--- Top 50 by estimated max total ([L][tag]+body+\\n) ---")
    print(f"{'est':>5} {'body':>5} {'pre':>3} {'>%159':>5}  level  tag            file:line")
    for r in rows[:50]:
        over = "YES" if r["total"] > 159 else ""
        unb = "*" if r["unbounded"] else " "
        fmt_short = r["fmt"].replace("\n", "\\n")
        if len(fmt_short) > 70:
            fmt_short = fmt_short[:67] + "..."
        print(
            f"{r['total']:5d}{unb}{r['body']:5d} {r['prefix']:3d} {over:>5}  "
            f"{r['level']:4s}  {r['tag'][:14]:14s} {r['file']}:{r['line']}"
        )
        print(f"       {fmt_short}")

    print("\n--- length histogram (estimated total) ---")
    bins = [(0, 80), (81, 110), (111, 127), (128, 160), (161, 200), (201, 99999)]
    for lo, hi in bins:
        c = sum(1 for r in rows if lo <= r["total"] <= hi)
        print(f"  {lo:5d}-{hi:5d}: {c:4d}")

    over = [r for r in rows if r["total"] > 159]
    unb = [r for r in rows if r["unbounded"]]
    print(f"\nSites with est > 159 (would truncate in ring): {len(over)}")
    print(f"Sites with unbounded %s (est used 128 for %s): {len(unb)}")

    # Also report pure fmt string lengths (no conversion expansion)
    print("\n--- Top 20 by format string literal length only ---")
    by_lit = sorted(rows, key=lambda r: (-len(r["fmt"]), r["file"]))
    for r in by_lit[:20]:
        print(f"  lit={len(r['fmt']):3d} est={r['total']:4d}  {r['file']}:{r['line']}  {r['fmt'][:80]!r}")

    # max among sites without unbounded %s
    bounded = [r for r in rows if not r["unbounded"]]
    if bounded:
        m = max(bounded, key=lambda r: r["total"])
        print(
            f"\nMax est among bounded formats (no open %s): "
            f"{m['total']} B @ {m['file']}:{m['line']} tag={m['tag']}"
        )
        print(f"  fmt={m['fmt'][:160]!r}")

    # For unbounded, try better estimate: common %s sources
    print("\n--- Unbounded %s samples (need runtime arg max) ---")
    for r in unb[:15]:
        print(f"  est~{r['total']}  {r['file']}:{r['line']}  {r['fmt'][:100]!r}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
