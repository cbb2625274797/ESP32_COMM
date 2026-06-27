# 临时语法验证脚本：用 riscv32-esp-elf-g++ -fsyntax-only 验证 gd32_flash_store.cpp
# 验证完成后删除本脚本。
import json
import os
import shlex
import subprocess
import sys

ROOT = r"g:\Robot_v2\DCU\ESP32_COMM"
CC_PATH = os.path.join(ROOT, "build", "compile_commands.json")
# 对照测试：可切换目标源文件
TARGET_NAME = os.environ.get("VERIFY_SRC", "gd32_flash_store.cpp")
TARGET_SRC = os.path.join(ROOT, "main", "storage", TARGET_NAME) if TARGET_NAME == "gd32_flash_store.cpp" else os.path.join(ROOT, "main", "ota", TARGET_NAME)
RESP = os.path.join(ROOT, "build", "_syntax_resp.txt")

with open(CC_PATH, encoding="utf-8") as f:
    db = json.load(f)

entry = None
for e in db:
    if e.get("file", "").endswith(TARGET_NAME):
        entry = e
        break
if not entry:
    print("ERROR: 未找到", TARGET_NAME, "的编译条目")
    sys.exit(2)

work_dir = entry["directory"]
# 任务要求：gcc_path 从 compile_commands.json 第一条 command 提取
first_cmd = shlex.split(db[0]["command"], posix=False)
gcc_path = first_cmd[0]
gxx_path = gcc_path.replace("riscv32-esp-elf-gcc.exe", "riscv32-esp-elf-g++.exe")
if gxx_path == gcc_path:
    print("ERROR: 未能从 gcc 路径推导 g++ 路径:", gcc_path)
    sys.exit(2)
if not os.path.exists(gxx_path):
    print("ERROR: g++ 不存在:", gxx_path)
    sys.exit(2)

# 目标条目的编译参数（去掉首位的编译器路径，改用推导出的 g++）
args = shlex.split(entry["command"], posix=False)

# 过滤：去掉 -o 及其参数，去掉源文件名
filtered = []
i = 1
while i < len(args):
    a = args[i]
    if a == "-o":
        i += 2  # 跳过 -o 和输出文件
        continue
    if a.startswith("-o") and len(a) > 2:
        i += 1  # -oXXX 形式
        continue
    if os.path.abspath(a) == os.path.abspath(TARGET_SRC):
        i += 1
        continue
    if a.endswith("gd32_flash_store.cpp"):
        i += 1
        continue
    filtered.append(a)
    i += 1

# 组装新命令：g++ + 原 args（去 -o/源文件） + -fsyntax-only -Wall + 源文件
# 注意1：原始构建用 -std=gnu++26（GNU 扩展模式），若覆盖为 -std=c++17 会定义 __STRICT_ANSI__，
#        导致 newlib <stdio.h> 的 FILE 类型不暴露而报错（与本次修改无关的 newlib 兼容性问题）。
#        故保留原始 -std=gnu++26，仅追加 -fsyntax-only -Wall；-Werror 原始已有，可严格检查本文件。
# 注意2：freertos/esp_hw_support 头文件链引用 FILE，但独立 syntax check 时 newlib <stdio.h>
#        的 FILE typedef 被跳过（ota_coordinator.cpp 同样报错，与本次修改无关）。
#        加 -include stdio.h 预先定义 FILE，绕过此无关问题，仅验证本文件语法。
final_args = [gxx_path] + filtered + ["-fsyntax-only", "-Wall", "-include", "stdio.h", TARGET_SRC]

# 写 response file：每行一个参数。
# GCC @file 解析时反斜杠会被当转义符吃掉，因此对所有参数统一处理：
#   反斜杠 -> \\，双引号 -> \"，并用双引号包裹（双引号内 \\=\" 才是字面）。
def quote_for_resp(p):
    esc = p.replace("\\", "\\\\").replace('"', '\\"')
    return '"' + esc + '"'

with open(RESP, "w", encoding="utf-8") as f:
    for a in final_args[1:]:  # 第一个是 gxx 本身，放命令行
        f.write(quote_for_resp(a) + "\n")

print("WORK_DIR:", work_dir)
print("GXX:", gxx_path)
print("RESP:", RESP)
print("ARGS(excl gxx):", len(final_args) - 1, "params")

# 诊断：预处理后查找 FILE 的 typedef，确认 FILE 是否被定义
diag_args = [gxx_path] + filtered + ["-E", "-include", "stdio.h", TARGET_SRC]
print("--- 诊断（-E，grep typedef FILE/__FILE/__sFILE） ---")
proc = subprocess.run(diag_args, cwd=work_dir, capture_output=True, text=True)
import re
for line in proc.stdout.splitlines():
    s = line.strip()
    if ("typedef" in s and ("FILE" in s or "__sFILE" in s)):
        print(s)
print("RETURN CODE:", proc.returncode)



