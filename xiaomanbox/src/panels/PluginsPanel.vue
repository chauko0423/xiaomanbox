<script setup lang="ts">
import { onMounted, ref } from 'vue';
import { listMcpServers, addMcpServer, removeMcpServer, type McpServerInfo } from '../api/mcp';
import { listWorkbenchTools, toggleTool, type WorkbenchTool } from '../api/plugins';
import { listSkills, loadSkill, deleteSkill, type SkillMeta } from '../api/skills';
import { post } from '../api/client';
import { t, isEnglish } from '../i18n';

/**
 * 插件（2026-10-09 重构：三形态一个入口）：
 *   Tab1 能力工具 = 小满自研的 20 个 agent 工具，每个「启用/停用」开关（enabled-tools.json）
 *   Tab2 Skill 包  = 专家指令集（SKILL.md），装/卸，中文摘要跟随语言
 *   Tab3 MCP 连接器 = 接外部 MCP 服务器（stdio/http），装/卸
 * 统一：一个「插件」页看全部，装/卸/启停一致。
 */
const tab = ref<'tools' | 'skills' | 'mcp'>('tools');

// ── Tab1 能力工具 ─────────────────────────────────────────
const workbenchTools = ref<WorkbenchTool[]>([]);
const toolsErr = ref<string | null>(null);
const toolsLoading = ref(true);

async function loadTools() {
  toolsLoading.value = true;
  try {
    const r = await listWorkbenchTools();
    workbenchTools.value = r.tools || [];
    toolsErr.value = null;
  } catch (e) {
    toolsErr.value = String((e as Error).message || e);
  } finally {
    toolsLoading.value = false;
  }
}

async function toggle(tool: WorkbenchTool) {
  try {
    await toggleTool(tool.name, !tool.enabled);
    await loadTools();
  } catch { /* 静默 */ }
}

// ── Tab2 Skill 包 ─────────────────────────────────────────
const skills = ref<SkillMeta[]>([]);
const skillLoading = ref(true);
const skillErr = ref<string | null>(null);
const skillNotice = ref<string | null>(null);

const openBody = ref<string | null>(null);
const bodyCache = ref<Record<string, string>>({});
const bodyLoading = ref(false);

const installing = ref(false);
const form = ref({ name: '', content: '' });
const formErr = ref<string | null>(null);
const formIssues = ref<string[]>([]);
const saving = ref(false);

const deleting = ref<string | null>(null);
const confirmDelete = ref<string | null>(null);

async function refreshSkills() {
  skillLoading.value = true;
  try {
    const r = await listSkills();
    skills.value = r.skills || [];
    if (r.errors?.length) {
      skillErr.value = `${r.errors.length} 个 skill 元数据损坏：` + r.errors.map((e) => `${e.skill}（${e.error}）`).join('、');
    } else {
      skillErr.value = null;
    }
  } catch (e) {
    skillErr.value = String((e as Error).message || e);
  } finally {
    skillLoading.value = false;
  }
}

async function toggleBody(name: string) {
  if (openBody.value === name) {
    openBody.value = null;
    return;
  }
  openBody.value = name;
  if (bodyCache.value[name]) return;
  bodyLoading.value = true;
  try {
    const r = await loadSkill(name);
    if (r.ok && r.skill) bodyCache.value[name] = r.skill.body;
    else bodyCache.value[name] = `（加载失败：${r.error ?? '未知错误'}）`;
  } catch (e) {
    bodyCache.value[name] = `（加载失败：${String((e as Error).message || e)}）`;
  } finally {
    bodyLoading.value = false;
  }
}

async function installSkill() {
  if (saving.value) return;
  saving.value = true;
  formErr.value = null;
  formIssues.value = [];
  try {
    const r = await post<{ ok: boolean; error?: string; issues?: string[]; code?: number }>('/skills/install', {
      name: form.value.name,
      content: form.value.content,
    });
    if (!r.ok) {
      formIssues.value = r.issues || [];
      throw new Error(r.error || `HTTP 错误 ${r.code}`);
    }
    skillNotice.value = `已安装「${form.value.name}」`;
    installing.value = false;
    form.value = { name: '', content: '' };
    await refreshSkills();
  } catch (e) {
    formErr.value = String((e as Error).message || e);
  } finally {
    saving.value = false;
  }
}

