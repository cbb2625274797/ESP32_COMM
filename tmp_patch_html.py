path = r'tools\ble_ota.html'
with open(path, 'r', encoding='utf-8-sig') as f:
    html = f.read()

old_hint = "      hint: '当前目标：ESP32。页面会使用 <b>cmd=0x01</b> 发送数据，<mark>写入完成后会先发 0xF9 让设备端读回算 SHA-256</mark>，前端比对一致后才发 0xF8 触发收尾（end + set_boot + reboot）。'"
new_hint = "      hint: '当前目标：ESP32。页面会使用 <b>cmd=0x01</b> 发送数据，写入完成后发 <b>0xF8</b> + 32 字节 SHA-256 触发设备端收尾：设备端读 OTA 分区算 SHA 并比对，一致则 end+set_boot+reboot；不一致上报 0x85，不重启。'"
print('old in html:', old_hint in html)
html2 = html.replace(old_hint, new_hint, 1)
print('changed:', html2 != html)
print('new still present:', new_hint in html2)
with open(path, 'w', encoding='utf-8-sig') as f:
    f.write(html2)
print('written, file len:', len(html2))

# verify
with open(path, 'r', encoding='utf-8-sig') as f:
    reread = f.read()
print('re-read len:', len(reread))
print('new in reread:', new_hint in reread)
print('old in reread:', old_hint in reread)
