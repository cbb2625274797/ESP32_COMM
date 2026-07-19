#!/usr/bin/env python3
"""List DEBUG_* call sites that can exceed 128 bytes (UTF-8), longest first.

Uses realistic bounds for known %s sources (SHA 64, SPI hex, esp_err_to_name, etc.).
"""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
THRESHOLD = 128  # bytes, including [L][TAG] prefix + body + \\n

PAT = re.compile(
    r"DEBUG_(ERR|WARN|INFO|DBG)\s*\(\s*([^,]+)\s*,\s*\"((?:\\.|[^\"\\])*)\"\s*(?:,|\))",
    re.M,
)

# When we can see following args in a window, classify %s
ARG_WIN = 200


def unescape_fmt(fmt: str) -> str:
    try:
        return bytes(fmt, "utf-8").decode("unicode_escape")
    except Exception:
        return fmt.replace("\\n", "\n").replace("\\r", "\r").replace("\\t", "\t")


def tag_info(tag_expr: str) -> tuple[int, str]:
    tag_expr = tag_expr.strip()
    m = re.match(r'^"([^"]*)"$', tag_expr)
    if m:
        return len(m.group(1).encode("utf-8")), m.group(1)
    # TAG / kTag variable — common values up to ~12
    return 12, tag_expr


def classify_s_arg(arg: str) -> int:
    """Max bytes for a %s argument expression (UTF-8)."""
    a = arg.strip()
    # SHA hex buffers
    if re.search(r"sha|hex|device_hex|frontend_sha", a, re.I):
        if "64" in a or "sha" in a.lower() or "hex" in a.lower():
            # device_hex / frontend_sha_hex are 64-char C strings
            if re.search(r"frontend_sha|device_hex|device_sha|out_device", a, re.I):
                return 64
            if re.search(r"resp_hex|payload_hex|hex\[", a, re.I):
                return 512  # worst unless capped in call site
            return 64
    if re.search(r"resp_hex", a, re.I):
        return 512  # SPI_OTA_FRAME_MAX*2
    if re.search(r"\bhex\b", a, re.I):
        # local hex[] in ota_relay often 64–72 chars
        return 72
    if "esp_err_to_name" in a:
        return 32
    if re.search(r"FW_VERSION|GIT_HASH|BUILD_TIME|version", a, re.I):
        return 16
    if re.search(r"partition|label|name|pcTaskName|task", a, re.I):
        return 16
    if re.search(r"stage|TAG|tag", a, re.I):
        return 24
    if re.search(r"ssid|ip_str|ip", a, re.I):
        return 32
    # string literals
    m = re.match(r'^"((?:\\.|[^"\\])*)"$', a)
    if m:
        return len(unescape_fmt(m.group(1)).encode("utf-8"))
    # ternary "...":"..."
    if "?" in a and ":" in a:
        return 16
    return 48  # generic unknown %s


def parse_args_after_fmt(text: str, fmt_end: int) -> list[str]:
    """Rough parse of C args after the format string until closing paren of DEBUG_*."""
    # fmt ends at quote; skip ", then args
    i = fmt_end
    # find the comma after format or )
    while i < len(text) and text[i] in " \t\r\n":
        i += 1
    if i < len(text) and text[i] == ")":
        return []
    if i < len(text) and text[i] == ",":
        i += 1
    args: list[str] = []
    depth = 0
    start = i
    in_str = False
    esc = False
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


