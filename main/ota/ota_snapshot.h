/*
 * ota_snapshot.h - OTA 状态只读快照（便捷聚合头文件）
 *
 * 本文件本身不包含结构体定义，仅聚合两个快照来源：
 *   - RelaySnapshot  → 定义在 relay/ota_relay.h（一次锁保护读取 relay 进度）
 *   - OtaSnapshot    → 定义在 ota/ota_coordinator.h（coordinator + meta + relay 快照）
 *
 * HTTP handler 等消费方只需 #include "ota/ota_snapshot.h" 即可使用两种快照。
 */
#ifndef OTA_SNAPSHOT_H
#define OTA_SNAPSHOT_H

#include "relay/ota_relay.h"        // RelaySnapshot
#include "ota/ota_coordinator.h"    // OtaSnapshot

#endif // OTA_SNAPSHOT_H
