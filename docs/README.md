# ESP32-C3FH4 OTA Relay

Complete firmware for ESP32-C3FH4 acting as OTA relay between APP/PC and GD32E508VET6.

## Build

```bash
export IDF_PATH=/path/to/esp-idf
source $IDF_PATH/export.sh
mkdir build && cd build
cmake ..
make
```

## Structure

```
main/
  main.c           - Entry point, task creation
  app_main.c       - Application init, system monitor task
  ota_relay.c      - 4-stage OTA state machine
  spi_master.c     - SPI2 (VSPI) master driver
  protocol.c       - Frame format, CRC16, AES, scramble
  wifi_ota.c       - WiFi AP/STA + HTTP server
  ble_ota.c        - BLE GATT server
  health_monitor.c - Temperature/voltage/WiFi monitoring
```

## Features

- WiFi AP mode (SSID: ESP32_OTA_RELAY, pw: 12345678)
- BLE GATT OTA service
- HTTP POST /ota/upload for firmware
- SPI to GD32 (40MHz, GPIO4-9)
- Flash storage with断点续传
- System health monitoring

## TODO (confirm with GD32 team)

- MASTER_KEY_HERE - AES-128 master key (16 bytes)
- SCRAMBLE_CONST_HERE - scramble algorithm seed
- CHIP_UID_HERE - GD32 chip unique ID
- GATT UUIDs for OTA service