async function doDeleteSkill(name: string) {
  if (deleting.value) return;
  deleting.value = name;
  try {
    const r = await deleteSkill(name);
    if (!r.ok) {
      skillErr.value = r.error || `删除失败（HTTP ${r.code}）`;
    } else {
      confirmDelete.value = null;
      skillNotice.value = `已删除「${name}」`;
      await refreshSkills();
    }
  } catch (e) {
    skillErr.value = String((e as Error).message || e);
  } finally {
    deleting.value = null;
  }
}

/** skill 卡片描述：跟随语言——中文模式显示中文摘要，英文模式显示英文首句 */
function descOf(s: SkillMeta): string {
  if (isEnglish.value) {
    const first = (s.description || '').split(/[.!?]\s/)[0] || '';
    return first ? first + '…' : '(no description)';
  }
  if (s.titleZh) return s.titleZh;
  const first = (s.description || '').split(/[.!?]\s/)[0] || '';
  return first ? first + '…' : t('（无描述）');
}

// ── Tab3 MCP 连接器 ───────────────────────────────────────
const servers = ref<McpServerInfo[]>([]);
const mcpErr = ref<string | null>(null);
const mcpLoading = ref(true);
const mcpAdding = ref(false);
const mcpForm = ref({ name: '', transport: 'stdio' as 'stdio' | 'http', command: '', url: '' });
const mcpFormErr = ref<string | null>(null);
const mcpRemoving = ref<string | null>(null);

async function loadMcp() {
  mcpLoading.value = true;
  try {
    const r = await listMcpServers();
    servers.value = r.servers || [];
    mcpErr.value = null;
  } catch (e) {
    mcpErr.value = String((e as Error).message || e);
  } finally {
    mcpLoading.value = false;
  }
}

async function addServer() {
  mcpFormErr.value = null;
  try {
    const cfgBody: { transport: 'stdio' | 'http'; command?: string; url?: string } = { transport: mcpForm.value.transport };
    if (mcpForm.value.transport === 'stdio') cfgBody.command = mcpForm.value.command;
    else cfgBody.url = mcpForm.value.url;
    const r = await addMcpServer(mcpForm.value.name, cfgBody);
    if (!r.ok) throw new Error(r.error || `HTTP 错误 ${r.code}`);
    mcpAdding.value = false;
    mcpForm.value = { name: '', transport: 'stdio', command: '', url: '' };
    await loadMcp();
  } catch (e) {
    mcpFormErr.value = String((e as Error).message || e);
  }
}

async function removeServer(name: string) {
  if (mcpRemoving.value) return;
  mcpRemoving.value = name;
  try {
    const r = await removeMcpServer(name);
    if (!r.ok) mcpErr.value = r.error || `删除失败（HTTP ${r.code}）`;
    await loadMcp();
  } catch (e) {
    mcpErr.value = String((e as Error).message || e);
  } finally {
    mcpRemoving.value = null;
  }
}

onMounted(() => {
  loadTools();
  refreshSkills();
  loadMcp();
});
</script>

