/*
 * can_feedback_task.h - 被动接收 GD32 主动推送的 CAN 反馈快照
 *
 * ESP32 侧不主动 1Hz 轮询 0xC1；由 GD32 按铃推送，本任务 takeEvent 接收。
 */
#ifndef CAN_FEEDBACK_TASK_H
#define CAN_FEEDBACK_TASK_H

#include <stdint.h>
#include "transport/canfd_bridge.h"

/** @brief 暂停/恢复 CAN 反馈监听（OTA 期间调用 pause=true）。 */
void pauseCanFeedbackTask(bool pause);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 CAN 反馈被动监听任务（幂等）。
 * @return true 表示任务已存在或创建成功。
 */
bool startCanFeedbackTask();

/**
 * @brief 拷贝某通道的最新反馈快照（线程安全）。
 * @param channel 0/1/2。
 * @param out     输出缓冲。
 * @return true 表示拿到一份有效数据，false 表示通道无效或尚未拿到第一份。
 */
bool getLatestCanFeedback(uint8_t channel, can_feedback_result_t *out);

#ifdef __cplusplus
}
#endif

#endif // CAN_FEEDBACK_TASK_H
