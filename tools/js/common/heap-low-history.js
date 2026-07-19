/**
 * heap-low-history.js - 堆"最低空闲创新低"记录的浏览器端持久化
 *
 * 目的：设备 RAM 只保留最新 1 条创新低；长时间挂机早期记录会被覆盖。
 *      前端定时拉 /system/heap_low_log，发现新记录就追加到 IndexedDB，
 *      弹窗里合并显示「设备当前 1 条 + 浏览器累积历史」，掉电/重启不丢本地史。
 *
 * 存储设计：
 *   - 单 objectStore 'heap_low_records'，keyPath 'id'（自增）
 *   - 每条记录附前端写入时的时间戳 saved_at_ms（wall clock）
 *   - 设备重启会重置 ts_ms（启动后毫秒）和 min_free_heap（水位清零），
 *     所以去重 key 用 (ts_ms, min_free_heap, free_heap, alloc_count) 组合，
 *     重启后第一条 min_free_heap 必然高于上一次会话最低值 → 自然不会误并入。
 *   - 上限 HEAP_HISTORY_MAX 条，超出按 saved_at_ms 升序删最早的。
 */

var IDB_NAME = 'esp_dcu_heap_low';
var IDB_STORE = 'heap_low_records';
var IDB_VERSION = 1;  // 独立库，与 file-handle.js 的 dcu_ota_fw_v1 不冲突
/** 浏览器可保存的创新低条数上限（设备仅 1 槽；多条全靠这里）。 */
export var HEAP_HISTORY_MAX = 500;

var _dbPromise = null;

function openDb() {
  if (!_dbPromise) {
    _dbPromise = new Promise(function (resolve, reject) {
      if (!window.indexedDB) {
        reject(new Error('indexedDB unavailable'));
        return;
      }
      var req = indexedDB.open(IDB_NAME, IDB_VERSION);
      req.onupgradeneeded = function () {
        var db = req.result;
        if (!db.objectStoreNames.contains(IDB_STORE)) {
          var store = db.createObjectStore(IDB_STORE, { keyPath: 'id', autoIncrement: true });
          // 去重查询索引：同一会话内 (ts_ms,min_free_heap,free_heap,alloc_count) 唯一
          store.createIndex('dedup', ['ts_ms', 'min_free_heap', 'free_heap', 'alloc_count'], { unique: false });
          store.createIndex('saved_at', 'saved_at_ms', { unique: false });
        }
      };
      req.onsuccess = function () { resolve(req.result); };
      req.onerror = function () { reject(req.error || new Error('idb open failed')); };
    });
    _dbPromise.catch(function () { _dbPromise = null; });
  }
  return _dbPromise;
}

/** 计算单条记录的去重 key 字符串（用于 in-memory 比对，避免每次都走 IDB 索引） */
function dedupKey(r) {
  return r.ts_ms + '|' + r.min_free_heap + '|' + r.free_heap + '|' + (r.alloc_count || 0);
}

/** 把设备拉到的一批 records（最新在前）增量写入 IndexedDB，去重。
 *  返回 Promise<number>：本次实际新增的条数。 */
export function appendHeapLowRecords(records) {
  if (!Array.isArray(records) || !records.length) return Promise.resolve(0);
  return openDb().then(function (db) {
    return new Promise(function (resolve, reject) {
      var tx = db.transaction(IDB_STORE, 'readwrite');
      var store = tx.objectStore(IDB_STORE);
      var idx = store.index('dedup');
      var newCount = 0;
      var savedAt = Date.now();

      // 顺序处理：每条先查 dedup 索引，不存在则 put
      var i = 0;
      function next() {
        if (i >= records.length) {
          // 全部处理完后再裁剪到上限
          pruneToMax(store, function () {
            tx.oncomplete ? null : null;
          });
          return;
        }
        var r = records[i++];
        var key = [r.ts_ms, r.min_free_heap, r.free_heap, r.alloc_count || 0];
        var getReq = idx.get(key);
        getReq.onsuccess = function () {
          if (!getReq.result) {
            // 新记录：写入
            var rec = Object.assign({}, r, { saved_at_ms: savedAt });
            var putReq = store.put(rec);
            putReq.onsuccess = function () { newCount++; next(); };
            putReq.onerror = function () { next(); };  // 单条失败不阻断
          } else {
            next();  // 已存在跳过
          }
        };
        getReq.onerror = function () { next(); };
      }
      next();

      tx.oncomplete = function () { resolve(newCount); };
      tx.onerror = function () { reject(tx.error); };
      tx.onabort = function () { reject(tx.error || new Error('tx aborted')); };
    });
  }).catch(function (e) {
    // IndexedDB 不可用（无痕模式等）静默降级
    return 0;
  });
}