<template>
  <div class="plugins">
    <p class="lead">{{ t('插件 = 三种形态，一个入口：能力工具（自研）、Skill 包（专家指令）、MCP 连接器（接外部）。统一装 / 卸 / 启停。') }}</p>

    <div class="tabs">
      <button class="tab" :class="{ on: tab === 'tools' }" @click="tab = 'tools'">{{ t('能力工具') }} <span class="cnt">{{ workbenchTools.filter((x) => x.enabled).length }}/{{ workbenchTools.length }}</span></button>
      <button class="tab" :class="{ on: tab === 'skills' }" @click="tab = 'skills'">{{ t('Skill 包') }} <span class="cnt">{{ skills.length }}</span></button>
      <button class="tab" :class="{ on: tab === 'mcp' }" @click="tab = 'mcp'">{{ t('MCP 连接器') }} <span class="cnt">{{ servers.length }}</span></button>
    </div>

    <!-- ══ Tab1 能力工具 ══ -->
    <div v-if="tab === 'tools'" class="pane">
      <div v-if="toolsErr" class="err block">{{ toolsErr }}</div>
      <div v-if="toolsLoading" class="empty">{{ t('加载中…') }}</div>
      <div v-else class="tool-list">
        <div v-for="tool in workbenchTools" :key="tool.name" class="tool-row">
          <div class="tool-txt">
            <span class="tool-name">{{ tool.name }}</span>
            <span class="tool-dsh">dsh: {{ tool.dsh }}</span>
            <span class="tool-label">{{ tool.label }}</span>
          </div>
          <button
            class="toggle"
            :class="{ on: tool.enabled }"
            :title="tool.enabled ? t('点击停用（不再传给小满）') : t('点击启用（传给小满当工具）')"
            @click="toggle(tool)"
          >
            <span class="knob"></span>
          </button>
        </div>
      </div>
    </div>

    <!-- ══ Tab2 Skill 包 ══ -->
    <div v-else-if="tab === 'skills'" class="pane">
      <div v-if="skillErr" class="err block">{{ skillErr }}</div>
      <div v-else-if="skillNotice" class="notice block">{{ skillNotice }}</div>

      <div v-if="skillLoading" class="empty">{{ t('加载中…') }}</div>
      <div v-else class="skill-list">
        <div v-for="s in skills" :key="s.name" class="skill-card" :class="{ open: openBody === s.name }">
          <button class="card-head" @click="toggleBody(s.name)">
            <span class="card-titles">
              <span class="card-name">{{ s.name }}</span>
              <span class="card-desc">{{ descOf(s) }}</span>
            </span>
            <span class="chev">⌄</span>
          </button>
          <div v-if="openBody === s.name" class="card-body">
            <pre v-if="bodyCache[s.name]">{{ bodyCache[s.name] }}</pre>
            <span v-else class="dim">{{ t('加载中…') }}</span>
          </div>
          <div class="card-foot">
            <span class="card-id">{{ s.name }}</span>
            <span class="spacer"></span>
            <button v-if="confirmDelete !== s.name" class="del-btn" @click.stop="confirmDelete = s.name">{{ t('删除') }}</button>
            <template v-else>
              <span class="del-confirm">{{ t('确认删除？') }}</span>
              <button class="del-btn danger" :disabled="deleting === s.name" @click.stop="doDeleteSkill(s.name)">{{ deleting === s.name ? t('删除中…') : t('删除') }}</button>
              <button class="del-btn" @click.stop="confirmDelete = null">{{ t('取消') }}</button>
            </template>
          </div>
        </div>
        <div v-if="!skills.length" class="empty">{{ t('还没有安装任何 skill —— 用下面的按钮装第一个。') }}</div>
      </div>

      <button v-if="!installing" class="add-btn" @click="installing = true">{{ t('+ 安装新 skill') }}</button>
      <div v-else class="install">
        <div class="install-row">
          <label>{{ t('skill 名') }}<input v-model="form.name" type="text" :placeholder="t('小写/数字/连字符，如 ue5-blueprint-tips')" /></label>
        </div>
        <label>{{ t('SKILL.md 全文（含 front-matter）') }}
          <textarea v-model="form.content" rows="10" :placeholder="t('---\nname: my-skill\ndescription: 一句话描述\n---\n\n# 步骤\n1. ...')"></textarea>
        </label>
        <div v-if="formErr" class="form-err">
          {{ formErr }}
          <ul v-if="formIssues.length">
            <li v-for="(i, idx) in formIssues" :key="idx">{{ i }}</li>
          </ul>
        </div>
        <div class="form-actions">
          <button class="btn-inline primary" :disabled="saving || !form.name || !form.content" @click="installSkill">{{ saving ? t('安装中…') : t('安装') }}</button>
          <button class="btn-inline" @click="installing = false">{{ t('取消') }}</button>
        </div>
      </div>
    </div>

    <!-- ══ Tab3 MCP 连接器 ══ -->
    <div v-else class="pane">
      <div v-if="mcpErr" class="err block">{{ mcpErr }}</div>
      <div v-if="mcpLoading" class="empty">{{ t('加载中…') }}</div>
      <div v-else class="mcp-list">
        <div v-for="s in servers" :key="s.name" class="mcp">
          <span class="mcp-name">{{ s.name }}</span>
          <span class="mini-tag">{{ s.transport }}</span>
          <span class="mcp-detail">{{ s.transport === 'stdio' ? s.command : s.url }}</span>
          <span class="spacer"></span>
          <button class="del-btn" :disabled="mcpRemoving === s.name" @click="removeServer(s.name)">{{ mcpRemoving === s.name ? t('删除中…') : t('删除') }}</button>
        </div>
        <div v-if="!servers.length" class="empty">{{ t('尚未接入 MCP 服务器 —— 团队画板等能力从这里挂进来。') }}</div>

        <button v-if="!mcpAdding" class="add-btn" @click="mcpAdding = true">{{ t('+ 添加 MCP 服务器') }}</button>
        <div v-else class="mcp new">
          <div class="mcp-form">
            <label>{{ t('名称') }}<input v-model="mcpForm.name" type="text" :placeholder="t('小写/数字/连字符，如 team-board')" /></label>
            <label>{{ t('传输') }}
              <select v-model="mcpForm.transport">
                <option value="stdio">stdio（{{ t('本地子进程') }}）</option>
                <option value="http">http（{{ t('远程地址') }}）</option>
              </select>
            </label>
            <label v-if="mcpForm.transport === 'stdio'">{{ t('命令') }}<input v-model="mcpForm.command" type="text" placeholder="如 node D:/path/to/server.js" /></label>
            <label v-else>{{ t('地址') }}<input v-model="mcpForm.url" type="text" placeholder="http://127.0.0.1:9999/mcp" /></label>
            <div v-if="mcpFormErr" class="form-err">{{ mcpFormErr }}</div>
            <div class="form-actions">
              <button class="btn-inline primary" @click="addServer">{{ t('添加') }}</button>
              <button class="btn-inline" @click="mcpAdding = false">{{ t('取消') }}</button>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.plugins { display: flex; flex-direction: column; gap: 14px; max-width: 820px; }
