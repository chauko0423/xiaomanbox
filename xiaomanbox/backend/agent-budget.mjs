/** Approximate preflight, not the model tokenizer. Never truncate tool-call pairs. */
export function estimateTokens(value) {
  const str = typeof value === 'string' ? value : JSON.stringify(value ?? '');
  let cjk = 0;
  for (const ch of str) if (ch.codePointAt(0) > 0x2e7f) cjk++;
  return cjk + Math.ceil((str.length - cjk) / 4);
}
export function requestBudget(messages, tools, cfg) {
  const window = Number.isSafeInteger(cfg.contextWindow) && cfg.contextWindow >= 1024 ? cfg.contextWindow : 131072;
  const output = cfg.maxOutputTokens > 0 ? cfg.maxOutputTokens : Math.min(1024, Math.max(256, Math.floor(window / 8)));
  let images = 0;
  const normalized = messages.map(m => ({...m, content: Array.isArray(m.content) ? m.content.map(p => {
    if (p.type !== 'image_url') return p;
    images++;
    return {type: 'image_url'};
  }) : m.content}));
  const input = estimateTokens(normalized) + estimateTokens(tools || []) + messages.length * 8 + 32 + images * 1024;
  const limit = Math.min(window - output, cfg.maxInputTokens > 0 ? cfg.maxInputTokens : Infinity);
  return {estimated: true, input, output, window, limit, images, fits: input <= limit};
}
export function enforceRequestBudget(messages, tools, cfg) {
  const budget = requestBudget(messages, tools, cfg);
  if (!budget.fits) {
    const e = new Error(`context_budget_exceeded：估算输入 ${budget.input} token，输入预算 ${budget.limit}，窗口 ${budget.window}，输出预留 ${budget.output}。请减少工具/技能/历史/附件，或在确认引擎支持后调整上下文窗口；不会自动截断请求。图片估算及文本估算不等于模型实际 token。`);
    e.retryable = false;
    throw e;
  }
  return budget;
}
