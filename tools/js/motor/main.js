/**
 * motor/main.js - 电机控制台页面逻辑（ES Module 入口）
 * 由 motor.html 内联 <script>（原 L1014-L3660）机械外提，逻辑未改。
 * 原脚本已是 "use strict"，转 module 行为中性；唯一对外暴露 window.phaseDebug。
 */
/* ═══════════════════════════════════════════════════════════════
   电机控制台 — 共用 OTA 主页设备 IP（localStorage dcu_esp_ip），本页无连接配置
   数据源:
     GET  /motor/state     1Hz  → 节点列表 + STATUS 解码
     POST /can/send_batch  唯一发送入口（CMD / CONFIG / VERSION / OTA）
     GET  /can/feedback    1Hz  → 状态栏 RX/TX/ERR/TEC/REC/BUS
     GET  /system/status   进入页自动探测 + 5s 健康检查 / 断线 2s 探测
   CAN ID（29-bit 扩展，物理 ID，不含 0x80000000 标志）:
     STATUS 0x000N0100 · CMD 0x000N0200(DLC32) · CMD_CONFIG 0x000N0301(DLC24)
     GET_VERSION 请求/回报共用 0x000N00FF(同 ID, DLC24)
     CANFD_BOOT 0x000N0000 · NODE_ID SET/GET 0x000N0400/0401
   ═══════════════════════════════════════════════════════════════ */
"use strict";

/* IP 存取公共实现（localStorage dcu_esp_ip，与 OTA/CAN 页共用） */
import {
  DEFAULT_ESP_IP, getDeviceIp, saveDeviceIp
} from "../common/base-url.js";

/* ─── 控制模式定义 ─── */
const MODES = {
  0: { name: "Disable", zh: "关闭", targets: [] },
  1: { name: "Angle", zh: "位置控制", targets: ["angle"] },
  2: { name: "Velocity", zh: "速度控制", targets: ["velocity"] },
  3: { name: "Velocity_Angle", zh: "速度+位置", targets: ["velocity", "angle"] },
  4: { name: "MIT", zh: "MIT Mode", targets: ["current", "velocity", "angle"] },
  5: { name: "Current", zh: "电流/力矩", targets: ["current"] },
  6: { name: "OpenLoop", zh: "开环速度", targets: ["velocity"] },
  7: { name: "OpenLoop2", zh: "开环速度(2)", targets: ["velocity"] },
};
const MODE_DESCS = {
  0: "电机关闭输出，进入自由旋转状态",
  1: "闭环位置控制，目标角度生效",
  2: "闭环速度控制，目标速度生效",
  3: "速度+位置环联合控制，目标速度与目标角度同时生效",
  4: "MIT 力控模式，电流/速度/角度目标均参与计算",
  5: "电流环直接控制（力矩模式），目标电流生效",
  6: "开环速度控制，无编码器反馈",
  7: "开环速度控制（备用通道）",
};

/* ─── 全局状态 ─── */
/* LS_ESP_IP / DEFAULT_ESP_IP 由 common/base-url.js 导入（与 OTA / CAN 页共用） */
/* 本页输入持久化 key：v2 扩展了 nodeIdInput + 按节点版本缓存 + OTA 固件元数据。
 * 保存内容：modeSelect / inVoltage / inVelocity / inAngle / pidCurP / pidVelP /
 * pidAngP / repeatPeriod / chSelect / selectedNode / nodeIdInput /
 * versions（按 node 缓存 {soft,hard,boot}）/ otaFwMeta（固件名+大小）。
 * 不保存：实时状态（motor 数组、解码、波形缓冲、连接态）、固件 bytes（用 IndexedDB handle 恢复）。 */
const LS_MOTOR_UI = "dcu_motor_ui_v2";
const state = {
  connected: false,        // 连续成功探测后视为在线
  connecting: false,
  ip: DEFAULT_ESP_IP,
  ch: 0,
  chEnabled: [true, true, true], // 各通道启用状态（由 onFeedback 更新）
  chOnline: [0, 0, 0],     // 各通道在线电机数（由 onMotorState 按 m.ch 统计，驱动下拉框信号标记）
  selectedNode: null,      // 当前选中节点号
  motors: [],              // 最近一次 /motor/state 的 motors 数组
  decoded: new Map(),      // node -> 解码后的 STATUS 对象
  versions: new Map(),     // node -> {soft,hard,boot} 查询到的版本号（持久化）
  /* 用户可调的电机状态轮询间隔（ms）。由工具栏 #refreshRate 输入框驱动。
   * 注意：仅影响 motor 链路；feedback(CAN 1Hz)/status(1Hz lite+5s full) 保持独立。 */
  refreshIntervalMs: 250,
  lastSentMode: new Map(), // node -> 最近发送的 Control_Mode
  sendInFlight: false,     // send_batch 互斥
  pendingCmd: null,        // latest-wins CMD 合并槽：发送在途时新 CMD 覆盖此帧，完成后立即发出
  busCooldownUntil: 0,     // 0xE2/0xE3 冷却截止时间戳
  txCount: 0,
  poll: { tick: null, probe: null, status: null, feedback: null },
  pollGen: 0,              // 递增使在途 tick 失效
  tickInFlight: false,
  feedbackInFlight: false, // /can/feedback 独立链路占用标记
  statusInFlight: false,   // /system/status 独立链路占用标记
  failCount: 0,            // 连续硬失败计数，达阈值才断线
  probing: false,          // 已在断线探测，避免重复 startProbe/toast
  lastConnToastAt: 0,      // toast 去抖
  offlineSince: 0,         // 进入离线时刻，短离线恢复不 toast
  tickN: 0,                // 轮询序号（每 5 轮附带 status）
  statusTickN: 0,           // status lite/full 交替计数（4 lite + 1 full 循环）
  phaseDebug: { lastSlowReq: null, slowStats: {} },  // 慢请求诊断（>200ms）
};

/* 控制台诊断入口：main.js 以 ES module 加载，state 等顶层变量不会挂到 window，
 * 控制台直接敲 state 会报 "state is not defined"。这里显式暴露只读诊断快照。
 * 用法：控制台敲 motorDbg.state 。 */
window.motorDbg = {
  state,                                                   // 完整 state 引用，方便深挖
};
/**
 * 连续「硬失败」多少次才判离线。
 * HTTP 500/503 busy、超时 一律视为瞬时，不计入、不改 LED。
 */
const FAIL_THRESHOLD = 5;
const CONN_TOAST_GAP_MS = 6000;
/** 离线不足此时长就恢复：不弹「连接已恢复」（过滤毛刺） */
const RECOVER_TOAST_MIN_OFFLINE_MS = 8000;
/* 前端固定节拍：与 GD32 0xC5 推送周期对齐（250ms）。 */
const TICK_INTERVAL_MS = 250;
const PROBE_INTERVAL_MS = 2000;

const MIN_CMD_PERIOD_MS = 80;

/* ─── DOM refs ─── */
const $ = (s) => document.querySelector(s);
const nodeListEl = $("#nodeList");

/* ─── 工具函数 ─── */
/** 读取主页写入的共享设备 IP（localStorage dcu_esp_ip） */
function getSharedDeviceIp() {
  return getDeviceIp();
}

/* ─── 输入持久化：刷新/重开页面后恢复上次输入 ───
 * 保存 strategy：用户改输入项时，把所有需保存项打包成 JSON 写 localStorage。
 * 恢复 strategy：页面加载时读出，按字段类型回填到对应 DOM + state。
 * 版本号按节点缓存（versions[node] = {soft,hard,boot}），切节点时回填。 */
const PERSIST_FIELDS = [
  "modeSelect", "inVoltage", "inVelocity", "inAngle",
  "pidCurP", "pidVelP", "pidAngP", "repeatPeriod", "chSelect",
  "nodeIdInput", "refreshRate",
];

function saveUiState() {
  try {
    const o = {};
    for (const id of PERSIST_FIELDS) {
      const el = document.getElementById(id);
      if (el) o[id] = el.value;
    }
    if (state.selectedNode != null) o.selectedNode = state.selectedNode;
    /* 版本缓存：state.versions 是 Map<node, {soft,hard,boot}>，序列化为普通对象 */
    if (state.versions && state.versions.size > 0) {
      const vo = {};
      for (const [n, v] of state.versions) {
        if (v && (v.soft || v.hard || v.boot)) vo[n] = { soft: v.soft || null, hard: v.hard || null, boot: v.boot || null };
      }
      o.versions = vo;
    }
    localStorage.setItem(LS_MOTOR_UI, JSON.stringify(o));
  } catch (_) {}
}

function loadUiState() {
  try {
    const raw = localStorage.getItem(LS_MOTOR_UI);
    if (!raw) return;
    const o = JSON.parse(raw);
    if (!o || typeof o !== "object") return;
    for (const id of PERSIST_FIELDS) {
      const el = document.getElementById(id);
      if (el && o[id] != null) el.value = o[id];
    }
    if (o.selectedNode != null) state.selectedNode = o.selectedNode;
    /* 恢复版本缓存到 state.versions Map */
    if (o.versions && typeof o.versions === "object") {
      for (const [n, v] of Object.entries(o.versions)) {
        const node = parseInt(n, 10);
        if (Number.isFinite(node) && v) state.versions.set(node, { soft: v.soft || null, hard: v.hard || null, boot: v.boot || null });
      }
    }
    /* 恢复刷新速度到 state（loadUiState 在 initPage 阶段调用，
     * 此时 polling 尚未启动，直接写 state 即可，无需重排定时器） */
    const rr = parseInt(o.refreshRate, 10);
    if (Number.isFinite(rr)) {
      /* 与 #refreshRate min/max 一致：最快 50ms(20Hz)，最慢 5s */
      const clamped = clampNum(rr, 50, 5000);
      state.refreshIntervalMs = clamped;
      const el = document.getElementById("refreshRate");
      if (el) el.value = clamped;
    }
  } catch (_) {}
}

/* 在 PERSIST_FIELDS 上挂 change/input 监听，自动保存。 */
function bindAutoSave() {
  for (const id of PERSIST_FIELDS) {
    const el = document.getElementById(id);
    if (el) {
      el.addEventListener("change", saveUiState);
      if (el.tagName === "INPUT") {
        el.addEventListener("input", debounceSaveUi);
      }
    }
  }
}
/* input 事件高频触发，debounce 300ms 避免每次按键都写 localStorage。 */
let _saveTimer = null;
function debounceSaveUi() {
  if (_saveTimer) clearTimeout(_saveTimer);
  _saveTimer = setTimeout(saveUiState, 300);
}
/** status 回报的设备 IP 写回共享存储（与 CAN/OTA 页一致） */
function setSharedDeviceIp(ip) {
  if (!ip || !String(ip).trim()) return;
  const v = String(ip).trim();
  state.ip = v;
  saveDeviceIp(v);
  const label = $("#ipLabel");
  if (label) label.textContent = "IP " + v;
}
function getBaseUrl() {
  return "http://" + state.ip;
}
function pad2(n) { return n.toString(16).padStart(2, "0").toUpperCase(); }
function hexId(id) { return "0x" + id.toString(16).toUpperCase().padStart(8, "0"); }
function nodeHex(n) { return n.toString(16).toUpperCase(); }
function nowMs() { return Date.now(); }
function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }
function clampNum(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }

/* CAN ID 构造（物理 ID，node 位于 bits[19:16]）— 对齐 DBC Motor_CAN_FD v0.70 */
function canId(node, suffix) { return ((node & 0xF) << 16) | (suffix & 0xFFFF); }
const ID_STATUS = (n) => canId(n, 0x0100);       /* 0x000N0100 */
const ID_CMD = (n) => canId(n, 0x0200);           /* 0x000N0200 */
const ID_CMD_CONFIG = (n) => canId(n, 0x0301);    /* 0x000N0301 */
/* 版本：主机与电机共用 0x000N00FF（同 ID 应答，DBC CANFD_GET_VERSION，发送方 Motor）。
 * 电机 APP 仅在收到 base_id=0x00FF 时回报（CANFD_VERSION_ID），DBC 无 0x05FF 消息。
 * GD32 为解析核心：捕获电机应答后缓存，经 0xC6 同步给 ESP32，前端读 /motor/version。 */
const ID_GET_VERSION_REQ = (n) => canId(n, 0x00FF);  /* 请求（与应答同 ID） */
const ID_SET_NODE_ID = (n) => canId(n, 0x0400);      /* 0x000N0400  DLC 2 */
const ID_GET_NODE_ID = (n) => canId(n, 0x0401);      /* 0x000N0401  DLC 2 */

/* hex 字符串 → Uint8Array */
function hexToBytes(hex) {
  const clean = (hex || "").replace(/[^0-9a-fA-F]/g, "");
  const out = new Uint8Array(Math.floor(clean.length / 2));
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(clean.substr(i * 2, 2), 16);
  }
  return out;
}
/* Uint8Array → 空格分隔 hex（用于状态栏预览） */
function bytesToHexSpaced(bytes, maxBytes) {
  const n = maxBytes ? Math.min(bytes.length, maxBytes) : bytes.length;
  let s = "";
  for (let i = 0; i < n; i++) s += pad2(bytes[i]) + " ";
  return s.trim();
}

/* ─── Toast ─── */
function toast(msg, type = "ok") {
  const el = document.createElement("div");
  el.className = `toast ${type}`;
  const ledCls = type === "ok" ? "led-on" : type === "warn" ? "led-warn" : "led-err";
  el.innerHTML = `<span class="led ${ledCls}" style="width:7px;height:7px"></span><span></span>`;
  el.lastElementChild.textContent = msg;
  $("#toastContainer").appendChild(el);
  setTimeout(() => { el.classList.add("out"); setTimeout(() => el.remove(), 220); }, 3000);
}

/* ─── Modal ─── */
let modalResolve = null;
function showModal(title, desc) {
  return new Promise((resolve) => {
    modalResolve = resolve;
    $("#modalTitle").textContent = title;
    $("#modalDesc").textContent = desc;
    $("#modalMask").classList.add("show");
  });
}
$("#modalCancel").onclick = () => { $("#modalMask").classList.remove("show"); if (modalResolve) modalResolve(false); };
$("#modalConfirm").onclick = () => { $("#modalMask").classList.remove("show"); if (modalResolve) modalResolve(true); };

/* ═══════════════════════ STATUS 帧解码（对齐 DBC Motor_CAN_FD v0.70） ═══════════════════════
 * BO_ STATUS DLC=32，信号起止与字节（Intel @1，除非注明 Motorola @0）：
 *   MCU_temp      0|16@1-  ×0.01  → B0-1
 *   Motor_temp   16|16@1-  ×0.01  → B2-3
 *   U/V/W_cur  32/48/64|16@1- mA  → B4-5 / B6-7 / B8-9
 *   motStatus    80|1@1+   0正常/1报错 → B10.0
 *   magStatus    81|1@1+            → B10.1
 *   thermal      82|1@1+            → B10.2
 *   EEPROMStatus 85|3@0+ Motorola   → B10 bits 85,84,83 → (B10>>3)&7
 *   Power_Voltage 88|16@1+ ×0.001 V → B11-12  ★非对齐
 *   RT_Angle    104|32@1- float °   → B13-16  ★非对齐
 *   RT_Velocity 136|32@1- float °/s → B17-20
 *   RT_Current  168|32@1- float mA  → B21-24
 *   test_float  200|32@1- float     → B25-28
 *   test_U16    232|16@1+           → B29-30
 *   CPU_Usage   248|8@1+  %         → B31
 *
 * 旧实现误把 Power 当 B12、float 当 B16/20/24/28，导致角度/速度/电流整体错位乱码。
 */
function decodeStatus(bytes) {
  if (!bytes || bytes.length < 32) return null;
  /* 拷到独立 ArrayBuffer，避免 SharedArrayBuffer/子视图偏移干扰 DataView */
  const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  const copy = new Uint8Array(32);
  copy.set(u8.subarray(0, 32));
  const dv = new DataView(copy.buffer);
  const b10 = copy[10];

  const f32 = (off) => {
    const v = dv.getFloat32(off, true);
    return Number.isFinite(v) ? v : 0;
  };

  return {
    mcuTemp: dv.getInt16(0, true) * 0.01,
    motTemp: dv.getInt16(2, true) * 0.01,
    uCurrent: dv.getInt16(4, true),
    vCurrent: dv.getInt16(6, true),
    wCurrent: dv.getInt16(8, true),
    motStatus: (b10 & 0x01) ? 1 : 0,          /* 0=正常 1=报错（DBC VAL_） */
    magStatus: (b10 & 0x02) ? 1 : 0,
    thermalStatus: (b10 & 0x04) ? 1 : 0,
    /* Motorola 3bit start=85 → 位序 85,84,83 = B10[5:3] */
    eepromStatus: (b10 >> 3) & 0x07,
    powerVoltage: dv.getUint16(11, true) * 0.001,  /* bit88 → B11 */
    rtAngle: f32(13),                              /* bit104 → B13 */
    rtVelocity: f32(17),                           /* bit136 → B17 */
    rtCurrent: f32(21),                            /* bit168 → B21 */
    testFloat: f32(25),                            /* bit200 → B25 */
    testU16: dv.getUint16(29, true),               /* bit232 → B29 */
    cpuUsage: copy[31],                            /* bit248 → B31 */
  };
}