.lead { margin: 0; font-size: 0.962rem; color: var(--dim); }

/* Tab 行 */
.tabs { display: flex; gap: 4px; border-bottom: 1px solid var(--border); }
.tab {
  background: transparent;
  border: none;
  border-bottom: 2px solid transparent;
  padding: 8px 14px;
  font-size: 0.923rem;
  color: var(--dim);
  cursor: pointer;
  display: inline-flex;
  align-items: center;
  gap: 6px;
}
.tab:hover { color: var(--ink); }
.tab.on { color: var(--ink); border-bottom-color: var(--caution); font-weight: 600; }
.cnt { font-size: 0.769rem; color: var(--dim-2); font-family: var(--font-num); }

.pane { display: flex; flex-direction: column; gap: 12px; }

/* 能力工具 */
.tool-list { display: flex; flex-direction: column; gap: 8px; }
.tool-row {
  display: flex;
  align-items: center;
  gap: 12px;
  background: var(--panel-bg-2);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 10px 14px;
}
.tool-txt { display: flex; align-items: baseline; gap: 10px; flex: 1; min-width: 0; }
.tool-name { font-family: var(--font-num); font-size: 0.923rem; color: var(--ink); }
.tool-dsh { font-family: var(--font-num); font-size: 0.769rem; color: var(--dim-2); }
.tool-label { font-size: 0.846rem; color: var(--dim); }
.toggle {
  flex: none;
  width: 40px;
  height: 22px;
  border-radius: 11px;
  background: var(--border);
  border: none;
  cursor: pointer;
  position: relative;
  transition: background 0.15s ease;
}
.toggle.on { background: var(--caution); }
.knob {
  position: absolute;
  top: 2px;
  left: 2px;
  width: 18px;
  height: 18px;
  border-radius: 50%;
  background: #fff;
  transition: left 0.15s ease;
}
.toggle.on .knob { left: 20px; }

