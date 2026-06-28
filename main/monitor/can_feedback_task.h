/*
 * can_feedback_task.h - 周期性轮询 GD32 CAN 反馈快照的任务接口
 */
#ifndef CAN_FEEDBACK_TASK_H
#define CAN_FEEDBACK_TASK_H

#include <stdint.h>
#include "transport/canfd_bridge.h"

/** @brief ID 快照缓存结构。 */
typedef struct {
    id_snapshot_entry_t entries[CANFD_ID_SNAPSHOT_MAX];
    uint8_t             count;
    bool                valid;
} id_snapshot_cache_t;

/** @brief 暂停/恢复 CAN 反馈轮询（OTA 期间调用 pause=true）。 */
void pauseCanFeedbackTask(bool pause);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 1Hz CAN 反馈轮询任务（幂等）。
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

/**
 * @brief 获取指定通道的最新 ID 快照（线程安全）。
 * @param channel 0/1/2。
 * @param out     输出缓冲。
 * @return true 有有效数据。
 */
bool getLatestIdSnapshot(uint8_t channel, id_snapshot_cache_t *out);

#ifdef __cplusplus
}
#endif

#endif // CAN_FEEDBACK_TASK_H
