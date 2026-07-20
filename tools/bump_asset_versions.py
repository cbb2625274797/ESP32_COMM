#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bump_asset_versions.py — 按本地静态资源内容 hash 自动更新 HTML 里的 ?v= 版本号。

动机
----
ESP32_COMM/tools 下多个 HTML（can.html / can_monitor.html / index.html）手工维护
<link>/<script> 引用的 `?v=...` query 版本号。components.css 修改后部分页面没 bump，
浏览器继续加载旧 CSS（审计 finding WEB-001）。

本脚本扫描所有 HTML 中形如
    <link ... href="css/foo.css?v=OLD">
    <script ... src="js/bar.js?v=OLD">
的本地资源引用，按文件内容计算短 hash（sha1 前 8 位十六进制），把 OLD 替换成 hash。
内容不变就不改，内容变了就 bump，所有页面引用同一资源时自动收敛到同一版本号。

用法
----
    python bump_asset_versions.py                 # 默认 dry-run，只打印将要改动的行
    python bump_asset_versions.py --write         # 实际写回 HTML
    python bump_asset_versions.py --check         # CI 模式，发现任何不一致即返回非 0
    python bump_asset_versions.py --root PATH     # 指定 tools 目录（默认脚本所在目录）

退出码
------
    0  全部一致（dry-run 无改动 或 --write 成功 或 --check 通过）
    1  发现需要 bump 的引用（dry-run/--check 时）
    2  参数错误或文件 IO 错误
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

# 匹配 <link href="...?v=..."> 与 <script src="...?v=...">，仅本地相对路径（无 scheme/host）。
# 路径里允许字母/数字/_-./ 子目录。query 部分仅捕获 v= 的值，保留其它 query key（若有）。
ASSET_REF_RE = re.compile(
    r"""
    (?P<tag>(?:href|src)\s*=\s*")            # 属性名 + =" 开头
    (?P<path>[A-Za-z0-9_./-]+\.(?:css|js))   # 本地资源路径（.css 或 .js）
    \?v=(?P<old>[^"&\s]+)                    # ?v= 版本号
    (?P<rest>"?)                             # 收尾引号（保留）
    """,
    re.VERBOSE,
)


def short_hash(content: bytes) -> str:
    """sha1 前 8 位十六进制；8 位足够区分日常 bump，且 URL 友好。"""
    return hashlib.sha1(content).hexdigest()[:8]


def find_html_files(root: Path) -> list[Path]:
    return sorted(p for p in root.glob("*.html") if p.is_file())


def process_html(html_path: Path) -> tuple[list[str], str]:
    """
    返回 (new_lines, new_text)。
    new_lines 是"将被改动"的描述列表（仅 dry-run/日志用），空列表表示无改动。
    new_text 是替换后的完整文本（无论是否有改动都返回）。
    """
    text = html_path.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    changes: list[str] = []

    def replace_in_line(line: str, line_no: int) -> str:
        def _sub(m: re.Match[str]) -> str:
            rel_path = m.group("path")
            old_ver = m.group("old")
            asset_path = html_path.parent / rel_path
            if not asset_path.is_file():
                # 资源缺失：不动，交给调用方在校验阶段报错
                return m.group(0)
            new_ver = short_hash(asset_path.read_bytes())
            if new_ver == old_ver:
                return m.group(0)
            changes.append(
                f"  L{line_no}: {rel_path}  {old_ver} -> {new_ver}"
            )
            return f'{m.group("tag")}{rel_path}?v={new_ver}{m.group("rest")}'

        return ASSET_REF_RE.sub(_sub, line)

    new_lines_text = [replace_in_line(line, i + 1) for i, line in enumerate(lines)]
    return changes, "".join(new_lines_text)


def collect_missing_assets(html_files: list[Path]) -> list[str]:
    """收集所有 HTML 引用但磁盘上不存在的本地资源，供 --check 报错。"""
    missing: set[str] = set()
    for html_path in html_files:
        text = html_path.read_text(encoding="utf-8")
        for m in ASSET_REF_RE.finditer(text):
            rel = m.group("path")
            if not (html_path.parent / rel).is_file():
                missing.add(f"{html_path.name}: {rel}")
    return sorted(missing)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="按本地静态资源内容 hash 自动更新 HTML 里的 ?v= 版本号。"
    )
    parser.add_argument(
        "--root", type=Path, default=None,
        help="tools 目录；默认为脚本所在目录。",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true", help="实际写回 HTML（默认 dry-run）。")
    mode.add_argument("--check", action="store_true", help="CI 模式：有改动或资源缺失即非 0 退出。")
    args = parser.parse_args(argv)

    root = (args.root if args.root is not None else Path(__file__).resolve().parent)
    if not root.is_dir():
        print(f"ERROR: root 不是目录: {root}", file=sys.stderr)
        return 2

    html_files = find_html_files(root)
    if not html_files:
        print(f"[bump] 没有找到 HTML 文件: {root}", file=sys.stderr)
        return 2

    mode_label = "check" if args.check else ("write" if args.write else "dry-run")
    print(f"[bump] root={root} mode={mode_label} html={len(html_files)}")

    missing = collect_missing_assets(html_files)
    if missing:
        print("[bump] 引用了不存在的本地资源：", file=sys.stderr)
        for item in missing:
            print(f"  {item}", file=sys.stderr)
        return 2

    total_changes = 0
    for html_path in html_files:
        changes, new_text = process_html(html_path)
        if not changes:
            continue
        total_changes += len(changes)
        rel = html_path.relative_to(root)
        print(f"[bump] {rel}（{len(changes)} 处）：")
        for line in changes:
            print(line)
        if args.write:
            html_path.write_text(new_text, encoding="utf-8")

    if total_changes == 0:
        print("[bump] 全部一致，无需 bump。")
        return 0

    if args.write:
        print(f"[bump] 已写回 {total_changes} 处版本号。")
        return 0
    if args.check:
        print(f"[bump] CHECK FAIL：{total_changes} 处版本号与内容不一致。", file=sys.stderr)
        return 1
    print(f"[bump] dry-run：{total_changes} 处需要 bump（加 --write 写回）。")
    return 1


if __name__ == "__main__":
    sys.exit(main())