/* skill 卡片 */
.skill-list { display: flex; flex-direction: column; gap: 10px; }
.skill-card {
  background: var(--panel-bg-2);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  overflow: hidden;
}
.skill-card:hover { border-color: var(--border-bright); }
.skill-card.open { border-color: var(--border-bright); }
.card-head {
  width: 100%;
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 10px;
  background: transparent;
  border: none;
  padding: 12px 16px;
  cursor: pointer;
  text-align: left;
}
.card-titles { display: flex; flex-direction: column; gap: 4px; min-width: 0; }
.card-name { font-family: var(--font-num); font-size: 1rem; font-weight: 600; color: var(--ink); }
.card-desc { font-size: 0.885rem; color: var(--dim); line-height: 1.5; }
.chev { color: var(--dim-2); font-size: 1.154rem; flex: none; }
.card-body { border-top: 1px solid var(--border); padding: 12px 16px; max-height: 260px; overflow-y: auto; }
.card-body pre { margin: 0; font-family: var(--font-num); font-size: 0.846rem; line-height: 1.6; color: var(--dim); white-space: pre-wrap; word-break: break-word; }
.card-foot { border-top: 1px solid var(--border); padding: 8px 16px; display: flex; align-items: center; gap: 8px; }
.card-id { font-family: var(--font-num); font-size: 0.769rem; color: var(--ink-3); }
.spacer { flex: 1; }

/* MCP */
.mcp-list { display: flex; flex-direction: column; gap: 10px; }
.mcp {
  display: flex;
  align-items: center;
  gap: 10px;
  background: var(--panel-bg-2);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 10px 14px;
}
.mcp.new { border-style: dashed; }
.mcp-name { font-family: var(--font-num); font-size: 0.923rem; color: var(--ink); }
.mcp-detail { font-family: var(--font-num); font-size: 0.808rem; color: var(--ink-3); flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.mcp-form { display: flex; flex-direction: column; gap: 10px; }
.mcp-form label { display: flex; flex-direction: column; gap: 4px; font-size: 0.846rem; color: var(--dim); }

/* 通用 */
.mini-tag { font-size: 0.769rem; color: var(--dim); border: 1px solid var(--border-bright); border-radius: var(--radius-sm); padding: 0 6px; flex: none; }
.del-btn { flex: none; font-size: 0.769rem; color: var(--dim); background: none; border: 1px solid var(--border-bright); border-radius: 4px; padding: 2px 8px; cursor: pointer; }
.del-btn:hover { color: var(--ink); border-color: var(--dim); }
.del-btn.danger { color: var(--err); border-color: var(--err); }
.del-btn:disabled { opacity: 0.5; cursor: not-allowed; }
.del-confirm { font-size: 0.769rem; color: var(--err); }

.install { display: flex; flex-direction: column; gap: 10px; background: var(--panel-bg-2); border: 1px dashed var(--border-bright); border-radius: var(--radius); padding: 16px; }
.install-row { display: flex; }
.install label { display: flex; flex-direction: column; gap: 4px; font-size: 0.846rem; color: var(--dim); flex: 1; }
.install textarea { width: 100%; font-family: var(--font-num); font-size: 0.885rem; line-height: 1.6; resize: vertical; }
.form-err { font-size: 0.885rem; color: var(--err); }
.form-err ul { margin: 4px 0 0; padding-left: 18px; }
.form-actions { display: flex; gap: 8px; }

.add-btn { width: 100%; background: transparent; border: 1px dashed var(--border-bright); border-radius: var(--radius); color: var(--dim); font-size: 1rem; padding: 14px 0; cursor: pointer; }
.add-btn:hover { color: var(--ink); border-color: var(--caution); }

.btn-inline.primary { background: var(--caution); border-color: var(--caution); color: #000; font-weight: 600; }
.btn-inline.primary:hover { background: var(--caution-deep); border-color: var(--caution-deep); color: #000; }
.btn-inline:disabled { opacity: 0.5; cursor: not-allowed; }

.notice { color: var(--ok); }
.block { font-size: 0.923rem; padding: 8px 12px; border: 1px solid var(--border); border-radius: var(--radius-sm); }
.err { color: var(--err); border-color: var(--err); }
.empty { color: var(--dim-2); font-size: 0.923rem; }
.dim { color: var(--dim-2); }
</style>