/**
 * STATUS 故障位简写（节点卡片右上角，空间紧凑）。
 * 来源 B10：motStatus/magStatus/thermal + EEPROMStatus(3bit)。
 * 无故障返回 ""；有故障用 · 连接，如 "电机·磁编" / "过温" / "EE3"。
 */
function faultShortParts(d) {
  if (!d) return [];
  const parts = [];
  if (d.motStatus) parts.push("电机");
  if (d.magStatus) parts.push("磁编");
  if (d.thermalStatus) parts.push("过温");
  if (d.eepromStatus) parts.push("EE" + d.eepromStatus);
  return parts;
}
function faultShortLabel(d) {
  const parts = faultShortParts(d);
  return parts.length ? parts.join("·") : "";
}
function faultCountOf(d) {
  return faultShortParts(d).length;
}

/* ═══════════════════════ CMD 帧编码（对齐 DBC Motor_CAN_FD v0.70，DLC 32） ═══════════════════════
 * Control_Mode        0|8@1+
 * Limit_Voltage_cmd   8|16@1+ ×0.001 → B1-2
 * Target_Current_cmd 24|32@1- float  → B3-6
 * Target_Veloctiy_cmd 56|32@1- float → B7-10
 * Target_Angle_cmd   88|32@1- float  → B11-14
 * PID_current_loop_P 120|16@1-       → B15-16
 * PID_vel_loop_P     136|16@1-       → B17-18
 * PID_angle_loop_P   152|16@1-       → B19-20
 */
/* ── CMD 数值输入提交制 ──
 * 周期发送每 80~100ms 读一次输入框：若直接读 .value，键入 "200" 的中间态
 * "2"/"20" 会被当目标值发给电机。改为：输入过程不生效，按回车或点击别处
 * （blur 触发 change）才提交到 cmdCommitted，buildCmdBytes 只读已提交值。 */
const CMD_VALUE_IDS = ["inVoltage", "inCurrent", "inVelocity", "inAngle", "pidCurP", "pidVelP", "pidAngP"];
const cmdCommitted = {};
function initCmdCommit() {
  for (const id of CMD_VALUE_IDS) {
    const el = document.getElementById(id);
    if (!el) continue;
    cmdCommitted[id] = el.value;  /* 初始提交：含 loadUiState 回填值（本函数在其之后调用） */
    el.addEventListener("change", () => { cmdCommitted[id] = el.value; });  /* blur/回车后值变化才触发 */
    el.addEventListener("keydown", (e) => { if (e.key === "Enter") el.blur(); });  /* 回车 = 失焦提交 */
  }
}
/* 读已提交值（未 init 时回退读当前框值，保险） */
function cmdVal(id) {
  return cmdCommitted[id] !== undefined ? cmdCommitted[id] : $("#" + id).value;
}

function buildCmdBytes() {
  const buf = new ArrayBuffer(32);
  const dv = new DataView(buf);
  const mode = parseInt($("#modeSelect").value, 10) || 0;
  dv.setUint8(0, mode);
  dv.setUint16(1, Math.round((parseFloat(cmdVal("inVoltage")) || 0) * 1000) & 0xFFFF, true);
  dv.setFloat32(3, parseFloat(cmdVal("inCurrent")) || 0, true);
  dv.setFloat32(7, parseFloat(cmdVal("inVelocity")) || 0, true);
  dv.setFloat32(11, parseFloat(cmdVal("inAngle")) || 0, true);
  dv.setInt16(15, parseInt(cmdVal("pidCurP"), 10) || 0, true);
  dv.setInt16(17, parseInt(cmdVal("pidVelP"), 10) || 0, true);
  dv.setInt16(19, parseInt(cmdVal("pidAngP"), 10) || 0, true);
  return new Uint8Array(buf);
}
function bytesToHexStr(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i++) s += pad2(bytes[i]);
  return s;
}

/* ═══════════════════════ 发送核心 /can/send_batch（互斥 + 流控） ═══════════════════════ */
const GD32_ERR_DESC = {
  0xE0: "payload 过短", 0xE1: "通道号非法",
  0xE2: "BUS_OFF（总线离线）", 0xE3: "TEC 过高",
  0xE4: "邮箱满（对端不 ACK）", 0xE5: "参数无效", 0xEE: "SPI 帧解析失败",
  0xFF: "无有效 SPI 响应（陈旧/空帧，非总线故障）",
};
/* -3 仅表示“GD32 未回 ACK”；是否真为总线故障看 gd32_err：0xE2/0xE3 才是总线，0xFF 是陈旧 SPI 帧 */
const CAN_SEND_ERR_DESC = { "-1": "参数错误", "-2": "SPI 通信失败", "-3": "GD32 未确认发送" };

function describeResult(result) {
  if (result && result.status === "ok") {
    return result.recv ? ("recv 0x" + result.recv + " (" + result.recv_len + "B)") : "ok";
  }
  const code = result ? result.code : -99;
  let desc = CAN_SEND_ERR_DESC[String(code)] || ("code=" + code);
  if (result && result.gd32_err != null && result.gd32_err !== 0) {
    desc += " · " + (GD32_ERR_DESC[result.gd32_err] || ("gd32_err=0x" + result.gd32_err.toString(16).toUpperCase()));
  }
  return desc;
}

/**
 * 发送一批帧（唯一入口）。互斥：同一时刻只允许一个请求在途。
 * @returns {Promise<{ok:boolean, results?:object[], networkError?:boolean, busFault?:number, desc?:string}>}
 */
async function sendBatch(frames, opts) {
  opts = opts || {};
  if (state.sendInFlight) {
    return { ok: false, desc: "上一次发送仍在进行（已跳过）", busy: true };
  }
  if (!state.connected && !opts.allowOffline) {
    return { ok: false, desc: "设备未连接", networkError: true };
  }
  state.sendInFlight = true;
  const n = frames.length;
  /* 单帧 3s：ESP32 handler 内部单帧超时 1200ms + HTTP 排队余量；
   * 旧值 8s 会在 SPI 卡顿时长时间霸占互斥锁，阻塞后续周期 CMD */
  const timeoutMs = (n <= 1) ? 3000 : Math.max(15000, n * 500);
  try {
    const resp = await fetch(getBaseUrl() + "/can/send_batch", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ ch: state.ch, frames: frames }),
      signal: AbortSignal.timeout(timeoutMs),
    });
    const text = await resp.text();
    let json = {};
    try { json = text ? JSON.parse(text) : {}; } catch (e) { throw new Error("响应非合法 JSON"); }
    if (!resp.ok) throw new Error("HTTP " + resp.status + ": " + (json.message || text || "发送失败"));
    if (!json.results || !Array.isArray(json.results)) throw new Error("响应缺少 results");

    let results = json.results.slice();
    const need = n - results.length;
    if (need > 0) {
      const pad = { status: "error", code: json.abort_code != null ? json.abort_code : -2, gd32_err: json.abort_gd32_err || 0 };
      for (let p = 0; p < need; p++) results.push(pad);
    }

    /* 流控：检查首帧 gd32_err */
    const first = results[0] || {};
    const gerr = first.gd32_err;
    if (gerr === 0xE4 && !opts.retried) {
      /* 邮箱满：随机退避 50~150ms 重试一次 */
      state.sendInFlight = false;
      const backoff = 50 + Math.floor(Math.random() * 101);
      await sleep(backoff);
      return sendBatch(frames, Object.assign({}, opts, { retried: true }));
    }
    if (gerr === 0xE2 || gerr === 0xE3) {
      /* BUS_OFF / TEC 高：停止发送，3s 冷却 */
      state.busCooldownUntil = nowMs() + 3000;
      stopRepeat(true);
      toast("总线异常 (" + GD32_ERR_DESC[gerr] + ") — 停止发送，冷却 3s", "err");
      return { ok: false, results: results, busFault: gerr, desc: GD32_ERR_DESC[gerr] };
    }
    const allOk = results.every((r) => r.status === "ok");
    return { ok: allOk, results: results, desc: describeResult(first) };
  } catch (e) {
    /* 发送失败：瞬时错误静默；真断线累计，达阈值才离线 */
    noteHardFailure(e, "send");
    return { ok: false, networkError: true, desc: e.message || "网络错误" };
  } finally {
    state.sendInFlight = false;
    /* latest-wins 排空：发送期间新到的 CMD 已合并到 pendingCmd（仅保留最新帧），
     * 此处立即发出，保证周期指令不因互斥而丢失。E4 重试路径（已提前清锁）不触发。 */
    if (!opts.retried && state.pendingCmd && state.connected && nowMs() >= state.busCooldownUntil) {
      const p = state.pendingCmd;
      state.pendingCmd = null;
      sendBatch([{ id: p.id, dlc: p.dlc, data: p.data }], { silent: true });
    }
  }
}

/* 单帧便捷封装 */
async function sendOneFrame(id, dlc, dataHex, opts) {
  return sendBatch([{ id: id, dlc: dlc, data: dataHex }], opts);
}

/** 手动一次性发送（CONFIG / VERSION / 节点 ID 等）：先等通道空闲再发。
 * 周期 CMD 及其排空链通常 <100ms 完成，短暂等待即可避免 busy 拒绝；
 * 超过 1.5s 仍忙则照发（sendBatch 返回 busy 并 toast 提示）。 */
async function sendOneFrameWhenFree(id, dlc, dataHex, opts) {
  const deadline = nowMs() + 1500;
  while (state.sendInFlight && nowMs() < deadline) await sleep(25);
  const res = await sendOneFrame(id, dlc, dataHex, opts);
  if (res.busy) toast("发送通道仍忙（上一包未完成），请稍后重试", "warn");
  return res;
}

/* ═══════════════════════ 连接稳健性 ═══════════════════════
 * 设备端 /system/status、/motor/state、/can/feedback 共用 httpJsonWorkMutex。
 * 任一并发 → 500 busy；旧逻辑还把瞬时失败变成 LED 黄灯/断线 toast → 「偶尔闪一下」。
 *
 * 现策略：
 *  1) 全链路单飞：递归 setTimeout，同一时刻只有 1 个 HTTP
 *  2) 顺序：motor → feedback →（每 5 轮）status
 *  3) 500/503/超时 = 瞬时，静默跳过，不改 LED、不计数
 *  4) 仅「真断线」类错误累计，达阈值才 banner/toast
 *  5) 短离线（<8s）恢复不弹「连接已恢复」
 */
function connToast(msg, type) {
  const now = nowMs();
  if (now - state.lastConnToastAt < CONN_TOAST_GAP_MS) return;
  state.lastConnToastAt = now;
  toast(msg, type);
}

/** 500/503 busy、超时：设备忙，不是链路掉线 */
function isTransientError(e) {
  if (!e) return false;
  if (e.transient === true) return true;
  const st = Number(e.status);
  if (st === 500 || st === 503 || st === 429) return true;
  const name = e.name || "";
  if (name === "TimeoutError" || name === "AbortError") return true;
  const msg = String(e.message || e);
  if (/timed?\s*out|aborted|AbortError|signal is aborted/i.test(msg)) return true;
  if (/^HTTP\s+(500|503|429)\b/.test(msg)) return true;
  return false;
}

/**
 * 统一 GET JSON。失败抛 Error，附带 status / transient。
 * @returns {Promise<object>}
 */
async function fetchDeviceJson(path, timeoutMs) {
  let resp;
  const t0 = performance.now();
  try {
    resp = await fetch(getBaseUrl() + path, {
      signal: AbortSignal.timeout(timeoutMs || 3000),
      cache: "no-store",
    });
  } catch (e) {
    const err = e instanceof Error ? e : new Error(String(e));
    err.transient = isTransientError(err);
    recordSlowReq(path, performance.now() - t0, err.message);
    throw err;
  }
  recordSlowReq(path, performance.now() - t0);
  if (!resp.ok) {
    const err = new Error("HTTP " + resp.status);
    err.status = resp.status;
    err.transient = (resp.status === 500 || resp.status === 503 || resp.status === 429);
    throw err;
  }
  return resp.json();
}

/* 慢请求统计：>200ms 记入 phaseDebug.slowStats[path] = {count, maxMs, lastMs, lastAt}。
 * 用于诊断"卡在哪个 endpoint"——不是看单次 lastSlowReq，而是看各 path 的卡顿分布。
 * 控制台 phaseDebug.slowStats 直接看哪个 path 累计最多。 */
function recordSlowReq(path, durMs, errMsg) {
  if (durMs <= 200) return;
  const s = state.phaseDebug.slowStats;
  const key = path.split("?")[0];   /* /motor/state?bin=1 → /motor/state */
  if (!s[key]) s[key] = { count: 0, maxMs: 0, lastMs: 0, lastAt: 0, lastErr: null };
  s[key].count++;
  s[key].maxMs = Math.max(s[key].maxMs, Math.round(durMs));
  s[key].lastMs = Math.round(durMs);
  s[key].lastAt = Date.now();
  if (errMsg) s[key].lastErr = errMsg;
  state.phaseDebug.lastSlowReq = {
    path, dur: Math.round(durMs), at: Date.now(), err: errMsg
  };
}

/* 连接判定：轮询/探测硬失败达阈值才离线（瞬时错误不计）。 */
function noteHardSuccess() { state.failCount = 0; }

function noteHardFailure(e, source) {
  if (e && e.transient) return;   /* 500/503/超时等瞬时错误不计入 */
  state.failCount++;
  if (state.failCount >= FAIL_THRESHOLD && state.connected) {
    enterDisconnected(e && e.message, source);
  }
}

function enterDisconnected(msg, source) {
  if (state.probing) return;
  const wasConnected = state.connected;
  state.connected = false;
  state.connecting = false;
  state.probing = true;
  state.failCount = 0;
  state.offlineSince = nowMs();
  stopRepeat(true);
  stopDataPolling();
  updateConnUi();
  $("#netBanner").classList.add("show");
  $("#netBannerText").textContent =
    "连接中断" + (source ? " (" + source + ")" : "") +
    " — 使用主页共享 IP (" + state.ip + ") 自动重连中…";
  if (wasConnected) {
    connToast("网络异常：" + (msg || "请求失败") + " — 已停止周期发送，自动重连中", "err");
  }
  startProbe();
}

function startProbe() {
  stopTimer("probe");
  state.probing = true;
  if (!state.offlineSince) state.offlineSince = nowMs();
  scheduleProbe(0);
}
function stopProbe() {
  stopTimer("probe");
  state.probing = false;
}
function scheduleProbe(delayMs) {
  stopTimer("probe");
  state.poll.probe = setTimeout(function () {
    state.poll.probe = null;
    probeOnce();
  }, delayMs == null ? PROBE_INTERVAL_MS : delayMs);
}

async function probeOnce() {
  if (!state.probing) return;
  state.ip = getSharedDeviceIp();
  const label = $("#ipLabel");
  if (label) label.textContent = "IP " + state.ip;
  try {
    const data = await fetchDeviceJson("/system/status", 2500);
    const offlineMs = state.offlineSince ? (nowMs() - state.offlineSince) : 0;
    stopProbe();
    state.connected = true;
    state.connecting = false;
    state.failCount = 0;
    state.offlineSince = 0;
    $("#netBanner").classList.remove("show");
    applySystemStatus(data);
    updateConnUi();
    /* 短离线恢复不弹 toast，避免毛刺「闪一下」 */
    if (offlineMs >= RECOVER_TOAST_MIN_OFFLINE_MS) {
      connToast("连接已恢复 — " + state.ip, "ok");
    }
    startPolling();
  } catch (e) {
    if (state.probing) scheduleProbe(PROBE_INTERVAL_MS);
  }
}

/**
 * 使用主页共享 IP 自动连接（本页无连接按钮）。
 * 失败不阻断页面，进入探测直到 /system/status 可用。
 */
function connectShared() {
  state.ip = getSharedDeviceIp();
  state.connecting = true;
  state.probing = false;
  state.failCount = 0;
  state.offlineSince = nowMs();
  state.ch = parseInt($("#chSelect").value, 10) || 0;
  updateConnUi();
  $("#netBanner").classList.add("show");
  $("#netBannerText").textContent =
    "自动连接 " + state.ip + " … 本页与 OTA 主页共用 IP";
  /* HTTP 探测驱动连接：/system/status 可用 → startPolling */
  startProbe();
}

function applySystemStatus(data) {
  const wifi = (data && data.wifi) || {};
  if (wifi.ip) setSharedDeviceIp(wifi.ip);
  let parts = [];
  if (data && data.gd32_features) {
    const f = data.gd32_features;
    if (f.known) parts.push("feat 0x" + (f.flags >>> 0).toString(16).toUpperCase());
  }
  if (data && data.app_version) parts.push("APP " + data.app_version);
  if (data && data.boot_version) parts.push("BOOT " + data.boot_version);
  if (wifi.mode) parts.unshift(wifi.mode + (wifi.connected ? " 在线" : " 离线"));
  const el = $("#devStatus");
  if (el) el.textContent = parts.length ? parts.join(" · ") : "在线";
}

