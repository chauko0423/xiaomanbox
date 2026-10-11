# UnrealAgentLink 插件 —— XiaoMan Box 自研 fork

## 来源

- **上游**：UEBox 开源仓库 `ueboxai/uebox`，路径 `plugin/UnrealAgentLink`
- **上游许可**：MIT（见 `LICENSE`）
- **fork 时间**：2026-10-07（对应上游版本 1.2.11）
- **来源检查基线**：`ueboxai/uebox@e3755983a3e867f05bdec03f6f3d2b12310795f8`（插件包含本地修改，不能视为该提交的原样副本）

## 为什么 fork

XiaoMan Box 要「彻底摆脱 UEBox 桌面程序依赖」，把 UE 编辑器桥接能力内置进工作台。
UnrealAgentLink 插件是 WebSocket **客户端**，主动连 `ws://127.0.0.1:17860`，
它不关心对面是谁——工作台后端起自己的 WS 服务端（`backend/uebridge.mjs` + `ual.mjs`，
零外部依赖）即可直接驱动它。

fork 的目的：作为工作台**自研插件**长期维护，可自由改造、重命名、加能力，
不再跟随 UEBox 上游节奏。

## 架构关系

```
XiaoMan Box 后端 (ual.mjs → uebridge.mjs, WS 服务端 @17860)
        ↑ 插件主动连 (WebSocket 客户端)
UnrealAgentLink 插件 (本目录, 装在 UE 工程 Plugins/ 下)
        ↑
Unreal 编辑器 (UE5.0 ~ 5.8)
```

## 自研计划（按优先级）

1. [ ] 重命名插件（FriendlyName / 模块名 / 命名空间）—— 脱离 UEBox 品牌
2. [ ] 按需裁剪（去掉工作台用不到的能力，瘦身）
3. [ ] 补上工作台特有的工具（对齐 ual-tools.mjs 的 163 个工具清单）

## 注意

- 插件当前已装在 `D:\UE5_7_4\Projects\Xiaoman\Plugins\UnrealAgentLink`（可替换为本 fork 构建产物）
- 插件断线后每 5 秒自动重连（UAL_NetworkManager.cpp）
- 命令行 `-UALServer=ws://host:port` 可覆盖默认连接地址（测试用）
