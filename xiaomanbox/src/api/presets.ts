import { get } from './client';
import type { PresetsResponse } from './contract';

/**
 * 预设 API —— 数据源是 backend/presets.mjs（唯一真源，含各模式专属系统提示词）。
 * 前端只消费 categories/presets 做选择器 UI；prompt 注入发生在后端 agent。
 */
export function fetchPresets(): Promise<PresetsResponse> {
  return get<PresetsResponse>('/presets');
}