function updateConnUi() {
  const led = $("#connLed");
  const ipLabel = $("#ipLabel");
  if (ipLabel) ipLabel.textContent = "IP " + state.ip;
  /* 连接灯只反映稳定状态：在线绿 / 探测黄 / 离线红，中间失败不闪黄 */
  if (state.connecting || state.probing) {
    if (led) led.className = "led led-warn led-pulse";
    if (!state.connected) {
      const ds = $("#devStatus");
      if (ds) ds.textContent = "探测中…";
    }
  } else if (state.connected) {
    if (led) led.className = "led led-on";
  } else {
    if (led) led.className = "led led-err";
    const ds = $("#devStatus");
    if (ds) ds.textContent = "未连接";
  }
  const sendHint = $("#sendHint");
  if (sendHint) sendHint.textContent = "POST /can/send_batch \u00b7 CH" + state.ch;
  const sbCh = $("#sbCh");
  if (sbCh) sbCh.textContent = "CH" + state.ch;
}

/* ═══════════════════════ 轮询调度（单飞递归，永不并发） ═══════════════════════ */
function stopTimer(key) {
  if (state.poll[key]) {
    clearTimeout(state.poll[key]);
    clearInterval(state.poll[key]);
    state.poll[key] = null;
  }
}
/**
 * 双链路轮询：motor 和 feedback 各自独立定时器，互不阻塞。
 *
 * 历史问题：原来 motor → feedback 串行，任一挂起（ESP32 端 mutex 阻塞 /
 * WiFi 抖动）会拖累另一个，表现为数据卡顿。
 * 现在：motor 和 feedback 各走独立固定节拍定时器，
 *   - motor 挂起 3s 超时期间，feedback 仍正常刷
 *   - feedback 挂起不影响 motor
 * 两者共用 ESP32 端 httpJsonWorkMutex，ESP32 串行处理即可。
 */
function stopDataPolling() {
  state.pollGen += 1;
  stopTimer("tick");
  stopTimer("feedback");
  stopTimer("status");
  state.tickInFlight = false;
  state.feedbackInFlight = false;
}

/**
 * 启动数据链路：HTTP 三链（motor tick / feedback 1Hz / status 1Hz）立即开车。
 */
function startPolling() {
  stopDataPolling();           /* 递增 pollGen，清残留定时器/在途链 */
  state.failCount = 0;
  state.tickN = 0;
  state.statusTickN = 0;
  const gen = state.pollGen;
  schedulePollLoop(gen, 0);
  scheduleFeedbackPoll(gen, 0);
  scheduleStatusPoll(gen, 1000);
  /* 连上后把持久化的刷新速度同步给 GD32，使 0xC5 推送周期与本地轮询一致（无需用户重新拨动） */
  sendMotorPeriod(state.refreshIntervalMs || 250);
}

function scheduleStatusPoll(gen, delayMs) {
  if (gen !== state.pollGen) return;
  stopTimer("status");
  state.poll.status = setTimeout(function () {
    state.poll.status = null;
    if (gen !== state.pollGen || !state.connected || state.probing) return;
    /* lite/full 交替：4 次 lite 后 1 次 full，循环。
     * lite ~150B 不调重活；full ~3KB 含 OTA/heap/SPI，5s 一次。 */
    const wantFull = (state.statusTickN % 5) === 0;
    state.statusTickN = (state.statusTickN || 0) + 1;
    fetchSystemStatus(gen, wantFull).finally(() => {
      if (gen === state.pollGen && state.connected && !state.probing) {
        scheduleStatusPoll(gen, 1000);
      }
    });
  }, delayMs == null ? 1000 : delayMs);
}
/* feedback 独立定时器：固定 1000ms（与 GD32 0xC1 CAN 反馈 1Hz 推送对齐）。
 * feedback 是 CAN 错误计数/总线状态快照，GD32 1Hz 推送，前端 1Hz 拉即可，
 * 加快只会拿到陈旧数据还跟 motor 抢 ESP32 端 mutex。
 * 与 motor 解耦后，motor 挂起不拖 feedback，feedback 挂起不拖 motor。 */
function scheduleFeedbackPoll(gen, delayMs) {
  if (gen !== state.pollGen) return;
  stopTimer("feedback");
  state.poll.feedback = setTimeout(function () {
    state.poll.feedback = null;
    if (gen !== state.pollGen || !state.connected || state.probing) return;
    if (state.feedbackInFlight) {
      scheduleFeedbackPoll(gen, 1000);
      return;
    }
    state.feedbackInFlight = true;
    fetchFeedback(gen).finally(() => {
      state.feedbackInFlight = false;
      if (gen === state.pollGen && state.connected && !state.probing) {
        scheduleFeedbackPoll(gen, 1000);
      }
    });
  }, delayMs == null ? 1000 : delayMs);
}
function schedulePollLoop(gen, delayMs) {
  if (gen !== state.pollGen) return;
  stopTimer("tick");
  const delay = delayMs != null ? delayMs : (state.refreshIntervalMs || TICK_INTERVAL_MS);
  state.poll.tick = setTimeout(function () {
    state.poll.tick = null;
    pollLoop(gen);
  }, delay);
}

/**
 * motor 轮询主链：只跑 fetchMotorState，feedback/status 已剥离独立定时器。
 * motor 挂起 3s 超时期间，feedback 仍正常刷 CAN 统计（不再被 motor 拖）。
 */
async function pollLoop(gen) {
  if (gen !== state.pollGen || !state.connected || state.probing) return;
  if (state.tickInFlight) {
    schedulePollLoop(gen);
    return;
  }
  state.tickInFlight = true;
  try {
    await fetchMotorState(gen);
    if (gen !== state.pollGen || !state.connected || state.probing) return;
    state.tickN += 1;
  } finally {
    state.tickInFlight = false;
    if (gen === state.pollGen && state.connected && !state.probing) {
      schedulePollLoop(gen);
    }
  }
}

async function fetchSystemStatus(gen, wantFull) {
  if (gen !== state.pollGen || !state.connected || state.probing) return;
  try {
    /* wantFull=false（默认）走轻量路径 ?lite=1：~150B，
     * 不调 ESP32 端 heap_caps_get_info / 任务栈遍历 / OTA snapshot，
     * 避免 httpd 单线程被重 handler 阻塞几十~上百 ms。
     * wantFull=true 走完整路径，5s 一次，含 OTA/heap/SPI。 */
    const path = wantFull ? "/system/status" : "/system/status?lite=1";
    const data = await fetchDeviceJson(path, wantFull ? 3500 : 1500);
    if (gen !== state.pollGen) return;
    applySystemStatus(data);
    noteHardSuccess();
  } catch (e) {
    if (gen !== state.pollGen) return;
    noteHardFailure(e, "status");
  }
}

async function fetchMotorState(gen) {
  if (gen != null && gen !== state.pollGen) return false;
  if (!state.connected || state.probing) return false;
  try {
    /* 二进制模式：体积 ~408B vs JSON ~1200B，省 66%。
     * ESP32 端 memcpy 拼包，无 snprintf，CPU 开销降一个数量级。 */
    const data = await fetchMotorStateBinary();
    if (gen != null && gen !== state.pollGen) return false;
    onMotorState(data);
    noteHardSuccess();
    return true;
  } catch (e) {
    if (gen != null && gen !== state.pollGen) return false;
    noteHardFailure(e, "motor");
    return false;
  }
}

/* /motor/state?bin=1 二进制解析：协议见 ESP32 motor_state_handler。
 * 返回与原 JSON 等价的对象 {have_data, age_ms, active_count, motors[]}，
 * 让 onMotorState 不用改。data 字段用 32 字节 hex 字符串保持兼容（前端
 * hexToBytes 解析 STATUS 帧）。 */
async function fetchMotorStateBinary() {
  const t0 = performance.now();
  let resp;
  try {
    resp = await fetch(getBaseUrl() + "/motor/state?bin=1", {
      signal: AbortSignal.timeout(3000),
      cache: "no-store",
    });
  } catch (e) {
    const err = e instanceof Error ? e : new Error(String(e));
    err.transient = isTransientError(err);
    recordSlowReq("/motor/state?bin=1", performance.now() - t0, err.message);
    throw err;
  }
  recordSlowReq("/motor/state?bin=1", performance.now() - t0);
  if (!resp.ok) {
    const err = new Error("HTTP " + resp.status);
    err.status = resp.status;
    err.transient = (resp.status === 500 || resp.status === 503 || resp.status === 429);
    throw err;
  }
  const ab = await resp.arrayBuffer();
  return parseMotorStateBinary(ab);
}

/* 解析 ESP32 二进制 /motor/state?bin=1 响应。
 * Header 12B: magic u16 LE=0x4D53, ver u8, flags u8, age_ms u32 LE,
 *             active u8, count u8, rsvd u16
 * Entry 44B: node u8, ch u8, online u8, dlc u8,
 *             rx_count u32 LE, seq u32 LE, data[32]
 * 无效响应（magic 不匹配/长度不足）回退到 have_data:false。 */
function parseMotorStateBinary(ab) {
  const out = { have_data: false, active_count: 0, age_ms: null, boot_active: false, motors: [] };
  if (!ab || ab.byteLength < 12) return out;
  const dv = new DataView(ab);
  const magic = dv.getUint16(0, true);
  if (magic !== 0x4D53) return out;   /* 'MS' LE */
  const ver = dv.getUint8(2);
  if (ver !== 1) return out;
  const flags = dv.getUint8(3);       /* flags bit0 = boot_active */
  const age = dv.getUint32(4, true);
  const active = dv.getUint8(8);
  const count = dv.getUint8(9);
  /* rsvd u16 @10..11 */

  /* age == 0xFFFFFFFF 表示 ESP32 端无数据 */
  const have = (age !== 0xFFFFFFFF);
  out.have_data = have;
  out.active_count = active;
  out.age_ms = have ? age : null;
  out.boot_active = (flags & 0x01) !== 0;   /* 总线有电机在 BOOT（0x103 心跳） */

  const u8 = new Uint8Array(ab);
  let off = 12;
  for (let i = 0; i < count && off + 44 <= ab.byteLength; i++) {
    const node = u8[off];
    const ch = u8[off + 1];
    const online = u8[off + 2] !== 0;
    const dlc = u8[off + 3];
    const rx_count = dv.getUint32(off + 4, true);
    const seq = dv.getUint32(off + 8, true);
    /* data[32] @ off+12..off+43 转 hex 字符串（兼容 onMotorState 的 hexToBytes） */
    let hex = "";
    for (let b = 0; b < 32; b++) {
      hex += u8[off + 12 + b].toString(16).padStart(2, "0").toUpperCase();
    }
    out.motors.push({
      node, ch, online, dlc, rx_count, seq, data: hex,
    });
    off += 44;
  }
  return out;
}

async function fetchFeedback(gen) {
  if (gen != null && gen !== state.pollGen) return false;
  if (!state.connected || state.probing) return false;
  try {
    /* ?lite=1：跳过 entries 数组（~3KB → ~200B）。motor.html 只用 ch/stats 字段，
     * 不需要 CAN 帧快照（那是 index.html/can.html 监控页用的）。
     * ESP32 端 lite 路径不走 chunked + httpJsonWorkMutex，handler <1ms。 */
    const data = await fetchDeviceJson("/can/feedback?lite=1", 1500);
    if (gen != null && gen !== state.pollGen) return false;
    onFeedback(data);
    noteHardSuccess();
    return true;
  } catch (e) {
    if (gen != null && gen !== state.pollGen) return false;
    noteHardFailure(e, "feedback");
    return false;
  }
}

/* ═══════════════════════ /motor/state 处理 ═══════════════════════ */
/* 后端表已改 node×channel 双键（同节点号可同时在多通道在线），
 * motors[] 可能含同 node 不同 ch 的多条记录，decoded 缓存键也必须带通道。 */
function motorKey(m) { return m.ch + ":" + m.node; }

function onMotorState(data) {
  const motors = (data && data.motors) ? data.motors : [];
  state.motors = motors;

  /* 解码每个 节点×通道 的 STATUS 帧 */
  const decoded = new Map();
  for (const m of motors) {
    const bytes = hexToBytes(m.data);
    const d = decodeStatus(bytes);
    if (d) decoded.set(motorKey(m), d);
  }
  state.decoded = decoded;

  /* 按实际通道统计在线电机数：驱动通道下拉框信号标记 + 本页通道过滤视图 */
  const chOnline = [0, 0, 0];
  for (const m of motors) {
    if (m.online && m.node >= 1 && m.node <= 9 && m.ch >= 0 && m.ch <= 2) chOnline[m.ch]++;
  }
  state.chOnline = chOnline;
  updateChSelectOptions();

  /* 固定节点 1–9：始终可选（含离线），便于电机已进 BOOT / 无 STATUS 时仍能刷写。
   * 自动选择：当前选中已是 1–9 则保持；否则优先当前通道的在线节点，其次任意在线，否则默认 1。 */
  let autoSelected = false;
  if (state.selectedNode == null || state.selectedNode < 1 || state.selectedNode > 9) {
    const online = motors.find((m) => m.online && m.ch === state.ch && m.node >= 1 && m.node <= 9)
                || motors.find((m) => m.online && m.node >= 1 && m.node <= 9);
    state.selectedNode = online ? online.node : 1;
    autoSelected = true;
  }

  refreshChannelViews();
  updateNodeLabels();
  if (autoSelected) restoreVersionUi(state.selectedNode);  /* 自动切节点时回填缓存版本 */

  /* BOOT 徽章：总线上有电机在 Bootloader（0x103 心跳）时显示，独立于 N1-N9。
   * 0x103 固定 ID 不带节点号，故不归属任何 Nx，只作总线级 BOOT 提示。
   * 同时写 class + style，避免任何残留内联 display 把 .show 盖掉。 */
  const bootEl = $("#bootBadge");
  if (bootEl) {
    const on = !!(data && data.boot_active);
    bootEl.classList.toggle("show", on);
    bootEl.style.display = on ? "flex" : "none";
    bootEl.setAttribute("aria-hidden", on ? "false" : "true");
    const sub = $("#bootBadgeSub");
    if (sub) sub.textContent = on ? "0x103 心跳" : "0x103";
  }
}

/* ─── 节点列表渲染 ───
 * ★ 固定显示 N1–N9：即使离线/无 0xC5 推送也可见可选，方便电机在 BOOT 时刷写。
 * 性能：首次全量重建 DOM；之后只增量更新文本/class。 */
const FIXED_MOTOR_NODES = [1, 2, 3, 4, 5, 6, 7, 8, 9];
const _nodeCards = new Map();   // node -> { card, led, modeEl, stTxt, angleVal, velVal, curVal, faultsEl }

/** 把后端 motors[] 叠到固定 1–9 槽位；无数据的槽位记为 offline。
 * ★ node×channel 双键：同节点号可能在多个通道各有一条记录。
 *   优先级：当前通道在线 > 其他通道在线（otherCh 标注，如 "0/2"）>
 *   当前通道离线记录 > 空槽位。 */
function getDisplayMotors() {
  const motors = state.motors || [];
  return FIXED_MOTOR_NODES.map((node) => {
    const entries = motors.filter((m) => m && m.node === node);
    const cur = entries.find((m) => m.ch === state.ch);
    if (cur && cur.online) return cur;
    const others = entries.filter((m) => m.online && m.ch !== state.ch);
    if (others.length) {
      const chs = others.map((o) => o.ch).sort().join("/");
      return { node, ch: others[0].ch, online: false, otherCh: chs, dlc: 0, rx_count: 0, seq: 0, data: "" };
    }
    if (cur) return cur;
    return { node, ch: state.ch || 0, online: false, dlc: 0, rx_count: 0, seq: 0, data: "" };
  });
}

/** 通道切换或电机数据更新后，按当前通道刷新节点列表 / STATUS 面板 / 在线汇总 */
function refreshChannelViews() {
  renderNodeList();
  renderStatusPanel();
  const onlineCount = (state.motors || [])
    .filter((m) => m.online && m.ch === state.ch && m.node >= 1 && m.node <= 9).length;
  $("#nodeSummary").textContent = `${onlineCount}/9 在线 · CH${state.ch}`;
}

