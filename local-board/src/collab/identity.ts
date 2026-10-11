const COLORS = [
  "#E03131",
  "#1971C2",
  "#2F9E44",
  "#F08C00",
  "#9C36B5",
  "#0C8599",
  "#D6336C",
  "#5F3DC4",
];

const NAMES = [
  "熊猫",
  "水獭",
  "狐狸",
  "海豚",
  "猎鹰",
  "树懒",
  "鲸鱼",
  "刺猬",
  "松鼠",
  "企鹅",
];

const STORAGE_KEY = "team-board:identity";

export type Identity = {
  id: string;
  name: string;
  color: string;
};

function pick<T>(list: T[]): T {
  return list[Math.floor(Math.random() * list.length)];
}

/**
 * 本地生成一个稳定的身份，存在 localStorage 里，刷新不变。
 *
 * @param displayName 登录后传入的真实用户名：覆盖掉随机动物名，
 *   这样光标上的名字别人认得出来。传 null/undefined 时保持原样。
 */
export function getIdentity(displayName?: string | null): Identity {
  let identity: Identity | null = null;
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw) {
      const parsed = JSON.parse(raw);
      if (parsed && parsed.name && parsed.color) identity = parsed;
    }
  } catch {
    // localStorage 不可用时退化为随机会话身份
  }

  if (!identity) {
    identity = {
      id: Math.random().toString(36).slice(2, 10),
      name: pick(NAMES),
      color: pick(COLORS),
    };
  }

  // 用户名变了（登录/切换账号）→ 就地改掉并落盘，颜色保持不动
  if (displayName && displayName !== identity.name) {
    identity = { ...identity, name: displayName };
  }

  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(identity));
  } catch {
    // 忽略
  }
  return identity;
}

export function randomRoomId(): string {
  return Math.random().toString(36).slice(2, 10);
}
