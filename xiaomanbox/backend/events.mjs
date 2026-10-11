/**
 * M3 眼 —— append-only 事件日志（W-69）+ 事件订阅镜像（W-74）
 *
 * 核心：卡片状态来自本地事件日志，不调模型/引擎。断开引擎后事件日志仍在，
 * 所以卡片仍显示最后状态（W-75 负控：证明真的没问模型）。
 *
 * 事件：{ id(递增), ts, type, sessionId?, sceneId?, data }
 * 落盘：~/.xiaomanbox/events.jsonl（append-only，每行一条 JSON）
 * 内存：保留最近 MAX_IN_MEM 条供增量订阅。
 */

import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

const EVENTS_FILE = path.join(process.env.XIAOMAN_HOME || path.join(os.homedir(), '.xiaomanbox'), 'events.jsonl');
const MAX_IN_MEM = 1000;

let seq = 0; // 事件自增 id（进程内；重启后从落盘恢复）
const inMem = []; // 最近事件（含 id/ts/type/data）

function load() {
  try {
    const raw = fs.readFileSync(EVENTS_FILE, 'utf8');
    for (const line of raw.split('\n')) {
      if (!line.trim()) continue;
      try {
        const e = JSON.parse(line);
        if (e.id > seq) seq = e.id;
        inMem.push(e);
        if (inMem.length > MAX_IN_MEM) inMem.shift();
      } catch {
        /* 跳过坏行 */
      }
    }
  } catch {
    /* 文件不存在 = 空日志 */
  }
}
load();

/** 追加事件（append-only） */
export function emit(type, data = {}, meta = {}) {
  seq += 1;
  const e = { id: seq, ts: new Date().toISOString(), type, ...meta, ...data };
  inMem.push(e);
  if (inMem.length > MAX_IN_MEM) inMem.shift();
  try {
    fs.mkdirSync(path.dirname(EVENTS_FILE), { recursive: true });
    fs.appendFileSync(EVENTS_FILE, JSON.stringify(e) + '\n', 'utf8');
  } catch {
    /* 落盘失败不影响内存态 */
  }
  return e;
}

/** 增量订阅：返回 id > since 的事件 */
export function listSince(since = 0) {
  return inMem.filter((e) => e.id > since);
}

/** 当前最新事件 id（前端轮询游标） */
export function latestId() {
  return seq;
}

/** 事件总数（含落盘但不在内存的） */
export function totalCount() {
  return seq;
}
