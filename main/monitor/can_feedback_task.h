/*
 * can_feedback_task.h - GD32 周期查询 + 事件消费任务
 *
 * 2026-08 架构改造：0xC1 CAN 反馈请求驱动（canFdQueryFeedback）；
 * 0xC4 ADC / 0xC5 电机状态改为本任务周期查询（250ms/1s）；
 * 0xC6 电机版本 / 0xD2 电机 OTA 进度保留事件推送，由本任务 takeEvent 消费。
 */
#ifndef CAN_FEEDBACK_TASK_H
#define CAN_FEEDBACK_TASK_H

/** @brief 暂停/恢复周期查询与事件消费（OTA 期间调用 pause=true）。 */
void pauseCanFeedbackTask(bool pause);

/** @brief 查询是否处于暂停（OTA / send_batch / 电机 OTA 期间为 true）。
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