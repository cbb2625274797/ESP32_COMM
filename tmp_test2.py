import os
os.chdir(r'G:\Robot_v2\DCU\ESP32_COMM')
with open('tools/ble_ota.html', 'r', encoding='utf-8-sig') as f:
    text = f.read()

# Try a small, single line replace
old = "    fileSha256: null,"
print('old line found:', text.count(old))

# Now do a multi-line block test
old_block = """    fileSha256: null,
    deviceSha256: null,
    expectedReboot: false,        // 发送 0xFA 后置 true，重连后清
    reconnecting: false,          // 自动重连中
    reconnectAttempts: 0,         // 重连尝试次数
    shaWaiter: null,              // {resolve, reject, timer} 0xC1 等待者
  };"""

print('block found:', old_block in text)
print('block repr (last 200):', repr(old_block[-200:]))
