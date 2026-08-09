/**
 * tick.worker.js - 电机轮询节拍心跳 Worker
 *
 * 浏览器对后台（隐藏）标签页的 setTimeout/setInterval 做 ≥1s 节流，
 * 但 Dedicated Worker 的定时器不受此限制。
 * 此 Worker 只做一件事：按主线程设定的间隔定期发送 'tick' 消息。
 *
 * 主线程收到 tick 后触发 pollLoop（如不在飞行中），
 * 从而实现后台标签页仍保持设定刷新速度。
 *
 * 消息协议：
 *   { type: 'start', interval: N }  → 启动节拍，interval 单位 ms
 *   { type: 'stop' }                 → 停止节拍
 *   { type: 'update', interval: N }  → 更新间隔（不重置 tick 计数）
 */
let timer = null;

self.onmessage = function(e) {
  const msg = e.data;
  if (!msg || !msg.type) return;

  if (msg.type === 'start' || msg.type === 'update') {
    if (timer) clearInterval(timer);
    timer = setInterval(() => {
      self.postMessage('tick');
    }, msg.interval || 250);
  } else if (msg.type === 'stop') {
    if (timer) {
      clearInterval(timer);
      timer = null;
    }
  }
};