# 第三方归属与分发状态

自有基础版代码暂按 Apache-2.0 整理，许可选择待作者最终确认。以下第三方保持自身许可；根 LICENSE 不替代它们。

| 内容 | 来源 | 本地处理 |
| --- | --- | --- |
| UnrealAgentLink 插件及借鉴的 UE 桥协议 | UEBox / ueboxai/uebox，plugin/UnrealAgentLink | 保留 plugin/UnrealAgentLink/LICENSE（MIT）、README 与 FORK；上游版本记录1.2.11，来源检查基线 e3755983a3e867f05bdec03f6f3d2b12310795f8，插件有本地修改 |
| React、Excalidraw、Hocuspocus、Yjs、Dagre | npm 对应包，锁文件记录版本/完整性 | 可找到的包许可存 third-party-licenses；静态 Excalidraw 资源保留 vendor LICENSE.txt；Virgil/Cascadia/Assistant 的 OFL 与 Excalidraw/Hocuspocus 的 MIT 文本已从上游补齐，来源和校验见 third-party-licenses/sources.json |
| Vue、Electron 与构建工具 | npm 对应包 | 源码由锁文件安装；Electron 二进制保留自带 LICENSE/LICENSES.chromium.html；打包依赖材料需最终整理 |
| 小满基座 | 本地训练记录指向 Qwen/Qwen3.8-27B | 官方模型卡标注 Apache-2.0；具体 revision 与来源材料待核实，模型卡有明确 pending，不附完整权重 |
| NInfer 与 FFmpeg DLL | 当前本机构建产物 | 内部候选；当前无法仅凭产物确认全部构建选项与完整分发材料，不放入源码 ZIP |

没有包含外部 NodeUE 节点模板数据、桌宠角色 PSD、SenseVoice 语音模型、训练数据与教师输出样本。18 个 UE 技能保留原引用，描述与插件源码应继续进行最终人工来源审阅。可选联网搜索、外部 API/MCP/DSH 不表示本项目附送这些服务、商业模型或第三方插件代码。
