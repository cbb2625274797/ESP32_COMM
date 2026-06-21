#!/usr/bin/env python3
"""
ota_upload_test.py - ESP32 OTA 上传集成测试脚本

用法: python ota_upload_test.py
前提: 已编译 build/ESP32_COMM.bin，ESP32 已连接网络。
步骤: 上传固件 → 触发更新 → 等待重启并验证版本。
"""
import urllib.request, json, time, sys

ESP_IP = "192.168.31.127"  # 目标 ESP32 IP 地址（根据实际网络修改）

# Step 1: Upload ESP32 firmware
bin_path = r"G:\Robot_v2\DCU\ESP32_COMM\build\ESP32_COMM.bin"
with open(bin_path, "rb") as f:
    fw_data = f.read()

header = json.dumps({"type": "esp32", "total_size": len(fw_data)}).encode("utf-8")
body = header + fw_data
print(f"[1/3] Uploading {len(fw_data)} bytes...")

req = urllib.request.Request(f"http://{ESP_IP}/ota/upload", data=body, method="POST")
req.add_header("Content-Type", "application/octet-stream")
try:
    with urllib.request.urlopen(req, timeout=120) as resp:
        result = resp.read().decode()
        print(f"  Upload OK: {result}")
except urllib.error.HTTPError as e:
    body_text = e.read().decode()
    print(f"  Upload FAILED: HTTP {e.code} - {body_text}")
    sys.exit(1)

# Step 2: Trigger update
print("[2/3] Triggering OTA update...")
time.sleep(1)
req2 = urllib.request.Request(f"http://{ESP_IP}/ota/update", data=b"", method="POST")
try:
    with urllib.request.urlopen(req2, timeout=10) as resp:
        result2 = resp.read().decode()
        print(f"  Update OK: {result2}")
except (ConnectionResetError, urllib.error.URLError, OSError) as e:
    print(f"  Update triggered (connection reset expected): {type(e).__name__}")

# Step 3: Wait for ESP32 to come back
print("[3/3] Waiting for ESP32 to reboot with new firmware...")
for attempt in range(1, 31):
    time.sleep(5)
    try:
        req3 = urllib.request.Request(f"http://{ESP_IP}/system/status")
        with urllib.request.urlopen(req3, timeout=5) as resp:
            data = json.loads(resp.read().decode())
            ver = data.get("version", "?")
            bld = data.get("build_time", "?")
            upt = data.get("uptime_sec", 0)
            print(f"  Online! version={ver} build={bld} uptime={upt}s")
            break
    except Exception:
        if attempt % 5 == 0:
            print(f"  Still waiting... ({attempt}/30)")
else:
    print("  ESP32 did not come back within 150 seconds")
