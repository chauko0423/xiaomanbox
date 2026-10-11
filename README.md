# XiaoManBox 基础版

XiaoManBox 基础版提供本地 AI 对话、文件/命令工具、技能、MCP、UE 编辑器集成，以及无需账号的本地基础画板。账号系统、团队交流和成员发现已移除。画板由使用者自己的电脑托管，局域网内通过编辑/只读链接分享。

**本仓库提供基础版源码和部署工具。** 源码已构建并完成隔离集成验收。量化权重、Windows 安装包和推理引擎尚未提供公开下载；主线文本模型已完成真实生成与工具调用回环，视觉模型已完成文本与单图颜色冒烟测试（RTX 5080、2048上下文）；UE 真机复验尚未完成。量化候选只有文件清单与 SHA256，引擎及 DLL 的分发材料尚未补齐。

正式仓库：https://github.com/chauko0423/xiaomanbox 。点击 Code → Download ZIP，或使用 `git clone https://github.com/chauko0423/xiaomanbox.git` 获取源码。

## 从哪里开始

- 给 AI：把下载的仓库源码 ZIP（或作者另行提供的 `小满-AI部署包.zip`）交给 AI，要求先读 `AI部署入口.md`。
- 人工部署：读 `人工部署说明.md`。源码部署需要官方 Node.js 22+（含 npm）。
- Windows 安装包：尚未发布；当前请使用源码部署。无需小满权重，配置自己的模型 API 也能使用基础版。
- 使用小满量化模型：读 `xiaoman/MODEL_CARD.md`；公开仓库不含权重，校验值在 `release-manifest.json`；下载地址为 null 的文件尚未发布。
- 复验：`node tests/acceptance.mjs`。脚本使用独立数据目录及测试端口，不加载真实模型。

## 快速启动（源码方式）

```powershell
node deploy.mjs doctor
node deploy.mjs install
node deploy.mjs api-config --url http://127.0.0.1:8390/v1/chat/completions --model xiaoman
node deploy.mjs app
```

示例地址只有在你自己运行了对应 API 时才可用。也可在程序的“模型配置”选择自己提供的完整 chat/completions 地址与模型 ID；API key 只填入本机配置。

## 文件布局

| 路径 | 用途 |
| --- | --- |
| `xiaomanbox/` | 基础版 Vue/Electron/Node 源码、18 个 UE 技能、UnrealAgentLink 插件源码 |
| `local-board/` | 账户无关的基础白板、持久化协作服务 |
| `xiaoman/` | 量化模型卡、硬件要求和待发布说明 |
| `deploy.mjs`、三个 `.bat` | 探测、安装构建、配置、下载校验、启动/停止、健康检查 |
| `assets/` | 模板及校验说明；权重与引擎不在源码仓库内 |
| `release-manifest.json` | 文件大小、SHA256、兼容标记、未来下载 URL |
| `release-candidates/` | 作者本机的安装包候选目录；不在源码仓库内 |
| `validation/` | 本次验收记录 |

部署数据默认存入本包 `.runtime/`；直接运行安装版时默认为用户目录 `.xiaomanbox/`。不会读入原工程 `.workbench/` 的账户、密钥和会话。

## 联网行为

对话请求只发往使用者选定的模型 API；画板使用同一台本地服务的 HTTP/WebSocket。文件、UE 和 AI 后端默认只监听 127.0.0.1。安装依赖会访问 npm/Electron 下载源；使用 web 搜索/抓取、外部 MCP、DSH 或自备远程 API 时，按该功能访问相应第三方地址。没有原作者服务器的回退连接。

## 发布边界

自有源码采用 Apache-2.0，见 `LICENSE`。第三方保留自己的许可，详见 `THIRD-PARTY-NOTICES.md`。不要把根目录 LICENSE 理解为所有模型、DLL、字体和插件都由此重新授权。正式上传前见 `发布前待办.md`。

## GitHub 量化分片

主线文本与视觉候选按每片最多1 GiB准备，清单含逐片和完整文件 SHA256。当前分片尚未公开上传，URL 为 null；不会自动下载不存在的文件。发布后 `node deploy.mjs download --id xiaoman-v6-sd` 将自动下载、校验并合并，完成的分片可复用。手动下载全部分片后，用 `--parts-dir <分片目录>` 合并。详见 `xiaoman/GITHUB_RELEASE.md`。