function renderNodeList() {
  const motors = getDisplayMotors();

  /* 固定 9 槽：仅首次或 DOM 被清空时全量重建 */
  let needRebuild = _nodeCards.size !== FIXED_MOTOR_NODES.length;
  if (!needRebuild) {
    for (const n of FIXED_MOTOR_NODES) {
      if (!_nodeCards.has(n)) { needRebuild = true; break; }
    }
  }
  if (needRebuild) {
    _nodeCards.clear();
    nodeListEl.innerHTML = motors.map(buildNodeCardHtml).join("");
    for (const m of motors) {
      const card = nodeListEl.querySelector(`.node-card[data-node="${m.node}"]`);
      if (!card) continue;
      _nodeCards.set(m.node, {
        card,
        led: card.querySelector(".led"),
        modeEl: card.querySelector(".nc-mode"),
        stTxt: card.querySelector(".nc-status-text"),
        angleVal: card.querySelector(".nc-reading:nth-child(1) .val"),
        velVal:  card.querySelector(".nc-reading:nth-child(2) .val"),
        curVal:  card.querySelector(".nc-reading:nth-child(3) .val"),
        faultsEl: card.querySelector(".nc-faults"),
      });
    }
    return;
  }

  /* 槽位稳定 → 增量更新文本/class */
  for (const m of motors) {
    const ref = _nodeCards.get(m.node);
    if (!ref) continue;
    /* otherCh 卡片不取解码数据：其 STATUS 属于其他通道，本通道视图不展示/不标故障 */
    const d = m.otherCh != null ? null : state.decoded.get(motorKey(m));
    const faultCount = faultCountOf(d);
    const faultLabel = faultShortLabel(d);
    const isSel = m.node === state.selectedNode;

    const cls = [
      "node-card",
      isSel ? "active" : "",
      faultCount > 0 ? "fault" : "",
      !m.online ? "offline" : "",
    ].join(" ").trim();
    if (ref.card.className !== cls) ref.card.className = cls;
    ref.card.setAttribute("aria-selected", String(isSel));
    if (faultLabel) ref.card.title = "故障: " + faultLabel;
    else if (m.otherCh != null) ref.card.title = `节点在 CH${m.otherCh} 在线，切换顶部通道查看`;
    else if (!m.online) ref.card.title = "离线也可选中刷写";
    else ref.card.title = "在线";

    const ledCls = m.otherCh != null ? "led-warn" : (m.online ? (faultCount > 0 ? "led-err" : "led-on") : "");
    const ledFull = "led " + ledCls;
    if (ref.led.className !== ledFull) ref.led.className = ledFull;

    const sentMode = state.lastSentMode.get(m.node);
    const mode = (sentMode != null && MODES[sentMode]) ? MODES[sentMode] : null;
    /* motStatus 是报错位(1=故障)，不是运行标志；无最近 CMD 时用 待机。
     * otherCh：节点在其他通道在线，标注实际通道供用户切换。 */
    const modeLabel = m.otherCh != null ? ("CH" + m.otherCh) : (!m.online ? "离线" : (mode ? mode.zh : "待机"));
    if (ref.modeEl.textContent !== modeLabel) ref.modeEl.textContent = modeLabel;
    const modeCls = m.otherCh != null ? "nc-mode mode-other-ch"
      : (!m.online ? "nc-mode mode-off" : (faultCount > 0 ? "nc-mode mode-fault" : "nc-mode"));
    if (ref.modeEl.className !== modeCls) ref.modeEl.className = modeCls;

    const stTxt = !m.online ? "--" : (faultLabel || "OK");
    if (ref.stTxt.textContent !== stTxt) ref.stTxt.textContent = stTxt;
    if (ref.stTxt) ref.stTxt.title = faultLabel ? ("故障: " + faultLabel) : (m.online ? "正常" : "");

    const angle = d && m.online ? d.rtAngle.toFixed(1) + "°" : "--";
    const vel   = d && m.online ? d.rtVelocity.toFixed(0) : "--";
    const cur   = d && m.online ? d.rtCurrent.toFixed(0) : "--";
    if (ref.angleVal.textContent !== angle) ref.angleVal.textContent = angle;
    if (ref.velVal.textContent !== vel) ref.velVal.textContent = vel;
    if (ref.curVal.textContent !== cur) ref.curVal.textContent = cur;

    if (ref.faultsEl) {
      const dots = ref.faultsEl.querySelectorAll(".fault-dot").length;
      if (dots !== faultCount) {
        ref.faultsEl.innerHTML = faultCount > 0 ? "<span class='fault-dot'></span>".repeat(faultCount) : "";
      }
    }
  }
}

/* 构建单节点卡片的 HTML（仅集合变化时调用） */
function buildNodeCardHtml(m) {
  /* otherCh 卡片不取解码数据（同 renderNodeList 增量路径） */
  const d = m.otherCh != null ? null : state.decoded.get(motorKey(m));
  const faultCount = faultCountOf(d);
  const faultLabel = faultShortLabel(d);
  const isSel = m.node === state.selectedNode;
  const cls = [
    "node-card",
    isSel ? "active" : "",
    faultCount > 0 ? "fault" : "",
    !m.online ? "offline" : "",
  ].join(" ").trim();
  const sentMode = state.lastSentMode.get(m.node);
  const mode = (sentMode != null && MODES[sentMode]) ? MODES[sentMode] : null;
  const modeLabel = m.otherCh != null ? ("CH" + m.otherCh) : (!m.online ? "离线" : (mode ? mode.zh : "待机"));
  const modeCls = m.otherCh != null ? "mode-other-ch" : (!m.online ? "mode-off" : (faultCount > 0 ? "mode-fault" : ""));
  const stTxt = !m.online ? "--" : (faultLabel || "OK");
  const cardTitle = faultLabel ? ("故障: " + faultLabel)
    : m.otherCh != null ? `节点在 CH${m.otherCh} 在线，切换顶部通道查看`
    : (m.online ? "在线" : "离线也可选中刷写");
  const angle = d && m.online ? d.rtAngle.toFixed(1) + "°" : "--";
  const vel = d && m.online ? d.rtVelocity.toFixed(0) : "--";
  const cur = d && m.online ? d.rtCurrent.toFixed(0) : "--";
  return `<div class="${cls}" role="option" tabindex="0" data-node="${m.node}" aria-selected="${isSel}" title="${cardTitle}">
    ${faultCount > 0 ? `<div class="nc-faults">${"<span class='fault-dot'></span>".repeat(faultCount)}</div>` : ""}
    <div class="nc-top">
      <span class="led ${m.otherCh != null ? "led-warn" : (m.online ? (faultCount > 0 ? "led-err" : "led-on") : "")}"></span>
      <span class="nc-id">N${m.node}</span>
      <span class="nc-mode ${modeCls}">${modeLabel}</span>
      <span class="nc-status-text" title="${faultLabel ? ("故障: " + faultLabel) : ""}">${stTxt}</span>
    </div>
    <div class="nc-readings">
      <div class="nc-reading"><span class="lbl">角度</span><span class="val">${angle}</span></div>
      <div class="nc-reading"><span class="lbl">速度</span><span class="val">${vel}</span></div>
      <div class="nc-reading"><span class="lbl">电流</span><span class="val">${cur}</span></div>
    </div>
  </div>`;
}
nodeListEl.addEventListener("click", (e) => {
  const card = e.target.closest(".node-card");
  if (!card) return;
  state.selectedNode = parseInt(card.dataset.node, 10);
  saveUiState();   /* 选中节点变化时持久化，刷新后仍选中此节点 */
  renderNodeList();
  updateNodeLabels();
  renderStatusPanel();
  restoreVersionUi(state.selectedNode);  /* 回填该节点缓存的版本号 */
});
nodeListEl.addEventListener("keydown", (e) => {
  if (e.key === "Enter") { const card = e.target.closest(".node-card"); if (card) card.click(); }
});

/* ─── 节点相关标签更新 ─── */
function updateNodeLabels() {
  const n = state.selectedNode;
  if (n == null) {
    $("#nodeBadge").textContent = "NODE -";
    $("#cmdIdLabel").textContent = "ID 0x000-0200 · DLC 32";
    $("#statusIdLabel").textContent = "0x000-0100 · 1 Hz";
    document.querySelectorAll(".cfgNodeNibble").forEach((el) => (el.textContent = "-"));
    return;
  }
  const hex = nodeHex(n);
  $("#nodeBadge").textContent = `NODE ${n}`;
  $("#cmdIdLabel").textContent = `ID ${hexId(ID_CMD(n))} · DLC 32`;
  $("#statusIdLabel").textContent = `${hexId(ID_STATUS(n))} · 1 Hz`;
  document.querySelectorAll(".cfgNodeNibble").forEach((el) => (el.textContent = hex));
}

/* ═══════════════════════ STATUS 面板渲染 ═══════════════════════ */
function flashEl(el) {
  if (!el) return;
  el.classList.add("flash");
  setTimeout(() => el.classList.remove("flash"), 350);
}

function renderStatusPanel() {
  const n = state.selectedNode;
  /* ★ node×channel 双键：只展示当前通道的记录；节点在其他通道在线时给出切换提示 */
  const entries = n != null ? state.motors.filter((m) => m.node === n) : [];
  const cur = entries.find((m) => m.ch === state.ch);
  const others = entries.filter((m) => m.online && m.ch !== state.ch);
  const onOtherCh = !(cur && cur.online) && others.length > 0;
  const motor = onOtherCh ? null : cur;
  const d = (motor && motor.online) ? state.decoded.get(motorKey(motor)) : null;
  const offline = !motor || !motor.online || !d;

  $("#offlineOverlay").classList.toggle("show", offline);
  $("#statusLive").classList.toggle("hide", offline);
  if (offline) {
    if (!state.connected) {
      $("#offlineTitle").textContent = "设备未连接";
      $("#offlineSub").textContent = "本页共用 OTA 主页 IP，请到主页配置并测试连接";
    } else if (onOtherCh) {
      const chs = others.map((o) => "CH" + o.ch).sort().join(" / ");
      $("#offlineTitle").textContent = `节点 ${n} 在 ${chs} 有信号`;
      $("#offlineSub").textContent = `当前通道 CH${state.ch} 无此节点，请切换顶部通道查看`;
    } else if (!motor || !motor.online) {
      $("#offlineTitle").textContent = n != null ? `节点 ${n} 离线（仍可刷写）` : "节点离线（仍可刷写）";
      $("#offlineSub").textContent = "无 STATUS 属正常（电机在 BOOT / 未上电）。左侧 N1–N9 可直接选中刷写";
    } else {
      $("#offlineTitle").textContent = `节点 ${n} 暂无解码数据`;
      $("#offlineSub").textContent = "等待 STATUS 帧；OTA 刷写不受影响";
    }
    return;
  }

  $("#rtAngle").textContent = d.rtAngle.toFixed(2);
  $("#rtVelocity").textContent = d.rtVelocity.toFixed(1);
  $("#rtCurrent").textContent = d.rtCurrent.toFixed(0);
  $("#mcuTemp").textContent = d.mcuTemp.toFixed(2) + " °C";
  $("#motTemp").textContent = d.motTemp.toFixed(2) + " °C";

  const mcuPct = clampNum(d.mcuTemp, 0, 100);
  const motPct = clampNum(d.motTemp, 0, 100);
  $("#mcuTempBar").style.width = mcuPct + "%";
  $("#mcuTempBar").className = "temp-fill" + (d.mcuTemp > 70 ? " hot" : d.mcuTemp > 50 ? " warm" : "");
  $("#motTempBar").style.width = motPct + "%";
  $("#motTempBar").className = "temp-fill" + (d.motTemp > 70 ? " hot" : d.motTemp > 50 ? " warm" : "");

  $("#valU").textContent = `${d.uCurrent >= 0 ? "+" : ""}${d.uCurrent}`;
  $("#valV").textContent = `${d.vCurrent >= 0 ? "+" : ""}${d.vCurrent}`;
  $("#valW").textContent = `${d.wCurrent >= 0 ? "+" : ""}${d.wCurrent}`;

  $("#pwrVolt").textContent = d.powerVoltage.toFixed(3) + " V";
  $("#testFloat").textContent = d.testFloat.toFixed(3);
  $("#testU16").textContent = String(d.testU16);
  $("#cpuPct").textContent = Math.round(d.cpuUsage) + "%";
  $("#cpuSub").textContent = `seq ${motor.seq} · rx ${motor.rx_count}`;
  const circumference = 131.9;
  $("#cpuRingFg").style.strokeDashoffset = (circumference * (1 - clampNum(d.cpuUsage, 0, 100) / 100)).toFixed(1);

  /* 状态标志位 */
  const setFlag = (itemId, ledId, stateId, active, isFault, okText, faultText) => {
    $("#" + itemId).className = "flag-item " + (active ? (isFault ? "is-fault" : "is-ok") : "");
    $("#" + ledId).className = "flag-led" + (active ? (isFault ? " on-err" : " on-ok") : "");
    $("#" + stateId).textContent = active ? (isFault ? faultText : okText) : "—";
  };
  /* motStatus：DBC 0=正常 / 1=报错（不是使能位） */
  setFlag("flagItemMot", "flagMot", "flagMotState", true, d.motStatus === 1, "NORMAL", "FAULT");
  setFlag("flagItemMag", "flagMag", "flagMagState", true, d.magStatus === 1, "NORMAL", "FAULT");
  setFlag("flagItemThermal", "flagThermal", "flagThermalState", true, d.thermalStatus === 1, "NORMAL", "过温!");
  setFlag("flagItemEeprom", "flagEeprom", "flagEepromState", true, d.eepromStatus !== 0, "NORMAL", "ERR=" + d.eepromStatus);

  flashEl($("#rtAngle")); flashEl($("#rtVelocity")); flashEl($("#rtCurrent"));
}

/* ═══════════════════════ /can/feedback 处理（状态栏） ═══════════════════════ */
function busStateText(bs) {
  if (bs == null) return "--";
  if (bs === 0) return "ACTIVE";
  const parts = [];
  if (bs & 1) parts.push("INIT");
  if (bs & 2) parts.push("SLEEP");
  if (bs & 4) parts.push("ERR_PASSIVE");
  if (bs & 8) parts.push("BUS_OFF");
  return parts.length ? parts.join("|") : ("0x" + bs.toString(16));
}

/** 根据通道启用/信号状态更新下拉框选项：
 *  - 禁用通道贴 "(禁用)" 标签并置灰
 *  - 有信号通道（存在在线电机）贴 "●N" 标记并着色 */
function updateChSelectOptions() {
  const sel = $("#chSelect");
  if (!sel) return;
  for (let i = 0; i < sel.options.length; i++) {
    const opt = sel.options[i];
    const en = state.chEnabled[i];
    const cnt = state.chOnline[i] || 0;
    const base = "CH" + i;
    opt.textContent = !en ? base + " (禁用)" : (cnt > 0 ? `${base} ●${cnt}` : base);
    opt.style.color = !en ? "var(--fg-dim,#666)" : (cnt > 0 ? "var(--ok,#3ddc97)" : "");
  }
  /* 收起态的选框文本用的是 select 自身颜色（option 内联色仅展开时生效），
   * 同步按当前选中通道的状态着色，避免选中有信号通道后绿色变回默认白 */
  const curEn = state.chEnabled[state.ch];
  const curCnt = state.chOnline[state.ch] || 0;
  sel.style.color = !curEn ? "var(--fg-dim,#666)" : (curCnt > 0 ? "var(--ok,#3ddc97)" : "");
}

function onFeedback(data) {
  const channels = (data && data.channels) || [];
  /* 更新各通道启用状态，禁用通道在下拉框标灰 + 阻止发送 */
  for (let i = 0; i < 3; i++) {
    const c = channels.find((x) => x.ch === i) || channels[i];
    state.chEnabled[i] = !c || c.enabled === undefined || c.enabled === null || !!c.enabled;
  }
  updateChSelectOptions();
  const ch = channels.find((c) => c.ch === state.ch) || channels[state.ch] || {};
  const valid = ch.valid && ch.enabled !== false;
  $("#sbRx").textContent = "RX " + (valid && ch.rx != null ? ch.rx.toLocaleString() : "--");
  $("#sbTx").textContent = valid && ch.tx != null ? ch.tx.toLocaleString() : "--";
  $("#sbErr").textContent = valid && ch.err != null ? ch.err.toLocaleString() : "--";
  $("#sbTec").textContent = valid && ch.tec != null ? ch.tec : "--";
  $("#sbRec").textContent = valid && ch.rec != null ? ch.rec : "--";
  const bsText = valid ? busStateText(ch.bus_state) : "--";
  const busEl = $("#sbBus");
  busEl.textContent = bsText;
  const isBusOff = valid && ch.bus_state != null && (ch.bus_state & 8);
  const isPassive = valid && ch.bus_state != null && (ch.bus_state & 4);
  busEl.style.color = isBusOff ? "var(--danger)" : isPassive ? "var(--warn)" : (valid ? "var(--ok)" : "var(--fg-muted)");
  const sbLed = $("#sbLed");
  sbLed.className = "led " + (isBusOff ? "led-err" : (valid ? "led-on" : ""));
}

/* ═══════════════════════ 控制模式联动 ═══════════════════════ */
const modeSelect = $("#modeSelect");
function applyModeVisibility() {
  const mode = parseInt(modeSelect.value, 10);
  const targets = MODES[mode].targets;
  $("#fldCurrent").classList.toggle("target-hidden", !targets.includes("current"));
  $("#fldVelocity").classList.toggle("target-hidden", !targets.includes("velocity"));
  $("#fldAngle").classList.toggle("target-hidden", !targets.includes("angle"));
  $("#modeDesc").textContent = MODE_DESCS[mode];
}
modeSelect.addEventListener("change", applyModeVisibility);

/* ═══════════════════════ 发送 CMD ═══════════════════════ */
function requireNode() {
  if (state.selectedNode == null) { toast("请先在左侧选择一个节点", "warn"); return false; }
  return true;
}
function inBusCooldown() {
  if (nowMs() < state.busCooldownUntil) {
    toast("总线冷却中，请稍候…", "warn");
    return true;
  }
  return false;
}

