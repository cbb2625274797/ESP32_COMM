/*
 * version.h - 固件版本信息（手动维护）
 *
 * 注意：本文件不再由构建脚本自动生成。如需修改版本号或 Git Hash，
 * 请直接编辑本文件。
 *   - FW_VERSION_MAJOR / MINOR / PATCH：发版时手动递增
 *   - GIT_HASH：手动填写对应提交的 git short hash
 *   - 编译时间：已改为构建系统自动生成（build_info.h 的 fwBuildTime()，
 *     每次构建刷新），手写 BUILD_TIME 已删——旧值停在 2026-07-26 20:25
 *     曾被误判为 OTA 回退。
 */
#pragma once

// 固件版本
#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 5
#define FW_VERSION_PATCH 0
#define FW_VERSION_STRING "0.5.0"   // 内存：s_ota_upload_recv 从常驻静态 4KB 改为 OTA 会话内 RAII malloc，稳态省 4KB 内部 DRAM（httpd 单线程+协调器互斥，结束/失败自动 free）。0.4.9 tx_total/tx_retry/tx_fail/tx_fail_last_type 计数进 /system/status：透明重试(kTxRetryMax=3)掩盖真实失败率，DBG 日志被 CONFIG_LOG_MAXIMUM_LEVEL=3 编译期裁掉→"默默失败无打印"。0.4.8 ring_steal_diag=[rxLen,rb0,rb1] 撕裂形态签名   // 0.4.5 CS 事务间最小间隔 1ms（transferClockedFlags 单点）：GD32 CS EXTI re-arm 被 ECAT/CAN 中断延迟时，ESP32 背靠背 CS 撞进 re-arm 窗口使 RX DMA mid-pulse 重装→查询帧撕裂→0x82 0xE8→迟到偷读风暴。0.4.4 快速响应类型禁用盲读。0.4.3 0x82 量化归因插桩。0.4.2 serviceRing 偷读迟到 RESPONSE 的 seq 失配修复：poll 超时放弃后按铃读把响应当 EVENT 偷走且不推进 m_txSeq→下一条命令复用旧序号被 GD32 判重吞掉回放缓存旧 0x82（命令不再执行根因）。0.3.85 INIT_STATUS fault_codes hex；0.3.84 模块级故障码；0.3.83 GD32 APP 初始化状态可视；0.3.82 UART0 RX 泵→/logs；0.3.81 GD32 OTA 包级防呆

// Git 信息（手动填写）
#define GIT_HASH "770ae14"