def body_max_bytes(fmt: str, args: list[str]) -> tuple[int, list[str]]:
    """Walk fmt and consume args for max expanded body length in UTF-8 bytes."""
    # Work on UTF-8: literal parts as utf-8 len
    i = 0
    n = len(fmt)
    out = 0
    arg_i = 0
    notes: list[str] = []

    def next_arg() -> str:
        nonlocal arg_i
        if arg_i < len(args):
            a = args[arg_i]
            arg_i += 1
            return a
        return ""

    while i < n:
        if fmt[i] != "%":
            j = fmt.find("%", i)
            if j < 0:
                out += len(fmt[i:].encode("utf-8"))
                break
            out += len(fmt[i:j].encode("utf-8"))
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
            next_arg()  # width arg
            i += 1
        else:
            while i < n and fmt[i].isdigit():
                width = width * 10 + int(fmt[i])
                i += 1
        prec = None
        if i < n and fmt[i] == ".":
            i += 1
            if i < n and fmt[i] == "*":
                next_arg()
                prec = 0
                i += 1
            else:
                prec = 0
                while i < n and fmt[i].isdigit():
                    prec = prec * 10 + int(fmt[i])
                    i += 1
        # length
        while i < n and fmt[i] in "hlLzjtq":
            i += 1
        # PRI macros might appear as broken sequence in source - rare
        if i >= n:
            break
        conv = fmt[i]
        i += 1
        arg = next_arg()
        if conv in "diouxX":
            est = 20 if conv in "diou" else 16
            if prec is not None:
                est = max(est, prec)
            if width:
                est = max(est, width)
            out += est
        elif conv in "fFeEgG":
            est = 32
            if prec is not None:
                est = max(est, prec + 8)
            if width:
                est = max(est, width)
            out += est
        elif conv == "s":
            if prec is not None:
                est = prec
            else:
                est = classify_s_arg(arg)
                notes.append(f"%s←{est}:{arg[:40]}")
            if width:
                est = max(est, width)
            out += est
        elif conv == "c":
            out += max(1, width or 1)
        elif conv == "p":
            out += 18
        else:
            # maybe start of PRIu32 in string - treat as literal leftover
            out += 12
    return out, notes


def main() -> int:
    rows = []
    for path in ROOT.rglob("*"):
        if path.suffix not in {".cpp", ".c", ".h"}:
            continue
        if "build" in path.parts or ".git" in path.parts:
            continue
        if "components" in path.parts and "cjson" in path.parts:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        rel = path.relative_to(ROOT).as_posix()
        for m in PAT.finditer(text):
            level, tag_expr, fmt_raw = m.group(1), m.group(2), m.group(3)
            fmt = unescape_fmt(fmt_raw).rstrip("\n\r")
            # end of format string in source: position after closing quote of fmt
            # m.end() is after last "; use group 3 span
            fmt_end = m.end(3) + 1  # past closing "
            args = parse_args_after_fmt(text, fmt_end)
            body, notes = body_max_bytes(fmt, args)
            tlen, tag = tag_info(tag_expr)
            prefix = len(f"[{level[0] if False else ''}]")  # placeholder
            # real prefix: [E]/[W]/[I]/[D] + [tag] + space
            level_c = {"ERR": "E", "WARN": "W", "INFO": "I", "DBG": "D"}.get(level, "?")
            prefix = len(f"[{level_c}][{tag}] ".encode("utf-8")) if tag_expr.strip().startswith('"') else (6 + tlen)
            if not tag_expr.strip().startswith('"'):
                # use tlen for tag byte len estimate (ASCII tags)
                prefix = 6 + tlen
            else:
                prefix = len(f"[{level_c}][{tag}] ".encode("utf-8"))
            total = prefix + body + 1  # + \\n
            if total <= THRESHOLD:
                continue
            line_no = text.count("\n", 0, m.start()) + 1
            rows.append(
                {
                    "total": total,
                    "body": body,
                    "prefix": prefix,
                    "level": level,
                    "tag": tag if tag_expr.strip().startswith('"') else tag_expr.strip(),
                    "fmt": fmt,
                    "file": rel,
                    "line": line_no,
                    "notes": notes,
                    "args": args,
                }
            )

    rows.sort(key=lambda r: (-r["total"], r["file"], r["line"]))
    print(f"Threshold: >{THRESHOLD} bytes (prefix+body+newline, UTF-8)")
    print(f"Candidates: {len(rows)}\n")
    print(f"{'#':>3} {'est':>5} {'body':>5} {'pre':>3}  {'lv':4}  {'tag':12}  location")
    print("-" * 100)
    for i, r in enumerate(rows, 1):
        print(
            f"{i:3d} {r['total']:5d} {r['body']:5d} {r['prefix']:3d}  "
            f"{r['level']:4s}  {str(r['tag'])[:12]:12s}  {r['file']}:{r['line']}"
        )
        fmt_show = r["fmt"].replace("\n", "\\n")
        if len(fmt_show) > 100:
            fmt_show = fmt_show[:97] + "..."
        print(f"     fmt: {fmt_show}")
        if r["notes"]:
            print(f"     args: {'; '.join(r['notes'][:6])}")
    print(f"\nTotal over {THRESHOLD}B: {len(rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
