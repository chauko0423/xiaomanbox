/**
 * 模型保活 / 预热（2026-10-09 用户：发任务到 4090 GPU 使用率上升耗时 10~13s 的优化）。
 *
 * 根因（实测）：网络链路只有 0.6ms，慢在「发任务 → GPU 真正开算」这段冷启动——
 * 4090 服务端把模型换出显存（或换 slot）后，下次请求要重新 load 权重。
 *
 * 对策：后端空闲时定时发一个「极轻」请求，让服务端保持模型热加载。
 *   - 只对「远程」提供方保活（本地 127.0.0.1 引擎自己管，不干预）；
 *   - 用 /v1/models（最轻、不触发推理、不烧 token）做探测 + 一次极小 chat 做真预热；
 *   - 全部 fire-and-forget，失败静默（后端离线/网络断都不影响主流程）；
 *   - 只在「空闲」（无任务运行）时发，绝不和真实任务抢推理槽。
 *
 * 配置（环境变量，可调）：
 *   WB_KEEPALIVE_INTERVAL_MS  默认 60000（60s）
 *   WB_KEEPALIVE_ENABLED      默认 1（开）
 */

import http from 'node:http';
import https from 'node:https';
import { activeProvider } from './providers.mjs';

const INTERVAL = Number(process.env.WB_KEEPALIVE_INTERVAL_MS) || 60000;
const ENABLED = (process.env.WB_KEEPALIVE_ENABLED || '0') !== '0';

let timer = null;
let busy = false; // 有任务在跑时不保活（agent 置位）

/** agent 通知保活器「现在有没有任务在跑」 */
export function setKeepaliveBusy(v) {
  busy = !!v;
}

/** 发一个 HTTP 请求，返回 { ok, ms }；任何失败都吞掉 */
function ping(url, { apiKey, timeoutMs = 8000, method = 'GET', body } = {}) {
  return new Promise((resolve) => {
    let u;
    try {
      u = new URL(url);
    } catch {
      return resolve({ ok: false, ms: 0 });
    }
    const headers = { Accept: 'application/json' };
    if (apiKey) headers.Authorization = `Bearer ${apiKey}`;
    const payload = body ? (typeof body === 'string' ? body : JSON.stringify(body)) : null;
    if (payload) {
      headers['Content-Type'] = 'application/json';
      headers['Content-Length'] = Buffer.byteLength(payload); // NInfer 引擎对无长度 POST 会拒连，必须带
    }
    // 2026-10-11：https 提供方（智谱等云端 API）需要走 https.request，否则明文连 443 吃 301
    const transport = u.protocol === 'https:' ? https : http;
    const req = transport.request(
      { hostname: u.hostname, port: u.port, path: u.pathname, method, headers, timeout: timeoutMs },
      (res) => {
        res.resume();
        res.on('end', () => resolve({ ok: res.statusCode === 200, ms: Date.now() - t0 }));
      }
    );
    const t0 = Date.now();
    req.on('timeout', () => req.destroy());
    req.on('error', () => resolve({ ok: false, ms: Date.now() - t0 }));
    if (payload) req.write(payload);
    req.end();
  });
}

/** 单次保活：探测 + （可选）极小 chat 预热 */
async function tick() {
  if (busy) return; // 任务在跑，不抢槽
  const prov = activeProvider();
  if (!prov) return;
  // 只保活远程提供方（本地引擎 127.0.0.1 跳过）
  if (prov.baseUrl.includes('127.0.0.1') || prov.baseUrl.includes('localhost')) return;
  if (!prov.apiKey && !prov.baseUrl.includes('8400')) return; // 无 key 的远程不预热（大概率未配置完整）

  // 1. 探测：从 baseUrl 反推 /v1/models
  const base = prov.baseUrl.replace(/\/chat\/completions\/?$/, '').replace(/\/v1\/?$/, '');
  const modelsUrl = base.includes('/v1') ? base.replace(/\/$/, '') + '/models' : base.replace(/\/$/, '') + '/v1/models';
  await ping(modelsUrl, { apiKey: prov.apiKey });

  // 2. 真预热：极小 chat（让模型真正热起来；max_tokens 给 8，避免 1 个 token 全进思考导致空响应）
  const chatUrl = prov.baseUrl.includes('/chat/completions') ? prov.baseUrl : prov.baseUrl.replace(/\/$/, '') + '/v1/chat/completions';
  await ping(chatUrl, {
    apiKey: prov.apiKey,
    method: 'POST',
    timeoutMs: 15000,
    body: { model: prov.model, messages: [{ role: 'user', content: 'hi' }], max_tokens: 8, stream: false },
  });
}

export function startKeepalive() {
  if (!ENABLED || timer) return;
  // 启动后先预热一次，之后按 interval 循环
  tick().catch(() => {});
  timer = setInterval(() => tick().catch(() => {}), INTERVAL);
  if (timer.unref) timer.unref(); // 不阻止进程退出
  console.log(`[keepalive] 模型保活已启动 interval=${INTERVAL}ms`);
}

export function stopKeepalive() {
  if (timer) {
    clearInterval(timer);
    timer = null;
  }
}
