/**
 * feedback.js - 1Hz 反馈轮询（预留模块）
 *
 * ⚠️ 说明：原 tools/can.js（1526 行）中不存在 1Hz 反馈轮询 / ID 快照表渲染相关代码。
 *    这些功能实际位于 tools/can_monitor.js（CAN 监控页面），操作的是 can_monitor.html
 *    的 DOM 元素（fb0State / fb0Rx / idsnapBody 等），与 can.html 页面的元素体系不同，
 *    因此无法直接迁移。
 *
 *    can.js 中与"连接状态反馈"相关的代码为 setConnectionState / testConnection
 *    （一次性 GET /system/status），属于入口文件（js/can.js）的 init/cacheElements/bindEvents
 *    编排范畴，不在本子模块内。
 *
 *    本模块按任务要求 import common/connection.js，供入口文件在需要时引入连接管理能力。
 *    若后续 can 页面需要增加 1Hz 轮询 /can/feedback 或 ID 快照功能，可在此模块实现。
 */
import { updateConnectionUi, updateHealthUi, updateHeapAndTaskStatusUi, testConnection, refreshSystemStatusNow } from '../common/connection.js';

// 重新导出 common/connection.js 的连接管理函数，供 can 入口文件按需使用。
export { updateConnectionUi, updateHealthUi, updateHeapAndTaskStatusUi, testConnection, refreshSystemStatusNow };
