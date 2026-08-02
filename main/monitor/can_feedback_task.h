/*
 * can_feedback_task.h - 事件监听任务（0xC4/0xC5/0xC6/0xD2 推送）
 *
 * 2026-08 架构改造：0xC1 CAN 反馈已改为请求驱动（canFdQueryFeedback），
 * 不再被动缓存。本任务仅分发 GD32 主动推送的 ADC/电机状态/版本/OTA 进度事件。
 */
#ifndef CAN_FEEDBACK_TASK_H
#define CAN_FEEDBACK_TASK_H

/** @brief 暂停/恢复事件监听（OTA 期间调用 pause=true）。 */
void pauseCanFeedbackTask(bool pause);

/** @brief 查询监听是否处于暂停（OTA / send_batch 期间为 true）。
 *  请求驱动的 SPI 查询（canFdQueryFeedback）在暂停期间应跳过。 */
bool canFeedbackPaused();

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动事件监听任务（幂等）。
 * @return true 表示任务已存在或创建成功。
 */
bool startCanFeedbackTask();

#ifdef __cplusplus
}
#endif

#endif // CAN_FEEDBACK_TASK_H