async function doSendCmd(silent) {
  if (!state.connected) { if (!silent) toast("设备未连接", "err"); return; }
  if (!state.chEnabled[state.ch]) { if (!silent) toast("CH" + state.ch + " 已禁用（GD32 固件未启用此通道）", "err"); return; }
  if (!requireNode()) return;
  if (inBusCooldown()) return;
  const node = state.selectedNode;
  const bytes = buildCmdBytes();
  const id = ID_CMD(node);
  const dataHex = bytesToHexStr(bytes);
  const mode = parseInt($("#modeSelect").value, 10) || 0;
  const time = new Date().toLocaleTimeString("zh-CN", { hour12: false });

  if (state.sendInFlight) {
    /* latest-wins 合并：上一包仍在途 → 新帧覆盖合并槽（不排队、不丢弃），
     * sendBatch 的 finally 排空时立即发出最新帧，周期指令始终收敛到最新值 */
    state.pendingCmd = { id: id, dlc: 32, data: dataHex };
    $("#sbLastSend").textContent = `最近发送: CMD → ${hexId(id)} [32B] 已合并（上一包在途） ${time}`;
    return;
  }

  $("#sbLastSend").textContent = `最近发送: CMD → ${hexId(id)} [32B] ${bytesToHexSpaced(bytes, 8)} … ${time}`;
  const res = await sendOneFrame(id, 32, dataHex, { silent: !!silent });
  if (res.ok) {
    state.lastSentMode.set(node, mode);
    state.txCount++;
    renderNodeList();
    if (!silent) {
      toast(`CMD → Node ${node} · ${MODES[mode].zh} 发送成功`, "ok");
    } else if (state.pendingCmd) {
      /* 周期模式：仅在合并发生时轻提示，让用户知道发送速率跟不上周期 */
      $("#sbLastSend").textContent = `最近发送: CMD → ${hexId(id)} [32B] 周期发送中（部分帧已合并） ${time}`;
    }
  } else if (!res.busy && !res.networkError && !silent) {
    toast(`CMD 发送失败: ${res.desc}`, "err");
  }
}
$("#sendCmdBtn").addEventListener("click", () => doSendCmd(false));

/* ═══════════════════════ 周期发送（最小 80ms） ═══════════════════════ */
let repeatTimer = null;
const repeatBtn = $("#sendRepeatBtn");
const periodInput = $("#repeatPeriod");
function getPeriod() {
  return clampNum(parseInt(periodInput.value, 10) || 100, MIN_CMD_PERIOD_MS, 10000);
}
function startRepeat() {
  if (!state.connected) { toast("设备未连接", "err"); return; }
  if (!requireNode()) return;
  const period = getPeriod();
  periodInput.value = period;
  stopTimer("repeat");
  repeatTimer = setInterval(() => doSendCmd(true), period);
  state.poll.repeat = repeatTimer;
  repeatBtn.textContent = "停止周期"; repeatBtn.classList.add("btn-primary");
  const hz = 1000 / period;
  toast(`周期发送已启动 · ${period} ms（${hz >= 10 ? hz.toFixed(0) : hz.toFixed(1)} Hz）`, "ok");
}
function stopRepeat(silent) {
  if (repeatTimer) { clearInterval(repeatTimer); repeatTimer = null; state.poll.repeat = null; }
  state.pendingCmd = null;   /* 丢弃合并槽残留帧，避免停止后仍发出 */
  repeatBtn.textContent = "周期发送"; repeatBtn.classList.remove("btn-primary");
  if (!silent) toast("周期发送已停止", "warn");
}
repeatBtn.addEventListener("click", () => (repeatTimer ? stopRepeat() : startRepeat()));
periodInput.addEventListener("change", () => {
  const p = getPeriod(); periodInput.value = p;
  if (repeatTimer) { stopRepeat(true); startRepeat(); }
});

/* ═══════════════════════ 配置操作（CMD_CONFIG，DLC 24） ═══════════════════════ */
const CONFIG_ACTIONS = {
  motReset:   { title: "电机复位", bit: 0,  desc: "将向节点发送 motReset 脉冲，电机立即停止并清除故障标志。运行中的电机将失去力矩输出，请确认机械安全。" },
  saveConfig: { title: "保存配置", bit: 2,  desc: "将当前控制模式和 PID 参数写入 EEPROM。写入期间约 50ms 不可发送新指令。" },
  angleReset: { title: "角度置零", bit: 3,  desc: "将当前机械位置设为电气零点。置零后目标角度将以新零点为参考，请确认电机处于期望的零位。" },
  sleep:      { title: "休眠",     bit: 56, desc: "电机进入低功耗休眠模式，关闭驱动输出。需发送 CANFD_BOOT 或重新上电才能唤醒。" },
};
/* 构造 CMD_CONFIG 24 字节脉冲帧：仅置位对应 bit，其余为 0 */
function buildConfigBytes(bit) {
  const bytes = new Uint8Array(24);
  bytes[Math.floor(bit / 8)] = 1 << (bit % 8);
  return bytes;
}
document.querySelectorAll(".config-btn").forEach((btn) => {
  btn.addEventListener("click", async () => {
    const action = CONFIG_ACTIONS[btn.dataset.action];
    if (!action) return;
    if (!state.connected) { toast("设备未连接", "err"); return; }
    if (!requireNode()) return;
    if (inBusCooldown()) return;
    const ok = await showModal(action.title, `目标节点 ${state.selectedNode}。` + action.desc);
    if (!ok) return;
    const node = state.selectedNode;
    const id = ID_CMD_CONFIG(node);
    const bytes = buildConfigBytes(action.bit);
    const res = await sendOneFrameWhenFree(id, 24, bytesToHexStr(bytes));
    const time = new Date().toLocaleTimeString("zh-CN", { hour12: false });
    $("#sbLastSend").textContent = `最近发送: ${action.title} → ${hexId(id)} [24B] ${bytesToHexSpaced(bytes, 8)} … ${time}`;
    if (res.ok) {
      state.txCount++;
      toast(`${action.title} → Node ${node} 已发送 (CMD_CONFIG)`, "ok");
    } else if (!res.networkError && !res.busy) {
      toast(`${action.title} 发送失败: ${res.desc}`, "err");
    }
  });
});

/* ═══════════════════════ 版本查询 ═══════════════════════
 * 协议：主机与电机共用 0x000N00FF（同 ID 应答，DLC 24）。
 * GD32 为解析核心：捕获电机应答缓存后经 0xC6 同步，前端读 /motor/version。
 * 载荷布局（DBC v0.71 = 电机 APP sendVersionInfoViaCan 实际发送）：
 * soft@0 hard@8 boot@16，每组 major|minor|patch|build 4×u16 LE，24B 无空洞。
 * 旧 v0.70 把字段误当 YYYY.MM.DD 日期解析（3 信号+2B 空洞），年份校验
 * 把 major=26 判非法 → 全部标 "(?)" 且丢 build 字段，已废弃。
 */
const VERSION_FULL_BYTES = 24; /* 3 组 × 4 字段 × u16，满帧 24B */

function readVersionQuadAt(dv, n, off) {
  if (off + 8 > n) return null;
  const major = dv.getUint16(off, true);
  const minor = dv.getUint16(off + 2, true);
  const patch = dv.getUint16(off + 4, true);
  const build = dv.getUint16(off + 6, true);
  /* 与电机 versionToString 同格式：major.minor.patch.build，不补零 */
  return `${major}.${minor}.${patch}.${build}`;
}

/**
 * 解析 0x00FF 载荷。以 hex 实际字节数为准（勿被 dlc 声明截短）。
 * 固定布局 soft@0 hard@8 boot@16，不再做双布局猜测。
 */
function parseVersionPayload(hex) {
  const rb = hexToBytes(hex);
  if (!rb || rb.length < 8) return null;
  const n = Math.min(rb.length, 24);
  const copy = new Uint8Array(24);
  copy.set(rb.subarray(0, n));
  const dv = new DataView(copy.buffer);

  const soft = readVersionQuadAt(dv, n, 0);
  const hard = readVersionQuadAt(dv, n, 8);
  const boot = readVersionQuadAt(dv, n, 16);
  return {
    soft,
    hard,
    boot,
    rawHex: bytesToHexStr(copy.subarray(0, n)),
    byteLen: n,
    complete: n >= VERSION_FULL_BYTES && !!(soft && hard && boot),
  };
}

function setVer(id, text) {
  const el = $("#" + id);
  if (!el) return;
  if (text) {
    el.textContent = text;
    el.classList.remove("ver-empty");
  } else {
    el.textContent = "--";
    el.classList.add("ver-empty");
  }
}

/** 回填节点缓存的版本号到版本卡片（切节点/刷新后调用）。
 *  未缓存时显示 "--"，不覆盖正在查询中的状态。 */
function restoreVersionUi(node) {
  if (node == null) {
    setVer("verSoft", null);
    setVer("verHard", null);
    setVer("verBoot", null);
    return;
  }
  const v = state.versions.get(node);
  if (!v) {
    setVer("verSoft", null);
    setVer("verHard", null);
    setVer("verBoot", null);
    return;
  }
  /* 只在卡片仍为初始 "--" 或已有缓存值时回填，不覆盖"查询中…/失败/无回报"等瞬时态 */
  const softEl = $("#verSoft"), hardEl = $("#verHard"), bootEl = $("#verBoot");
  const isStale = (el) => !el || el.textContent === "--" || el.textContent.includes(".");
  if (isStale(softEl)) setVer("verSoft", v.soft || null);
  if (isStale(hardEl)) setVer("verHard", v.hard || null);
  if (isStale(bootEl)) setVer("verBoot", v.boot || null);
}

/* ── 版本查询：GD32 为解析核心，电机 0x00FF 应答由 GD32 缓存后经 0xC6 推送，
 *    ESP32 以 /motor/version 提供缓存。前端发请求后轮询 /motor/version，
 *    按 seq 增长判定是否为本次查询的新应答（不再从 /can/feedback 取原始帧）。 ── */
async function fetchMotorVersionNode(node) {
  try {
    const data = await fetchDeviceJson("/motor/version", 2000);
    if (!data || !data.have_data || !Array.isArray(data.nodes)) return null;
    return data.nodes.find((n) => n.node === node) || null;
  } catch (e) {
    return null; /* 瞬时 busy 忽略 */
  }
}

/** 轮询直到 node 的版本 seq 超过 prevSeq（新应答）；超时返回 null。 */
async function waitMotorVersion(node, prevSeq, timeoutMs) {
  const deadline = nowMs() + (timeoutMs || 5000);
  while (nowMs() < deadline) {
    const v = await fetchMotorVersionNode(node);
    if (v && (prevSeq == null || v.seq > prevSeq)) return v;
    await sleep(200);
  }
  return null;
}

$("#queryVerBtn").addEventListener("click", async () => {
  if (!state.connected) { toast("设备未连接", "err"); return; }
  if (!requireNode()) return;
  if (inBusCooldown()) return;
  const node = state.selectedNode;
  const reqId = ID_GET_VERSION_REQ(node); /* 0x000N00FF 请求（与应答同 ID） */
  const bytes = new Uint8Array(24);

  ["verSoft", "verHard", "verBoot"].forEach((k) => {
    const el = $("#" + k);
    if (el) { el.textContent = "查询中…"; el.classList.add("ver-empty"); }
  });

  /* 记录查询前该节点的版本 seq，用于判定电机是否给出新应答 */
  const before = await fetchMotorVersionNode(node);
  const prevSeq = before ? before.seq : null;

  const res = await sendOneFrameWhenFree(reqId, 24, bytesToHexStr(bytes));
  const time = new Date().toLocaleTimeString("zh-CN", { hour12: false });
  $("#sbLastSend").textContent =
    `最近发送: GET_VERSION 请求 ${hexId(reqId)} → 等 GD32 0xC6 同步 … ${time}`;

  if (!res.ok) {
    if (!res.networkError && !res.busy) {
      ["verSoft", "verHard", "verBoot"].forEach((k) => {
        const el = $("#" + k);
        if (el) { el.textContent = "失败"; el.classList.add("ver-empty"); }
      });
      toast(`GET_VERSION 发送失败: ${res.desc}`, "err");
    }
    return;
  }
  state.txCount++;

  const v = await waitMotorVersion(node, prevSeq, 5000);
  if (v && v.version) {
    const parsed = parseVersionPayload(v.version);
    if (parsed && (parsed.soft || parsed.hard || parsed.boot)) {
      setVer("verSoft", parsed.soft || "--");
      setVer("verHard", parsed.hard || "--");
      setVer("verBoot", parsed.boot || "--");
      /* 缓存到 state.versions 并持久化，刷新/切节点后回填 */
      state.versions.set(node, { soft: parsed.soft || null, hard: parsed.hard || null, boot: parsed.boot || null });
      saveUiState();
      const bl = parsed.byteLen || 0;
      const full = parsed.complete;
      toast(
        full
          ? `版本已更新 · Node ${node} · ${bl}B · seq ${v.seq}`
          : `版本部分更新 · 仅 ${bl}B（需≥${VERSION_FULL_BYTES}B 才有 hard/boot）`,
        full ? "ok" : "warn"
      );
      return;
    }
  }
  ["verSoft", "verHard", "verBoot"].forEach((k) => {
    const el = $("#" + k);
    if (el) { el.textContent = "无回报"; el.classList.add("ver-empty"); }
  });
  toast(
    `已发 ${hexId(reqId)}，5s 内未收到电机新版本的 0xC6 同步`,
    "warn"
  );
});

/* ═══════════════════════ 节点 ID（DBC：SET 0x000N0400 / GET 0x000N0401，DLC 2） ═══════════════════════ */
$("#setNodeIdBtn").addEventListener("click", async () => {
  if (!state.connected) { toast("设备未连接", "err"); return; }
  if (!requireNode()) return;
  if (inBusCooldown()) return;
  const newId = parseInt($("#nodeIdInput").value, 10);
  if (!Number.isFinite(newId) || newId < 1 || newId > 9) { toast("Node ID 范围 1-9", "err"); return; }
  const cur = state.selectedNode;
  const ok = await showModal("设置节点 ID", `将节点 ${cur} 的地址修改为 ${newId}，CAN ID 前缀将变为 0x000${nodeHex(newId)}xxxx。修改立即生效并写入 EEPROM。`);
  if (!ok) return;
  /* CANFD_SET_NODE_ID：NodeID_Set 0|16@1+ = 新节点号 u16 LE */
  const bytes = new Uint8Array(2);
  bytes[0] = newId & 0xFF;
  bytes[1] = (newId >> 8) & 0xFF;
  const id = ID_SET_NODE_ID(cur);
  const res = await sendOneFrameWhenFree(id, 2, bytesToHexStr(bytes));
  const time = new Date().toLocaleTimeString("zh-CN", { hour12: false });
  $("#sbLastSend").textContent = `最近发送: SET_NODE_ID → ${hexId(id)} [2B] new=${newId} … ${time}`;
  if (res.ok) {
    state.txCount++;
    state.selectedNode = newId;
    $("#nodeIdResult").textContent = `设置成功: Node ID = ${newId} · 新 CMD ID: ${hexId(ID_CMD(newId))}`;
    toast(`节点 ID 已更新为 ${newId}`, "ok");
    updateNodeLabels();
  } else if (!res.networkError && !res.busy) {
    $("#nodeIdResult").textContent = `设置失败: ${res.desc}`;
    toast(`SET_NODE_ID 失败: ${res.desc}`, "err");
  }
});
$("#getNodeIdBtn").addEventListener("click", async () => {
  if (!state.connected) { toast("设备未连接", "err"); return; }
  if (!requireNode()) return;
  if (inBusCooldown()) return;
  const cur = state.selectedNode;
  /* CANFD_GET_NODE_ID 0x000N0401 DLC2：NodeID_Request 0|16@1+ */
  const bytes = new Uint8Array(2);
  bytes[0] = cur & 0xFF;
  bytes[1] = 0;
  const id = ID_GET_NODE_ID(cur);
  const res = await sendOneFrameWhenFree(id, 2, bytesToHexStr(bytes));
  const time = new Date().toLocaleTimeString("zh-CN", { hour12: false });
  $("#sbLastSend").textContent = `最近发送: GET_NODE_ID → ${hexId(id)} [2B] cur=${cur} … ${time}`;
  if (res.ok) {
    state.txCount++;
    const r = res.results && res.results[0];
    let reported = cur;
    if (r && r.recv) {
      const rb = hexToBytes(r.recv);
      if (rb.length >= 2) reported = rb[0] | (rb[1] << 8);
      else if (rb.length >= 1) reported = rb[0];
    }
    $("#nodeIdResult").textContent =
      `节点 ${cur} 回报 ID: ${reported} · ${hexId(id)}`;
    toast(`GET_NODE_ID → Node ${cur} 已读取: ${reported}`, "ok");
  } else if (!res.networkError && !res.busy) {
    $("#nodeIdResult").textContent = `读取失败: ${res.desc}`;
    toast(`GET_NODE_ID 失败: ${res.desc}`, "err");
  }
});

/* ═══════════════════════ Tab 切换 ═══════════════════════ */
document.querySelectorAll(".tab-btn").forEach((btn) => {
  btn.addEventListener("click", () => {
    document.querySelectorAll(".tab-btn").forEach((b) => b.classList.remove("active"));
    document.querySelectorAll(".tab-page").forEach((p) => p.classList.remove("active"));
    btn.classList.add("active");
    $("#tab-" + btn.dataset.tab).classList.add("active");
  });
});

/* ═══════════════════════ 电机 OTA（relay 模式）═══════════════════════
 * 两段式（与 BLE 刷 GD32 对齐）：
 *   步1 POST /motor/ota        收固件写 ESP32 flash（不碰 SPI，避免与 GD32 按铃竞态）
 *   步2 POST /motor/ota/relay   触发 SPI START + feedTask 喂 GD32 → GD32 本地 AES 握手 + CAN stop-and-wait
 * 前端负责：选文件 → 上传 → 进BOOT → 触发转发 → 轮询进度 → 显示结果。
 * ★ 顺序（2026-07-27 调整）：先收完固件再让电机进 BOOT——上传期间电机仍在 APP
 * 正常运行，上传失败/固件损坏时电机不会被白白踢进 BOOT 空等。 */
