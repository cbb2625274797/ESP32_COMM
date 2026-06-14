import os
os.chdir(r'G:\Robot_v2\DCU\ESP32_COMM')
path = 'tools/ble_ota.html'

with open(path, 'r', encoding='utf-8-sig') as f:
    html = f.read()

# 计数
print('0xF9 BEFORE:', html.count('0xF9'))
print('shaWaiter BEFORE:', html.count('shaWaiter'))

# 1) TARGETS esp32 hint
old_hint = "      hint: '当前目标：ESP32。页面会使用 <b>cmd=0x01</b> 发送数据，<mark>写入完成后会先发 0xF9 让设备端读回算 SHA-256</mark>，前端比对一致后才发 0xF8 触发收尾（end + set_boot + reboot）。'"
new_hint = "      hint: 'TEST NEW'"
print('hint in html:', old_hint in html)
html = html.replace(old_hint, new_hint, 1)

with open(path, 'w', encoding='utf-8-sig') as f:
    f.write(html)

# 验证
with open(path, 'r', encoding='utf-8-sig') as f:
    reread = f.read()
print('TEST NEW present:', 'TEST NEW' in reread)
print('old hint present:', old_hint in reread)