/** 删最早的记录直到条数 <= HEAP_HISTORY_MAX。需在 readwrite tx 内调用。 */
function pruneToMax(store, done) {
  var countReq = store.count();
  countReq.onsuccess = function () {
    var total = countReq.result;
    if (total <= HEAP_HISTORY_MAX) { done(); return; }
    var toRemove = total - HEAP_HISTORY_MAX;
    // 按 saved_at 索引升序游标删前 toRemove 条
    var idx = store.index('saved_at');
    var curReq = idx.openCursor();
    var removed = 0;
    curReq.onsuccess = function (ev) {
      var cursor = ev.target.result;
      if (!cursor || removed >= toRemove) { done(); return; }
      var delReq = store.delete(cursor.primaryKey);
      delReq.onsuccess = function () { removed++; cursor.continue(); };
      delReq.onerror = function () { cursor.continue(); };
    };
    curReq.onerror = function () { done(); };
  };
  countReq.onerror = function () { done(); };
}

/** 读全部累积历史，按设备时间 ts_ms 升序（旧的在前），便于弹窗"历史段"显示。
 *  返回 Promise<Array>。IndexedDB 不可用时返回 []。 */
export function loadHeapLowHistory() {
  return openDb().then(function (db) {
    return new Promise(function (resolve, reject) {
      var tx = db.transaction(IDB_STORE, 'readonly');
      var store = tx.objectStore(IDB_STORE);
      var all = [];
      var curReq = store.openCursor();
      curReq.onsuccess = function (ev) {
        var cursor = ev.target.result;
        if (!cursor) return;
        all.push(cursor.value);
        cursor.continue();
      };
      curReq.onerror = function () { reject(curReq.error); };
      tx.oncomplete = function () {
        // 按 ts_ms 升序；同 ts_ms 时按 saved_at 升序（保证稳定）
        all.sort(function (a, b) {
          if (a.ts_ms !== b.ts_ms) return a.ts_ms - b.ts_ms;
          return (a.saved_at_ms || 0) - (b.saved_at_ms || 0);
        });
        resolve(all);
      };
      tx.onerror = function () { reject(tx.error); };
    });
  }).catch(function () { return []; });
}

/** 清空全部历史（用于"清空"按钮，可选实现）。返回 Promise<boolean>。 */
export function clearHeapLowHistory() {
  return openDb().then(function (db) {
    return new Promise(function (resolve) {
      var tx = db.transaction(IDB_STORE, 'readwrite');
      var req = tx.objectStore(IDB_STORE).clear();
      req.onsuccess = function () { resolve(true); };
      req.onerror = function () { resolve(false); };
    });
  }).catch(function () { return false; });
}

/** 返回当前累积条数（仅用于 UI 提示，不需要数据）。返回 Promise<number>。 */
export function countHeapLowHistory() {
  return openDb().then(function (db) {
    return new Promise(function (resolve) {
      var tx = db.transaction(IDB_STORE, 'readonly');
      var req = tx.objectStore(IDB_STORE).count();
      req.onsuccess = function () { resolve(req.result || 0); };
      req.onerror = function () { resolve(0); };
    });
  }).catch(function () { return 0; });
}

/** 计算给定 records 数组（设备拉到的最新在前）与参考集合（已存历史）相比的新条数。
 *  纯内存比对，不写库，用于先判断是否需要写。 */
export function diffNewCount(records, existingHistory) {
  if (!Array.isArray(records) || !records.length) return 0;
  var seen = {};
  if (Array.isArray(existingHistory)) {
    for (var i = 0; i < existingHistory.length; ++i) {
      seen[dedupKey(existingHistory[i])] = true;
    }
  }
  var n = 0;
  for (var j = 0; j < records.length; ++j) {
    if (!seen[dedupKey(records[j])]) ++n;
  }
  return n;
}