const OTA_MAX_FW = 0xC000;    // 48 KB 上限
const OTA_POLL_MS = 500;      // 进度轮询间隔
const OTA_TOTAL_TIMEOUT_MS = 120000;  // OTA 整体超时（含进 BOOT + 上传 + 传输），超时自动取消
const ota = {
  running: false,
  file: null,
  bytes: null,
  pollTimer: null,
  watchdogTimer: null,     /* 整体超时看门狗 */
  startTime: 0,            /* OTA 开始时间戳 */
  fileHandle: null,       /* FileSystemFileHandle（FSAPI），用于刷新后恢复固件 */
  pendingHandle: null,    /* 待重新授权的 handle（权限丢失时暂存，点上传区再授权） */
};

/* ─── 固件文件持久化（仿主页面 file-handle.js 模式）───
 * localStorage 存文件元数据（名/大小），IndexedDB 存 FileSystemFileHandle。
 * 刷新后：权限仍 granted → 静默恢复 bytes；否则展示文件名，点"选择固件"区重新授权。
 * File 对象本身不可序列化，bytes 不入 localStorage；IndexedDB 仅存 handle（可结构化克隆）。 */
const LS_OTA_FW_META = "dcu_motor_ota_fw_meta";
const IDB_OTA_NAME = "dcu_motor_ota_fw_v1";
const IDB_OTA_STORE = "file_handle";
let _otaIdbDb = null;

function otaIdbOpen() {
  if (_otaIdbDb) return Promise.resolve(_otaIdbDb);
  if (!window.indexedDB) return Promise.reject(new Error("no IDB"));
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(IDB_OTA_NAME, 1);
    req.onupgradeneeded = () => {
      if (!req.result.objectStoreNames.contains(IDB_OTA_STORE)) req.result.createObjectStore(IDB_OTA_STORE);
    };
    req.onsuccess = () => { _otaIdbDb = req.result; resolve(_otaIdbDb); };
    req.onerror = () => { _otaIdbDb = null; reject(req.error); };
  });
}
function otaIdbPutHandle(handle) {
  return otaIdbOpen().then((db) => new Promise((resolve, reject) => {
    const tx = db.transaction(IDB_OTA_STORE, "readwrite");
    if (handle) tx.objectStore(IDB_OTA_STORE).put(handle, "motor");
    else tx.objectStore(IDB_OTA_STORE).delete("motor");
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  })).catch(() => {});
}
function otaIdbGetHandle() {
  return otaIdbOpen().then((db) => new Promise((resolve, reject) => {
    const tx = db.transaction(IDB_OTA_STORE, "readonly");
    const r = tx.objectStore(IDB_OTA_STORE).get("motor");
    r.onsuccess = () => resolve(r.result || null);
    r.onerror = () => reject(r.error);
  })).catch(() => null);
}

function otaSaveFwMeta(file) {
  try {
    if (file && file.name) {
      localStorage.setItem(LS_OTA_FW_META, JSON.stringify({ name: file.name, size: file.size || 0, lastModified: file.lastModified || 0 }));
    } else {
      localStorage.removeItem(LS_OTA_FW_META);
    }
  } catch (_) {}
}
function otaLoadFwMeta() {
  try {
    const raw = localStorage.getItem(LS_OTA_FW_META);
    return raw ? JSON.parse(raw) : null;
  } catch (_) { return null; }
}

/** 查询 handle 权限（不 request，刷新无手势时 request 会失败） */
async function otaQueryPermission(handle) {
  if (!handle || typeof handle.getFile !== "function") return "denied";
  try {
    if (typeof handle.queryPermission === "function") return await handle.queryPermission({ mode: "read" });
    await handle.getFile();
    return "granted";
  } catch (_) { return "denied"; }
}

/** 把恢复的 File 应用到 ota 对象 + UI */
async function otaApplyRestoredFile(handle, file, opts) {
  opts = opts || {};
  ota.fileHandle = handle || null;
  ota.file = file;
  ota.bytes = new Uint8Array(await file.arrayBuffer());
  $("#otaFileName").textContent = `${file.name} — ${file.size.toLocaleString()} B`;
  $("#otaPktTotal").textContent = String(Math.ceil(file.size / 48));
  otaSaveFwMeta(file);
  if (!opts.silent) otaLog(`<span class="log-info">[固件]</span> ${file.name} · ${file.size.toLocaleString()} B · ${Math.ceil(file.size / 48)} 包`);
}

/** 无权限时展示文件名，提示点上传区重新授权 */
function otaShowPendingReauth(handle, meta) {
  ota.fileHandle = handle || null;
  ota.pendingHandle = handle || null;
  const name = (meta && meta.name) || (handle && handle.name) || "上次固件";
  $("#otaFileName").textContent = `${name} — 点击「选择固件」重新授权恢复`;
  if (meta && meta.size) $("#otaPktTotal").textContent = String(Math.ceil(meta.size / 48));
  otaLog(`<span class="log-info">[固件]</span> 上次固件「${name}」需点击「选择固件」重新授权（浏览器安全限制）`);
}

/** 页面加载后恢复上次固件（权限 granted → 静默恢复；否则提示重新授权） */
async function otaRestorePersistedFirmware() {
  let handle;
  try { handle = await otaIdbGetHandle(); } catch (_) { handle = null; }
  const meta = otaLoadFwMeta();
  if (!handle) {
    /* 无 handle（非 FSAPI 选择的旧文件或首次）：仅展示元数据提示 */
    if (meta && meta.name) {
      $("#otaFileName").textContent = `${meta.name} — ${meta.size ? meta.size.toLocaleString() + " B · " : ""}请重新选择`;
      otaLog(`<span class="log-info">[固件]</span> 上次固件「${meta.name}」需重新选择（无 FSAPI handle）`);
    }
    return;
  }
  const perm = await otaQueryPermission(handle);
  if (perm === "granted") {
    try {
      const file = await handle.getFile();
      await otaApplyRestoredFile(handle, file, { silent: true });
      otaLog(`<span class="log-ok">[固件]</span> 已恢复刷新前的固件: ${file.name}`);
    } catch (e) {
      otaShowPendingReauth(handle, meta);
    }
  } else {
    otaShowPendingReauth(handle, meta);
  }
}

/** 用户手势中：对 pending handle 重新授权并恢复（点"选择固件"时先尝试此路径） */
async function otaResumeWithGesture() {
  const handle = ota.pendingHandle;
  if (!handle || typeof handle.getFile !== "function") return false;
  try {
    let perm = "granted";
    if (typeof handle.queryPermission === "function") perm = await handle.queryPermission({ mode: "read" });
    if (perm !== "granted" && typeof handle.requestPermission === "function") perm = await handle.requestPermission({ mode: "read" });
    if (perm !== "granted") { toast("未授予读取权限，请在弹窗中允许或重新选择", "warn"); return false; }
    const file = await handle.getFile();
    if (file.size > OTA_MAX_FW) { toast(`固件过大 (${file.size} B)，上限 ${OTA_MAX_FW} B (48 KB)`, "err"); return false; }
    await otaApplyRestoredFile(handle, file, {});
    ota.pendingHandle = null;
    toast(`已恢复上次固件: ${file.name}`, "ok");
    return true;
  } catch (e) {
    otaLog(`<span class="log-err">[固件]</span> 恢复失败: ${e.message || e}`);
    return false;
  }
}

function otaLog(html) {
  const el = $("#otaLog");
  const div = document.createElement("div");
  div.innerHTML = html;
  el.appendChild(div);
  el.scrollTop = el.scrollHeight;
}

/* OTA 日志工具栏：清空 / 复制 */
$("#otaLogClearBtn").addEventListener("click", function () {
  const el = $("#otaLog");
  el.innerHTML = '<div class="log-info">[已清空] 日志已清除</div>';
});
$("#otaLogCopyBtn").addEventListener("click", function () {
  const el = $("#otaLog");
  const text = el.innerText.trim();
  if (!text) { toast("日志为空", "warn"); return; }
  const btn = this;
  function onDone() {
    btn.classList.add("copied");
    setTimeout(function () { btn.classList.remove("copied"); }, 1200);
    toast("已复制日志", "ok");
  }
  function fallback() {
    const ta = document.createElement("textarea");
    ta.value = text;
    ta.style.cssText = "position:fixed;left:-9999px;opacity:0";
    document.body.appendChild(ta);
    ta.select();
    try { document.execCommand("copy"); onDone(); } catch (_) {}
    document.body.removeChild(ta);
  }
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(text).then(onDone).catch(fallback);
  } else {
    fallback();
  }
});
function setOtaStage(stageIdx) {
  document.querySelectorAll(".ota-stage").forEach((el, i) => {
    el.classList.toggle("done", i < stageIdx);
    el.classList.toggle("current", i === stageIdx);
  });
}
function resetOtaUi() {
  setOtaStage(-1);
  $("#otaProgressFill").style.width = "0%";
  $("#otaPktNum").textContent = "0";
}

/* 状态名 → 阶段索引映射（6 阶段：上传固件 → 进入BOOT → 转发握手 → 擦除 → 传输 → 完成）
 * 后端 motor_ota_state_t 不含 upload/boot 阶段（纯前端驱动），后端状态从 handshake_req 起。
 * 顺序：上传固件(stage0) → 进BOOT(stage1) → 触发转发 → GD32 握手(stage2)/擦除(stage3)/传输(stage4)/完成(stage5)。 */
const OTA_STATE_STAGE = {
  idle: -1, pre_cancel: 2, handshake_req: 2, handshake_nonce: 2, handshake_confirm: 2,
  erase: 3, transfer: 4, finish_ota: 5, wait_app_run: 5, finished: 5, error: -1,
};
const OTA_STATE_ZH = {
  idle: "空闲", pre_cancel: "准备中…", handshake_req: "握手中…", handshake_nonce: "密钥协商…",
  handshake_confirm: "会话确认…", erase: "擦除 Flash…", transfer: "数据传输",
  finish_ota: "等待跳转…", wait_app_run: "等新 APP 运行确认(0AAC)…", finished: "完成", error: "错误",
};

$("#otaFileBtn").addEventListener("click", async () => {
  /* 优先尝试恢复待授权的 handle（FSAPI），成功则不再弹文件选择框 */
  if (ota.pendingHandle && await otaResumeWithGesture()) return;
  /* FSAPI：showOpenFilePicker 可拿到 handle 用于刷新后恢复 */
  if (window.showOpenFilePicker) {
    try {
      const [handle] = await window.showOpenFilePicker({
        types: [{ description: "固件 (.bin)", accept: { "application/octet-stream": [".bin"] } }],
        multiple: false,
      });
      const file = await handle.getFile();
      if (file.size > OTA_MAX_FW) {
        toast(`固件过大 (${file.size} B)，上限 ${OTA_MAX_FW} B (48 KB)`, "err");
        return;
      }
      ota.fileHandle = handle;
      ota.pendingHandle = null;
      ota.file = file;
      ota.bytes = new Uint8Array(await file.arrayBuffer());
      $("#otaFileName").textContent = `${file.name} — ${file.size.toLocaleString()} B`;
      $("#otaPktTotal").textContent = String(Math.ceil(file.size / 48));
      $("#otaCrc").textContent = "—";
      otaSaveFwMeta(file);
      otaIdbPutHandle(handle);
      otaLog(`<span class="log-info">[固件]</span> ${file.name} · ${file.size.toLocaleString()} B · ${Math.ceil(file.size / 48)} 包`);
      toast(`固件已加载: ${file.name}`, "ok");
      return;
    } catch (e) {
      if (e && e.name === "AbortError") return;  /* 用户取消，静默 */
      /* FSAPI 失败则回退到 input[type=file] */
    }
  }
  $("#otaFileInput").click();
});
$("#otaFileInput").addEventListener("change", async (e) => {
  const file = e.target.files[0];
  if (!file) return;
  if (file.size > OTA_MAX_FW) {
    toast(`固件过大 (${file.size} B)，上限 ${OTA_MAX_FW} B (48 KB)`, "err");
    e.target.value = "";
    return;
  }
  const buf = await file.arrayBuffer();
  ota.file = file;
  ota.bytes = new Uint8Array(buf);
  ota.fileHandle = null;      /* input 选择无 handle，刷新后只能展示文件名 */
  ota.pendingHandle = null;
  $("#otaFileName").textContent = `${file.name} — ${file.size.toLocaleString()} B`;
  $("#otaPktTotal").textContent = String(Math.ceil(file.size / 48));
  $("#otaCrc").textContent = "—";
  otaSaveFwMeta(file);
  /* input 选择无 FSAPI handle，清掉旧 handle 避免恢复到旧文件 */
  otaIdbPutHandle(null);
  otaLog(`<span class="log-info">[固件]</span> ${file.name} · ${file.size.toLocaleString()} B · ${Math.ceil(file.size / 48)} 包`);
  toast(`固件已加载: ${file.name}`, "ok");
});

$("#otaStartBtn").addEventListener("click", startOta);
$("#otaCancelBtn").addEventListener("click", cancelOta);

