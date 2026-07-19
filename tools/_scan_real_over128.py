#!/usr/bin/env python3
"""Scan DEBUG_* with realistic arg widths (actual-ish max line size)."""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
THRESHOLD = 128

PAT = re.compile(
    r'DEBUG_(ERR|WARN|INFO|DBG)\s*\(\s*([^,]+)\s*,\s*"((?:\\.|[^"\\])*)"\s*(?:,|\))',
    re.M,
)

# Known TAG macros
KNOWN_TAGS = {
    "TAG": 7,  # worst of BLE_OTA/OTA_CORE/OTA_COORD
    "kTag": 8,
}


def unescape(fmt: str) -> str:
    return fmt.replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"')


def parse_args(text: str, fmt_end: int) -> list[str]:
    i = fmt_end
    while i < len(text) and text[i] in " \t\r\n":
        i += 1
    if i < len(text) and text[i] == ")":
        return []
    if i < len(text) and text[i] == ",":
        i += 1
    args, depth, start = [], 0, i
    in_str, esc = False, False
    while i < len(text):
        c = text[i]
        if in_str:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            i += 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0 and c == ")":
                part = text[start:i].strip()
                if part:
                    args.append(part)
                break
            depth -= 1
        elif c == "," and depth == 0:
            part = text[start:i].strip()
            if part:
                args.append(part)
            start = i + 1
        i += 1
        if i - start > 500:
            break
    return args


def s_est(arg: str) -> int:
    a = arg.strip()
    if re.search(r"frontend_sha|device_hex|device_sha|sha_hex", a, re.I):
        return 64
    if re.search(r"resp_hex", a, re.I):
        return 64  # should not dump full anymore
    if "esp_err_to_name" in a:
        return 24
    if re.search(r"stateToString|phase|label|name|rx_phy|tx_phy", a, re.I):
        return 16
    m = re.match(r'^"((?:\\.|[^"\\])*)"$', a)
    if m:
        return len(unescape(m.group(1)).encode())
    if "?" in a and ":" in a:
        # ternary of string lits
        parts = re.findall(r'"((?:\\.|[^"\\])*)"', a)
        if parts:
            return max(len(unescape(p).encode()) for p in parts)
        return 8
    return 16


def body_est(fmt: str, args: list[str]) -> int:
    i, n, out, ai = 0, len(fmt), 0, 0

    def nexta() -> str:
        nonlocal ai
        if ai < len(args):
            a = args[ai]
            ai += 1
            return a
        return ""

    while i < n:
        if fmt[i] != "%":
            j = fmt.find("%", i)
            if j < 0:
                out += len(fmt[i:].encode())
                break
            out += len(fmt[i:j].encode())
            i = j
            continue
        if i + 1 < n and fmt[i + 1] == "%":
            out += 1
            i += 2
            continue
        i += 1
        while i < n and fmt[i] in "-+ #0":
            i += 1
        if i < n and fmt[i] == "*":
            nexta()
            i += 1
        else:
            while i < n and fmt[i].isdigit():
                i += 1
        if i < n and fmt[i] == ".":
            i += 1
            if i < n and fmt[i] == "*":
                nexta()
                i += 1
            else:
                while i < n and fmt[i].isdigit():
                    i += 1
        while i < n and fmt[i] in "hlLzjtq":
            i += 1
        if i >= n:
            break
        conv = fmt[i]
        i += 1
        arg = nexta()
        if conv in "diouxX":
            out += 10  # realistic
        elif conv in "fFeEgG":
            out += 8
        elif conv == "s":
            out += s_est(arg)
        elif conv == "c":
            out += 1
        elif conv == "p":
            out += 10
        else:
            out += 8
    return out


def tag_len(expr: str) -> tuple[int, str]:
    e = expr.strip()
    m = re.match(r'^"([^"]*)"$', e)
    if m:
        return len(m.group(1).encode()), m.group(1)
    if e in KNOWN_TAGS:
        return KNOWN_TAGS[e], e
    return 8, e


def main() -> int:
    rows = []
    for path in ROOT.rglob("*"):
        if path.suffix not in {".cpp", ".c", ".h"}:
            continue
        if "build" in path.parts or ".git" in path.parts:
            continue
        if "components" in path.parts:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        rel = path.relative_to(ROOT).as_posix()
        for m in PAT.finditer(text):
            level, tag_expr, fmt_raw = m.group(1), m.group(2), m.group(3)
            fmt = unescape(fmt_raw).rstrip("\n\r")
            args = parse_args(text, m.end(3) + 1)
            body = body_est(fmt, args)
            tlen, tag = tag_len(tag_expr)
            lc = {"ERR": "E", "WARN": "W", "INFO": "I", "DBG": "D"}[level]
            if tag_expr.strip().startswith('"'):
                prefix = len(f"[{lc}][{tag}] ".encode())
            else:
                prefix = len(f"[{lc}][".encode()) + tlen + len("] ".encode())
            total = prefix + body + 1
            if total <= THRESHOLD:
                continue
            ln = text.count("\n", 0, m.start()) + 1
            rows.append((total, body, prefix, level, tag, rel, ln, fmt))
    rows.sort(reverse=True)
    print(f"Realistic threshold >{THRESHOLD}B: {len(rows)} candidates\n")
    for i, (total, body, pre, lv, tag, rel, ln, fmt) in enumerate(rows, 1):
        fs = fmt if len(fmt) <= 90 else fmt[:87] + "..."
        print(f"{i:3d} {total:4d} body={body:3d} pre={pre:2d} {lv:4s} {str(tag)[:10]:10s} {rel}:{ln}")
        print(f"     {fs}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
