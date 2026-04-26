# ESP32-C3 OTA Relay

ESP32-C3FH4 OTA 中继固件，通过 WiFi / BLE 接收上位机 OTA 固件包，通过 SPI 转发给下位机（GD32）。

## 硬件平台

- **芯片:** ESP32-C3FH4 (RISC-V, 单核)
- **Flash:** 4MB
- **编译工具链:** ESP-IDF v6.0, riscv32-esp-elf

## 功能模块

| 模块 | 描述 |
|------|------|
| `health_monitor` | 温度 + 电压 + WiFi RSSI 监控 |
| `ota_relay` | OTA 数据包接收与转发 |
| `wifi_ota` | WiFi OTA 服务器 (HTTP) |
| `ble_ota` | BLE OTA 服务器 (GATT) |
| `spi_master` | SPI 主站，与下位机通信 |
| `protocol` | CRC 校验 + AES 解密 |

## 目录结构

```
main/
├── CMakeLists.txt
├── core/main.cpp          # 任务创建入口
├── relay/ota_relay.cpp   # OTA 中继逻辑
├── transport/
│   ├── spi_master.cpp    # SPI 主站
│   └── protocol.cpp       # 协议层 (CRC/AES)
├── server/
│   ├── wifi_ota.cpp      # WiFi OTA HTTP 服务器
│   └── ble_ota.cpp       # BLE OTA GATT 服务器
└── monitor/
    ├── health_monitor.cpp # 健康监控
    └── debug_log.cpp     # UDP 调试日志
```

## 任务配置 (单核 ESP32-C3)

所有任务固定运行在 CPU 核 0：

| 任务 | 优先级 | 栈大小 | 说明 |
|------|--------|--------|------|
| health_mon | configMAX_PRIORITIES - 4 | 4096 | 健康监控 |
| ota_relay | configMAX_PRIORITIES - 2 | 8192 | OTA 中继 |
| wifi_ota | configMAX_PRIORITIES - 3 | 8192 | WiFi OTA |
| ble_ota | configMAX_PRIORITIES - 3 | 8192 | BLE OTA |

## 待确认参数

- AES-128 主密钥 (MASTER_KEY_HERE)
- 搅伴算法种子 (SCRAMBLE_CONST_HERE)
- 芯片唯一码 (CHIP_UID_HERE)
- OTA Service UUID (需与 APP 侧协商)

## 编译

```bash
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

## 调试日志

UDP 输出到上位机，默认 IP: `DEBUG_LOG_IP`，端口: `DEBUG_LOG_PORT`。