async function startOta() {
  if (ota.running) return;
  if (!state.connected) { toast("设备未连接", "err"); return; }
  if (!ota.file) { toast("请先选择固件 (.bin)", "warn"); return; }
  /* 进 BOOT 需要目标电机 node_id（构造 0x000N0000），未选节点则中止。
   * node=0 会变成 0x00000000（取消帧/广播），必须拒绝。 */
  const node = state.selectedNode;
  if (node == null || node < 1) {
    toast("请先在左侧节点列表选中目标电机（node ≥ 1）", "warn");
    return;
  }
  const ch = state.ch || 0;

  /* ★ 每次刷写都重新从磁盘读取固件，确保用最新文件内容，不依赖内存缓存。
   * 之前只读一次存 ota.bytes，之后多次刷写复用缓存——若磁盘文件被重新编译
   * 覆盖（改代码 rebuild），前端仍发旧数据，导致刷进电机的不是最新固件。
   * 有 FSAPI handle 时从 handle 重新读（handle 总是指向同一文件路径）；
   * 无 handle（input 选择）时从 ota.file 重新读 File 对象。
   * 读取失败提示用户重新选择固件，绝不带着旧/坏缓存刷写。 */
  try {
    let file = ota.file;
    if (ota.fileHandle && typeof ota.fileHandle.getFile === "function") {
      file = await ota.fileHandle.getFile();
    }
    if (file.size > OTA_MAX_FW) {
      toast(`固件过大 (${file.size} B)，上限 ${OTA_MAX_FW} B (48 KB)`, "err");
      return;
    }
    ota.file = file;
    ota.bytes = new Uint8Array(await file.arrayBuffer());
    $("#otaFileName").textContent = `${file.name} — ${file.size.toLocaleString()} B`;
    $("#otaPktTotal").textContent = String(Math.ceil(file.size / 48));
  } catch (e) {
    otaLog(`<span class="log-err">[固件]</span> 重新读取失败：${e.message || e}，请重新选择固件`);
    toast("固件读取失败，请重新选择", "err");
    return;
  }

  const ok = await showModal("开始电机 OTA",
    `将先上传固件 ${ota.file.name} (${ota.file.size.toLocaleString()} B) 到设备并校验 SHA-256，` +
    `再发送 CANFD_BOOT (0x000${nodeHex(node)}0000) 让节点 ${node} 进入 Bootloader，` +
    `由 GD32 本地完成 AES 握手与 CAN 传输。OTA 期间请勿操作电机。`);
  if (!ok) return;

  stopRepeat(true);
  ota.running = true;
  ota.node = node;  /* 记录目标节点：finished 后 verifyNewApp 查版本用 */
  ota.startTime = performance.now();
  $("#otaStartBtn").disabled = true;
  $("#otaCancelBtn").disabled = false;
  resetOtaUi();
  setOtaStage(0);  /* 上传固件阶段 */

  /* 整体超时看门狗：上传 + 进 BOOT + 传输 + 跳转 总计不应超过 OTA_TOTAL_TIMEOUT_MS。
   * 超时后自动取消，避免卡死时前端无限轮询 + ESP32 feedTask 残留。 */
  if (ota.watchdogTimer) clearTimeout(ota.watchdogTimer);
  ota.watchdogTimer = setTimeout(() => {
    if (!ota.running) return;
    otaLog(`<span class="log-err">[超时]</span> OTA 整体超时 (${OTA_TOTAL_TIMEOUT_MS / 1000}s)，自动取消…`);
    cancelOta();
  }, OTA_TOTAL_TIMEOUT_MS);

  /* OTA 期间暂停 motor/feedback/status 三链路轮询：
   * 三条链各占一个 HTTP socket + httpd 线程时间，叠加固件上传 + OTA poll（500ms）
   * + cancel 需要的新连接，6 个 max_open_sockets 会紧张，
   * 导致 cancel "Failed to fetch"（连接建立失败/被 lru_purge 踢掉）；
   * 且电机随后将跳 BOOT，motor 状态轮询无意义。
   * OTA 完成后 otaDone→startPolling 恢复。 */
  stopDataPolling();

  /* ── 步骤 1：前端先算本地固件 SHA-256（只算一次，重传不变）── */
  const fwSha256 = await crypto.subtle.digest("SHA-256", ota.bytes);
  const fwShaHex = Array.from(new Uint8Array(fwSha256))
    .map(b => b.toString(16).padStart(2, "0")).join("");
  otaLog(`<span class="log-info">[校验]</span> 本地固件 SHA-256: ${fwShaHex}`);

  try {
    /* ── 步骤 2：上传 + SHA 比对，不通过重传（最多 3 次尝试 = 首发 + 重发 2 次）──
     * POST /motor/ota 只收固件写 flash（不触发 SPI），ESP32 收尾从 flash 读回算 SHA 随响应返回；
     * 前端比对不一致 = flash 写坏 → 重传（每次上传都全盘擦除重写）；
     * 连续 3 次不过才中断——此时电机仍在 APP 正常运行，重试/中断都不影响电机。 */
    const kUploadMaxAttempts = 3;
    let uploadOk = false;
    for (let attempt = 1; attempt <= kUploadMaxAttempts && ota.running; attempt++) {
      otaLog(`<span class="log-info">[上传]</span> 上传固件到 ESP32 (CH${ch})，第 ${attempt}/${kUploadMaxAttempts} 次…`);
      let upJson = {};
      try {
        const upResp = await fetch(getBaseUrl() + `/motor/ota?ch=${ch}`, {
          method: "POST",
          headers: { "Content-Type": "application/octet-stream" },
          body: ota.bytes,
          signal: AbortSignal.timeout(15000),
        });
        upJson = await upResp.json().catch(() => ({}));
        if (!upResp.ok) throw new Error(upJson.error || `HTTP ${upResp.status}`);
      } catch (e) {
        otaLog(`<span class="log-err">[上传]</span> 第 ${attempt} 次上传失败：${e.message || e}`);
        if (attempt < kUploadMaxAttempts) await sleep(300);
        continue;
      }
      otaLog(`<span class="log-ok">[上传]</span> 固件已存储 (${upJson.size || ota.bytes.length} B)`);

      /* 比对：本地 SHA vs ESP32 从 flash 读回算的 SHA（upJson.sha256） */
      if (!upJson.sha256 || upJson.sha256.length !== 64) {
        /* 旧 ESP32 固件无 sha256 字段：无法本地比对，且旧固件 relay 还要求 body（新前端已不发），
         * 需刷新 ESP32 固件；继续尝试，若 relay 被拒会报错中止。 */
        otaLog(`<span class="log-err">[校验]</span> 设备未返回 flash SHA（旧 ESP32 固件，建议先刷新 ESP32），跳过比对继续`);
        uploadOk = true;
        break;
      }
      if (upJson.sha256.toLowerCase() === fwShaHex) {
        otaLog(`<span class="log-ok">[校验]</span> flash SHA 比对一致，允许进 BOOT`);
        uploadOk = true;
        break;
      }
      otaLog(`<span class="log-err">[校验]</span> 第 ${attempt} 次 flash SHA 不一致（device=${upJson.sha256.slice(0, 16)}…）${attempt < kUploadMaxAttempts ? "，重传…" : ""}`);
      if (attempt < kUploadMaxAttempts) await sleep(300);
    }
    if (!uploadOk) {
      throw new Error(`上传/SHA 校验连续 ${kUploadMaxAttempts} 次失败，中断（电机未进 BOOT）`);
    }

    /* ── 步骤 3：发 0x000N0000 进 BOOT 帧（DLC=0，fire-and-forget）──
     * 固件已收完且 flash SHA 比对通过，此刻才让电机离开 APP。
     * APP 侧 handle_boot_message() 只看 ID 的 bits[19:16] == node_id 即跳 BOOT。
     * 电机收到后立即 jumpToBOOT()（can_deinit + 复位），来不及回 CAN ACK，
     * 因此「GD32 未确认发送 / 总线无应答」是预期现象，绝不能当作进 BOOT 失败——
     * 之前正因把「未 ACK」当失败而中止，导致「电机其实已进 BOOT 却被判为失败」。
     * 是否真进 BOOT 交给后续 GD32 relay 握手（REQ→00AC）判定，握手超时才算失败。 */
    setOtaStage(1);  /* 进入 BOOT 阶段 */
    const bootId = canId(node, 0x0000);  /* 0x000N0000 */
    otaLog(`<span class="log-info">[进BOOT]</span> 发送 CANFD_BOOT ${hexId(bootId)} → 节点 ${node} (CH${ch})`);
    try {
      const r = await sendBatch([{ id: bootId, dlc: 0, data: "" }], { allowOffline: true });
      if (r.ok) {
        otaLog(`<span class="log-ok">[进BOOT]</span> 命令已发送并确认`);
      } else {
        otaLog(`<span class="log-ok">[进BOOT]</span> 命令已发出（无 CAN 应答属正常——电机跳 BOOT 来不及 ACK，交握手确认）`);
      }
    } catch (e) {
      otaLog(`<span class="log-info">[进BOOT]</span> 发送异常：${e.message || e}（继续，由握手确认是否进 BOOT）`);
    }
    otaLog(`<span class="log-ok">[进BOOT]</span> 命令已发出，立即触发转发…`);

    /* ── 步骤 4：POST /motor/ota/relay 触发 SPI START + 喂数据（纯启动扳机，无 body）──
     * SHA 校验已在步骤 2 闭环（比对+重传），relay 不再传/比 SHA。
     * 无前端 sleep：电机跳转由 GD32 kPreCancel 等 0x103 心跳窗口吸收（3s 超时报 0x02）。
     * SPI 0xC0(进 BOOT) 与 0xD0(START) 经 SPI worker 串行，顺序有保证。 */
    const rResp = await fetch(getBaseUrl() + `/motor/ota/relay?ch=${ch}`, {
      method: "POST",
      signal: AbortSignal.timeout(8000),
    });
    const rJson = await rResp.json().catch(() => ({}));
    if (!rResp.ok) throw new Error(rJson.error || `HTTP ${rResp.status}`);
    otaLog(`<span class="log-ok">[转发]</span> GD32 开始握手…`);
    startOtaPoll();
  } catch (e) {
    otaLog(`<span class="log-err">[失败]</span> ${e.message || e}`);
    toast("OTA 启动失败：" + (e.message || e), "err");
    otaDone();
  }
}

function startOtaPoll() {
	  const startTime = performance.now();
	  let lastState = "";
	  let lastLoggedPkt = -1;
	  ota.pollTimer = setInterval(async () => {
	    try {
	      const resp = await fetch(getBaseUrl() + "/motor/ota/status", {
	        signal: AbortSignal.timeout(2000), cache: "no-store",
	      });
	      const st = await resp.json();
	      updateOtaUi(st, startTime);
	      /* 状态变化写日志，避免 UI 只停在「开始握手」一行 */
	      if (st.state && st.state !== lastState) {
	        const zh = OTA_STATE_ZH[st.state] || st.state;
	        otaLog(`<span class="log-info">[状态]</span> ${zh}` +
	          (st.total_packets ? ` · ${st.packets_acked || 0}/${st.total_packets}` : ""));
	        lastState = st.state;
	      } else if (st.state === "transfer" && st.total_packets > 0) {
	        const acked = st.packets_acked || 0;
	        /* 传输期每约 64 包补一条进度，避免只有握手日志 */
	        if (acked - lastLoggedPkt >= 64 || acked >= st.total_packets) {
	          otaLog(`<span class="log-info">[传输]</span> ${acked}/${st.total_packets}`);
	          lastLoggedPkt = acked;
	        }
	      }
	      if (st.state === "finished" || st.state === "error" || st.state === "idle") {
	        clearInterval(ota.pollTimer);
	        ota.pollTimer = null;
	        if (st.state === "finished") {
	          otaLog(`<span class="log-ok">[完成]</span> 传输完成 · ${st.packets_acked} 包 · 电机新 APP 已发 0AAC 运行确认，核对版本…`);
	        } else if (st.state === "error") {
	          const errHint = st.error === 0x25 ? "（刷写传完但新 APP 未发 0AAC 运行确认——APP 可能未启动）" : "";
	          otaLog(`<span class="log-err">[错误]</span> 代码 0x${(st.error || 0).toString(16).toUpperCase()}${errHint}`);
	          toast("电机 OTA 失败 (0x" + (st.error || 0).toString(16) + ")", "err");
	        }
	        /* ★ 正常完成(finished)不发 cancel：电机 APP 已跳转启动，此时发 cancel
	         * 会让 GD32 发 0x00000000(kOtaCancelId)，被刚启动的电机 APP 当作
	         * CANFD_BOOT_ID(base_id=0x0000) → enter_silent_mode 10s → APP 看似卡死。
	         * 只有 error/idle/异常退出才发 cancel 兜底清理设备端残留状态。 */
	        otaDone(st.state !== "finished");
	        /* ★ finished = GD32 已收到电机新 APP 上电自主发的 [0A AC] 运行确认
	         * （kWaitAppRun → kFinished，无确认 3s 超时报 0x25）——APP 已确认在跑。
	         * verifyNewApp 只做最后一步：查版本号核对并回填 UI。 */
	        if (st.state === "finished") verifyNewApp(ota.node);
	      }
	    } catch (e) { /* 轮询瞬时失败静默 */ }
	  }, OTA_POLL_MS);
	}
	
	/* ── OTA 终验：查版本号核对新 APP ──
	 * 运行确认已由设备端闭环：电机新 APP 上电自主发 [0A AC]，GD32 kWaitAppRun
	 * 收到才报 finished（无确认 3s → error 0x25）——走到这里 APP 必然在跑且 CAN 已就绪。
	 * 本函数只补最后一步：发 GET_VERSION(0x000N00FF) 拿新版本号展示/核对，
	 * 无应答仅提醒（不推翻设备端的运行确认结论）。 */
	async function verifyNewApp(node) {
	  if (node == null || node < 1) return;
	  otaLog(`<span class="log-info">[验证]</span> 查询新 APP 版本号 (${hexId(ID_GET_VERSION_REQ(node))})…`);
	  await sleep(200);  /* APP 已发过 0AAC，CAN 已就绪，留小间隔避开启动期杂帧 */
	  const kVerifyAttempts = 2;
	  for (let attempt = 1; attempt <= kVerifyAttempts; attempt++) {
	    try {
	      const before = await fetchMotorVersionNode(node);
	      const prevSeq = before ? before.seq : null;
	      const res = await sendOneFrameWhenFree(ID_GET_VERSION_REQ(node), 24, bytesToHexStr(new Uint8Array(24)));
	      if (res.ok) {
	        const v = await waitMotorVersion(node, prevSeq, 4000);
	        if (v && v.version) {
	          const parsed = parseVersionPayload(v.version);
	          if (parsed && parsed.soft) {
	            /* 同步版本卡片与缓存，用户无需再手动点查版本 */
	            setVer("verSoft", parsed.soft || "--");
	            setVer("verHard", parsed.hard || "--");
	            setVer("verBoot", parsed.boot || "--");
	            state.versions.set(node, { soft: parsed.soft || null, hard: parsed.hard || null, boot: parsed.boot || null });
	            saveUiState();
	            otaLog(`<span class="log-ok">[成功]</span> OTA 成功 · 新 APP v${parsed.soft} 运行中（0AAC 确认 + 版本应答）`);
	            toast(`电机 OTA 成功 · APP v${parsed.soft}`, "ok");
	            return;
	          }
	        }
	      }
	    } catch (e) { /* 单轮异常按未应答处理，进入重试 */ }
	    if (attempt < kVerifyAttempts) {
	      otaLog(`<span class="log-info">[验证]</span> 第 ${attempt} 次无应答，重试…`);
	    }
	  }
	  otaLog(`<span class="log-err">[提醒]</span> APP 已发 0AAC 确认运行，但版本查询无应答，请手动点「查版本」核对`);
	  toast("OTA 完成（版本查询无应答，请手动核对）", "warn");
	}

	function updateOtaUi(st, startTime) {
	  const stage = OTA_STATE_STAGE[st.state] ?? -1;
	  setOtaStage(stage);
	  const acked = st.packets_acked || 0;
	  const total = st.total_packets || 1;
	  /* HTML 模板已是「包 <已确认> / <总数>」，这里各填各的；
	   * 之前往 otaPktNum 塞整串 "acked/total" 会渲染成「包 696/912 / 912」。 */
	  $("#otaPktNum").textContent = String(acked);
	  $("#otaPktTotal").textContent = String(total);
	  $("#otaProgressFill").style.width = (acked / total * 100).toFixed(1) + "%";
	  const elapsed = (performance.now() - startTime) / 1000;
	  if (elapsed > 0.5 && acked > 0) {
	    $("#otaSpeed").textContent = (acked * 48 / elapsed / 1024).toFixed(1) + " KB/s";
	  }
	  $("#otaCrc").textContent = OTA_STATE_ZH[st.state] || st.state;
	}

async function cancelOta() {
  if (!ota.running) return;
  otaLog(`<span class="log-err">[取消]</span> 正在取消…`);
  /* 先停 OTA 进度轮询，腾出 HTTP socket 给 cancel 请求本身。
   * motor/feedback/status 三链路在 startOta 时已 stopDataPolling，
   * 这里只剩 OTA poll（500ms）可能正占用一个 socket。
   * 先停 poll 再发 cancel，避免 cancel 被挤导致 "Failed to fetch"。 */
  if (ota.pollTimer) { clearInterval(ota.pollTimer); ota.pollTimer = null; }
  /* 取消请求重试 3 次：单次失败（网络抖动/ESP32 忙）时不能静默吞掉，
   * 否则 ESP32 侧 feedTask 不会退出，motorOtaIsActive() 残留为 true，
   * 导致后续 ESP32/GD32 OTA 全被"电机 OTA 进行中"拒绝。 */
  let ok = false;
  for (let i = 1; i <= 3 && !ok; i++) {
    try {
      const r = await fetch(getBaseUrl() + "/motor/ota/cancel", { method: "POST", signal: AbortSignal.timeout(5000) });
      ok = r.ok;
    } catch (e) {
      otaLog(`<span class="log-err">[取消]</span> 第 ${i} 次失败：${e.message || e}`);
    }
    if (!ok && i < 3) await sleep(500);
  }
  if (!ok) otaLog(`<span class="log-err">[取消]</span> 3 次均失败，设备端 feedTask 可能残留，建议刷新页面或等待自动超时`);
  otaDone();
}

/* otaDone(sendCancel): 清理前端 OTA 状态。
 * sendCancel=true 时发 POST /motor/ota/cancel 兜底清理设备端残留状态。
 * sendCancel=false（正常 finished）时不发 cancel——电机 APP 已跳转启动，
 * 此时发 cancel 会让 GD32 发 0x00000000(kOtaCancelId)，被电机 APP 当作
 * CANFD_BOOT_ID(base_id=0x0000) → enter_silent_mode 10s，APP 看似卡死。
 * 调用方：
 *   - startOtaPoll 正常完成 → otaDone(false)
 *   - startOtaPoll error/idle → otaDone(true)
 *   - cancelOta（用户主动取消）→ otaDone(true)
 *   - startOta 启动失败 → otaDone(true) */
function otaDone(sendCancel = true) {
  ota.running = false;
  $("#otaStartBtn").disabled = false;
  $("#otaCancelBtn").disabled = true;
  if (ota.watchdogTimer) { clearTimeout(ota.watchdogTimer); ota.watchdogTimer = null; }
  /* OTA 期间暂停了 motor/feedback/status 三链路轮询，完成后恢复。
   * startPolling 内部先 stopDataPolling 再重建，安全可重复调用。 */
  if (state.connected && !state.probing) {
    startPolling();
  }
  /* 仅在需要时发 cancel 兜底清理设备端 motorOtaIsActive。
   * 正常完成(finished)时 feedTask 已自行退出 + GD32 已进 kFinished，
   * 不需要 cancel；发了反而干扰刚跳转的电机 APP（见上注释）。 */
  if (sendCancel && state.connected) {
    fetch(getBaseUrl() + "/motor/ota/cancel", { method: "POST", signal: AbortSignal.timeout(3000) })
      .catch(() => { /* 兜底请求失败静默，不阻塞 UI 恢复 */ });
  }
}

/* ═══════════════════════ 通道切换（无连接按钮） ═══════════════════════ */
$("#chSelect").addEventListener("change", () => {
  state.ch = parseInt($("#chSelect").value, 10) || 0;
  updateConnUi();
  updateChSelectOptions();   /* 同步选框字色：切到有信号通道则绿色，否则默认 */
  /* 按新通道重刷节点列表 / STATUS 面板 / 在线汇总（通道视图过滤立即生效） */
  refreshChannelViews();
  /* 通道切换不额外拉 feedback：等下一轮 pollLoop，避免与单飞链并发 */
});

/* ═══════════════════════ 刷新速度（motor 轮询间隔） ═══════════════════════ */
/* 用户改 #refreshRate 后：clamp、回填、写 state，并按「N×GD32周期」立即重排。
 * 仅影响 motor 链路；feedback/status 保持 1Hz 独立节拍。 */
const refreshRateInput = $("#refreshRate");

