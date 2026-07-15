/**
 * feedback.js - 1Hz 反馈轮询（预留模块）
 *
 * 重新导出 common/connection.js 的连接管理函数，供 can 入口文件按需使用。
 * 1Hz 轮询 /can/feedback 或 ID 快照功能可在本模块按需实现。
 */
import { updateConnectionUi, updateHealthUi, updateHeapAndTaskStatusUi, testConnection, refreshSystemStatusNow } from '../common/connection.js';

// 重新导出 common/connection.js 的连接管理函数，供 can 入口文件按需使用。
export { updateConnectionUi, updateHealthUi, updateHeapAndTaskStatusUi, testConnection, refreshSystemStatusNow };
