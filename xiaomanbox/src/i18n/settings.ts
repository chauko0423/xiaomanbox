import { addTranslations } from './index';

/**
 * 设置弹窗 + 外观（2026-10-09 全应用双语）。
 */
addTranslations({
  // 分类
  通用: 'General',
  个人主页: 'Profile',
  外观: 'Appearance',

  // 常规
  设置: 'Settings',
  常规: 'General',
  语言: 'Language',
  '界面显示语言。': 'Interface display language.',
  字体大小: 'Font size',
  '调整全局界面字号，即时生效': 'Adjust the global font size, applied instantly',
  小: 'Small',
  大: 'Large',

  // 个人主页
  头像: 'Avatar',
  '选图后自动裁成圆形并压缩上传（96×96）。': 'Image is auto-cropped to a circle and compressed on upload (96×96).',
  上传头像: 'Upload avatar',
  移除: 'Remove',
  昵称: 'Nickname',
  '展示名（用户名': 'Display name (the username',
  '是身份键，注册后不可改）。': 'is the identity key and cannot be changed after signup).',
  '昵称（留空显示用户名）': 'Nickname (leave blank to show username)',
  保存: 'Save',
  手机号: 'Phone',
  当前: 'Current',
  未绑定: 'Not bound',
  '输入完整 11 位新号可换绑。': 'Enter a full 11-digit number to rebind.',
  '输入新手机号换绑': 'Enter a new phone number to rebind',
  换绑: 'Rebind',
  修改密码: 'Change password',
  当前密码: 'Current password',
  新密码: 'New password',
  '至少 4 位；需再输一遍确认。': 'At least 4 characters; re-enter to confirm.',
  '新密码（至少 4 位）': 'New password (at least 4 chars)',
  '再输一遍新密码': 'Re-enter new password',
  '尚未注册账户。先到「账户」面板注册/登录，再回来完善资料。': 'No account yet. Register or sign in from the Account panel first, then return to complete your profile.',

  // 外观
  强调色: 'Accent color',
  '信号色（当前项 / 发送键 / 焦点环 / 进度），只做信号不铺面。': 'Signal color (active item, send button, focus ring, progress); signals only, not a surface wash.',
  界面字体: 'UI font',
  '本机未安装的字体自动回落到下一级。': 'Fonts not installed on this machine fall back to the next one.',
  恢复默认: 'Reset defaults',
  '强调色 / 字体 / 字号回到默认（沙金 · 系统默认 · 13px）。': 'Reset accent, font, and size to defaults (gold · system default · 13px).',
  恢复: 'Reset',
  小满工作台: 'XiaoMan Box',
  '预览：Aa 123 · 界面字号': 'Preview: Aa 123 · UI font size',
  '的实际观感': 'actual look',

  // 强调色预设名
  沙金: 'Gold',
  青碧: 'Teal',
  天青: 'Azure',
  紫晶: 'Amethyst',
  橘焰: 'Ember',
  翠玉: 'Jade',

  // 字体预设名
  系统默认: 'System default',
  微软雅黑: 'Microsoft YaHei',
  思源黑体: 'Source Han Sans',
  '等宽（工程感）': 'Monospace (engineering)',
});