/* 下发 GD32 0xC5 主动推送周期：复用通用 SPI 透传口 /spi/send（不新增 HTTP 路由）。
 * 命令码 0xC5 + 子操作码 0xFE，data 十六进制 = FE <ms_lo> <ms_hi>（毫秒 LE）。
 * GD32 端钳制 [50,5000]ms 并回 ACK。失败静默（不影响本地轮询）。 */
async function sendMotorPeriod(ms) {
  const lo = ms & 0xFF, hi = (ms >> 8) & 0xFF;
  const toHex = (b) => b.toString(16).padStart(2, "0").toUpperCase();
  try {
    await fetch(getBaseUrl() + "/spi/send", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ type: "C5", data: "FE" + toHex(lo) + toHex(hi), timeout_ms: 2000 }),
      signal: AbortSignal.timeout(3000),
    });
  } catch (e) { /* 周期下发失败静默：GD32 保持旧周期，本地轮询不受影响 */ }
}
function applyRefreshRate() {
  const v = clampNum(parseInt(refreshRateInput.value, 10) || 250, 50, 5000);
  refreshRateInput.value = v;
  state.refreshIntervalMs = v;
  if (state.poll.tick) {
    clearTimeout(state.poll.tick);
    state.poll.tick = null;
  }
  /* HTTP 轮询驱动：清掉旧节拍后必须立即按新间隔重排 motor 链，
   * 否则轮询链断死、STATUS 面板停更。
   * tickInFlight 时在途 pollLoop 的 finally 也会重排，schedulePollLoop 内部
   * stopTimer 保证不会双链并发。 */
  if (state.connected && !state.probing) {
    schedulePollLoop(state.pollGen, 0);
    sendMotorPeriod(v);   /* 同步 GD32 0xC5 推送周期（复用 /spi/send，命令码 0xC5 + 子命令 0xFE） */
  }
  const hz = 1000 / v;
  toast(`刷新速度 ${v} ms（≈${hz >= 10 ? hz.toFixed(0) : hz.toFixed(1)} Hz）`, "ok");
}
refreshRateInput.addEventListener("change", applyRefreshRate);
refreshRateInput.addEventListener("input", () => {
  /* 输入过程中只 clamp 下限显示，松手/change 再应用节拍，避免每个键都 toast */
  const raw = parseInt(refreshRateInput.value, 10);
  if (Number.isFinite(raw) && raw < 50) refreshRateInput.value = 50;
});

/* ═══════════════════════ 示波器系统 ═══════════════════════ */
const scopeSys = (() => {
  const BUF = 300;
  const dprOf = () => window.devicePixelRatio || 1;
  const hist = {
    u: new Float32Array(BUF), v: new Float32Array(BUF), w: new Float32Array(BUF),
    angle: new Float32Array(BUF), velocity: new Float32Array(BUF), current: new Float32Array(BUF),
    mcuTemp: new Float32Array(BUF), motTemp: new Float32Array(BUF),
  };
  let head = 0;

  function colors() {
    const cs = getComputedStyle(document.documentElement);
    return {
      u: cs.getPropertyValue("--seed-primary").trim() || "#22d3ee",
      v: "#34d399",
      w: cs.getPropertyValue("--seed-accent").trim() || "#f59e0b",
    };
  }
  let C = colors();
  setInterval(() => { C = colors(); }, 2000);

  function fit(canvas) {
    const dpr = dprOf();
    const r = canvas.getBoundingClientRect();
    if (r.width === 0) return false;
    if (canvas.width !== Math.round(r.width * dpr) || canvas.height !== Math.round(r.height * dpr)) {
      canvas.width = Math.round(r.width * dpr);
      canvas.height = Math.round(r.height * dpr);
    }
    canvas.getContext("2d").setTransform(dpr, 0, 0, dpr, 0, 0);
    return true;
  }
  function grid(ctx, w, h) {
    ctx.strokeStyle = "rgba(255,255,255,0.05)";
    ctx.lineWidth = 1;
    for (let i = 1; i < 4; i++) { const y = h / 4 * i; ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke(); }
    for (let i = 1; i < 6; i++) { const x = w / 6 * i; ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke(); }
    ctx.strokeStyle = "rgba(255,255,255,0.13)";
    ctx.beginPath(); ctx.moveTo(0, h / 2); ctx.lineTo(w, h / 2); ctx.stroke();
  }
  function trace(ctx, buf, color, range, w, h, lw, fill) {
    const mid = h / 2, gain = (h / 2 - 4) / range;
    if (fill) {
      ctx.beginPath();
      for (let i = 0; i < BUF; i++) {
        const idx = (head + i) % BUF;
        const x = i / (BUF - 1) * w;
        const y = mid - Math.max(-range, Math.min(range, buf[idx])) * gain;
        i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
      }
      ctx.lineTo(w, mid); ctx.lineTo(0, mid); ctx.closePath();
      ctx.fillStyle = color + "16";
      ctx.fill();
    }
    ctx.beginPath();
    ctx.strokeStyle = color; ctx.lineWidth = lw; ctx.lineJoin = "round";
    ctx.shadowColor = color; ctx.shadowBlur = 5;
    for (let i = 0; i < BUF; i++) {
      const idx = (head + i) % BUF;
      const x = i / (BUF - 1) * w;
      const y = mid - Math.max(-range, Math.min(range, buf[idx])) * gain;
      i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    }
    ctx.stroke();
    ctx.shadowBlur = 0;
  }
  function traceUni(ctx, buf, color, min, max, w, h, lw, fill) {
    const span = max - min;
    const yOf = (v) => h - 4 - Math.max(0, Math.min(1, (v - min) / span)) * (h - 8);
    if (fill) {
      ctx.beginPath();
      for (let i = 0; i < BUF; i++) {
        const idx = (head + i) % BUF;
        const x = i / (BUF - 1) * w;
        i === 0 ? ctx.moveTo(x, yOf(buf[idx])) : ctx.lineTo(x, yOf(buf[idx]));
      }
      ctx.lineTo(w, h); ctx.lineTo(0, h); ctx.closePath();
      ctx.fillStyle = color + "16";
      ctx.fill();
    }
    ctx.beginPath();
    ctx.strokeStyle = color; ctx.lineWidth = lw; ctx.lineJoin = "round";
    ctx.shadowColor = color; ctx.shadowBlur = 5;
    for (let i = 0; i < BUF; i++) {
      const idx = (head + i) % BUF;
      const x = i / (BUF - 1) * w;
      i === 0 ? ctx.moveTo(x, yOf(buf[idx])) : ctx.lineTo(x, yOf(buf[idx]));
    }
    ctx.stroke();
    ctx.shadowBlur = 0;
  }

  /* 大三相示波器 */
  const big = $("#scopeCanvas");
  const bigCtx = big.getContext("2d");
  function drawBig() {
    if (!fit(big)) return;
    const w = big.width / dprOf(), h = big.height / dprOf();
    bigCtx.clearRect(0, 0, w, h);
    grid(bigCtx, w, h);
    trace(bigCtx, hist.u, C.u, 5000, w, h, 1.6, false);
    trace(bigCtx, hist.v, C.v, 5000, w, h, 1.6, false);
    trace(bigCtx, hist.w, C.w, 5000, w, h, 1.6, false);
  }

  /* 迷你示波器（每卡片独立）
   * angle 用 auto: true 自适应 Y 轴（角度可能 >360 多圈累计或为负，
   * 固定 0~360 会让波形贴顶/贴底或越界不可见）。
   * 其余卡片量纲固定（velocity/current 对称范围，temp 单极性），保留 uni/range。 */
  const MINIS = {
    angle:    { el: $("#miniAngle"),    buf: hist.angle,    auto: true,                   unit: "\u00B0",     color: () => C.u },
    velocity: { el: $("#miniVelocity"), buf: hist.velocity, auto: true,                   unit: "deg/s", color: () => C.v },
    current:  { el: $("#miniCurrent"),  buf: hist.current,  auto: true,                   unit: "mA",    color: () => C.w },
    mcuTemp:  { el: $("#miniMcuTemp"),  buf: hist.mcuTemp,  uni: { min: 20, max: 100 },  ticks: [100, 60, 20],      unit: "\u00B0C",    color: () => C.v },
    motTemp:  { el: $("#miniMotTemp"),  buf: hist.motTemp,  uni: { min: 20, max: 100 },  ticks: [100, 60, 20],      unit: "\u00B0C",    color: () => C.w },
  };
  for (const m of Object.values(MINIS)) m.ctx = m.el.getContext("2d");

  const MONO = '"Cascadia Code","Fira Code",Consolas,monospace';
  const fmtTick = (v) => (Math.abs(v) >= 1000 ? (v / 1000).toFixed(v % 1000 === 0 ? 0 : 1) + "k" : String(v));

  /* 自适应 Y 轴：扫 buf 取实际数据范围，加 10% padding，生成 3 个 nice ticks。
   * 用于 angle/velocity/current（各自范围不确定，固定轴会让波形拟合成直线或越界）。
   * 空缓冲/全零时用小范围居中显示，避免空白卡片无刻度。 */
  function autoScale(buf) {
    let lo = Infinity, hi = -Infinity, hasNonZero = false;
    for (let i = 0; i < buf.length; i++) {
      const v = buf[i];
      if (!Number.isFinite(v)) continue;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
      if (v !== 0) hasNonZero = true;
    }
    /* 无有效数据或全零：小范围居中，等有数据后自然扩展 */
    if (!hasNonZero || !Number.isFinite(lo) || !Number.isFinite(hi)) {
      return { min: -1, max: 1, ticks: [1, 0, -1] };
    }
    /* 平坦线（lo===hi）：围绕该值拓展小范围 */
    if (lo === hi) {
      const c = lo;
      const pad = Math.abs(c) * 0.2 || 1;
      return { min: c - pad, max: c + pad, ticks: [c + pad, c, c - pad] };
    }
    /* 留 10% padding，避免波形贴顶/贴底 */
    const span = hi - lo;
    const pad = span * 0.1 || 1;
    let min = lo - pad, max = hi + pad;
    /* nice 化：把范围对齐到 1/2/5 整数倍刻度，ticks 取三等分点 */
    const niceStep = niceStepSize(max - min, 3);
    min = Math.floor(min / niceStep) * niceStep;
    max = Math.ceil(max / niceStep) * niceStep;
    const ticks = [max, (max + min) / 2, min];
    return { min, max, ticks };
  }

  /* 计算 nice 刻度步长：让 (max-min)/step ≈ targetTicks，并对齐到 1/2/5×10^k。 */
  function niceStepSize(range, targetTicks) {
    const rawStep = range / targetTicks;
    const mag = Math.pow(10, Math.floor(Math.log10(rawStep)));
    const norm = rawStep / mag;
    let step;
    if (norm < 1.5) step = 1;
    else if (norm < 3) step = 2;
    else if (norm < 7) step = 5;
    else step = 10;
    return step * mag;
  }

  function drawYScale(ctx, m, w, h) {
    /* auto 模式：drawMini 已把 m.uni/ticks 算好填入；其余模式用静态配置 */
    const yOf = m.uni
      ? (v) => h - 4 - Math.max(0, Math.min(1, (v - m.uni.min) / (m.uni.max - m.uni.min))) * (h - 8)
      : (v) => h / 2 - Math.max(-m.range, Math.min(m.range, v)) * ((h / 2 - 4) / m.range);
    ctx.save();
    ctx.font = "8px " + MONO;
    ctx.textBaseline = "middle";
    for (const t of m.ticks) {
      const y = yOf(t);
      ctx.strokeStyle = "rgba(226,233,242,0.35)";
      ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(5, y); ctx.stroke();
      ctx.fillStyle = "rgba(226,233,242,0.5)";
      ctx.fillText(fmtTick(Math.round(t)), 7, y);
    }
    if (m.unit) {
      ctx.textBaseline = "top";
      ctx.fillStyle = "rgba(226,233,242,0.35)";
      ctx.fillText(m.unit, w - ctx.measureText(m.unit).width - 4, 3);
    }
    ctx.restore();
  }

  function drawMini(m) {
    if (!fit(m.el)) return;
    const w = m.el.width / dprOf(), h = m.el.height / dprOf();
    /* auto 模式：每帧根据 buf 实际数据重算 Y 轴范围 + ticks */
    if (m.auto) {
      const sc = autoScale(m.buf);
      m.uni = { min: sc.min, max: sc.max };
      m.ticks = sc.ticks;
    }
    m.ctx.clearRect(0, 0, w, h);
    grid(m.ctx, w, h);
    if (m.uni) traceUni(m.ctx, m.buf, m.color(), m.uni.min, m.uni.max, w, h, 1.8, true);
    else trace(m.ctx, m.buf, m.color(), m.range, w, h, 1.8, true);
    drawYScale(m.ctx, m, w, h);
  }

  /* 点击卡片 → 只切换自己的波形 */
  document.querySelectorAll(".big-reading, .temp-block").forEach((card) => {
    card.addEventListener("click", () => card.classList.toggle("open"));
  });

  /* 采样：用真实解码的 STATUS 数据填充历史缓冲（1Hz 数据 → 平滑插值）。
   * node×channel 双键：只采当前通道的记录，避免波形混入其他通道同号电机数据。 */
  function sample() {
    const n = state.selectedNode;
    const motor = n != null ? state.motors.find((m) => m.node === n && m.ch === state.ch) : null;
    const d = motor ? state.decoded.get(motorKey(motor)) : null;
    const online = d && motor && motor.online;
    if (!online) {
      hist.u[head] = hist.v[head] = hist.w[head] = 0;
      hist.angle[head] = hist.velocity[head] = hist.current[head] = 0;
      hist.mcuTemp[head] = hist.motTemp[head] = 0;
    } else {
      /* 三相电流：用解码的 U/V/W 瞬时值（mA） */
      const targets = { u: d.uCurrent, v: d.vCurrent, w: d.wCurrent };
      for (const ch of ["u", "v", "w"]) {
        const prev = hist[ch][(head + BUF - 1) % BUF];
        hist[ch][head] = prev + (clampNum(targets[ch], -5000, 5000) - prev) * 0.5;
      }
      const pA = hist.angle[(head + BUF - 1) % BUF];
      hist.angle[head] = pA + (d.rtAngle - pA) * 0.4;
      const pV = hist.velocity[(head + BUF - 1) % BUF];
      hist.velocity[head] = pV + (d.rtVelocity - pV) * 0.4;
      const pC = hist.current[(head + BUF - 1) % BUF];
      hist.current[head] = pC + (d.rtCurrent - pC) * 0.4;
      const pMcu = hist.mcuTemp[(head + BUF - 1) % BUF];
      hist.mcuTemp[head] = pMcu + (d.mcuTemp - pMcu) * 0.1;
      const pMot = hist.motTemp[(head + BUF - 1) % BUF];
      hist.motTemp[head] = pMot + (d.motTemp - pMot) * 0.1;
    }
    head = (head + 1) % BUF;
  }

  let lastSample = 0;
  function loop(ts) {
    /* 采样节流到 ~30Hz，避免 1Hz 数据下过度密集 */
    if (ts - lastSample > 33) { sample(); lastSample = ts; }
    drawBig();
    for (const [sig, m] of Object.entries(MINIS)) {
      const card = document.querySelector(`[data-signal="${sig}"]`);
      if (card && card.classList.contains("open")) drawMini(m);
    }
    requestAnimationFrame(loop);
  }
  requestAnimationFrame(loop);
  return {};
})();

/* ═══════════════════════ 初始化 ═══════════════════════ */
/* 本页不提供连接配置：读主页 localStorage dcu_esp_ip，进入后自动探测 */
state.ip = getSharedDeviceIp();
/* 先恢复用户上次输入（modeSelect / 目标值 / PID / 通道 / 选中节点），
 * 再 applyModeVisibility 才能让保存的 mode 决定字段可见性。 */
loadUiState();
/* 固定节点 1–9：恢复后若无有效选中，默认 N1，保证离线也能直接刷写 */
if (state.selectedNode == null || state.selectedNode < 1 || state.selectedNode > 9) {
  state.selectedNode = 1;
}
applyModeVisibility();
updateNodeLabels();
updateConnUi();
renderNodeList();
renderStatusPanel();
restoreVersionUi(state.selectedNode);  /* 回填上次缓存的版本号 */
bindAutoSave();
initCmdCommit();  /* CMD 输入提交制：必须在 loadUiState 回填之后初始化，否则初始提交值是 HTML 默认值 */
otaRestorePersistedFirmware();  /* 异步恢复上次固件（无阻塞） */

/* 暴露诊断对象到 window，供控制台排查慢请求：
 *   window.phaseDebug.lastSlowReq    // 上次慢请求（>200ms）path+耗时
 *   window.phaseDebug.slowStats      // 累计慢请求统计
 *   window.phaseDebug.reset()        // 清空慢请求统计 */
window.phaseDebug = {
  get lastSlowReq() { return state.phaseDebug && state.phaseDebug.lastSlowReq; },
  get slowStats() { return state.phaseDebug && state.phaseDebug.slowStats; },
  reset() {
    if (state.phaseDebug) {
      state.phaseDebug.lastSlowReq = null;
      state.phaseDebug.slowStats = {};
    }
    console.log("[phaseDebug] 慢请求统计已清空");
  },
};

/* 延迟一帧再连，避免阻塞首屏渲染 */
setTimeout(connectShared, 80);
