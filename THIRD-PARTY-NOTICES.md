# 第三方归属与分发状态

自有基础版代码采用 Apache-2.0。以下第三方保持自身许可；根 LICENSE 不替代它们。

| 内容 | 来源 | 本地处理 |
| --- | --- | --- |
| UnrealAgentLink 插件及借鉴的 UE 桥协议 | UEBox / ueboxai/uebox，plugin/UnrealAgentLink | 保留 plugin/UnrealAgentLink/LICENSE（MIT）、README 与 FORK；上游版本记录1.2.11，来源检查基线 e3755983a3e867f05bdec03f6f3d2b12310795f8，插件有本地修改 |
| React、Excalidraw、Hocuspocus、Yjs、Dagre | npm 对应包，锁文件记录版本/完整性 | 可找到的包许可存 third-party-licenses；静态 Excalidraw 资源保留 vendor LICENSE.txt；Virgil/Cascadia/Assistant 的 OFL 与 Excalidraw/Hocuspocus 的 MIT 文本已从上游补齐，来源和校验见 third-party-licenses/sources.json |
| Vue、Electron 与构建工具 | npm 对应包 | 源码由锁文件安装；Electron 二进制保留自带 LICENSE/LICENSES.chromium.html；452项锁定依赖的许可文档/声明清单见third-party-licenses/npm-inventory.json；其中440项保留完整许可文本，12项只用于构建且不随安装包分发的包保留原npm许可声明 |
| 小满基座 | 本地训练记录指向 Qwen/Qwen3.8-27B | 官方模型卡标注 Apache-2.0；具体 revision 与来源材料待核实，模型卡有明确 pending，不附完整权重 |
| NInfer 与 FFmpeg DLL | Neroued/ninfer 与 BtbN/FFmpeg-Builds | 既有Windows视觉候选实际来自带本地修改的fork源码，localBaseCommit为6e8d41e685a16a58b94b199ac6b25fd02b67a241；之前记录的干净上游仓库不构成二进制来源证明，修正见validation/runtime-provenance-correction.json；FFmpeg 候选为 N-126856-ged27b2c498-20260925，启用 GPL 与 version3。7份DLL均与原包哈希一致，配置记录见 validation/runtime-provenance.json，候选 GPL 文本见 third-party-licenses/FFmpeg-candidate-GPLv3.txt。完整对应来源和依赖材料仍待补齐；二进制不在源码 ZIP |

没有包含外部 NodeUE 节点模板数据、桌宠角色 PSD、SenseVoice 语音模型、训练数据与教师输出样本。18 个 UE 技能保留原引用，描述与插件源码应继续进行最终人工来源审阅。可选联网搜索、外部 API/MCP/DSH 不表示本项目附送这些服务、商业模型或第三方插件代码。

## 独立文本推理包

[Blackwell文本推理包](https://github.com/chauko0423/xiaomanbox/releases/tag/ninfer-blackwell-text-v1)附带完整构建源码快照、逐文件摘要、Windows构建脚本和许可证。以本地fork快照构建，关闭FFmpeg解码，静态链接CUDA与MSVC运行库。NInfer/llama-jinja/cpp-httplib/nlohmann-json/spdlog/utf8proc保留各自许可，NVTX采用Apache-2.0带LLVM例外；NVIDIA CUDA部分保持NVIDIA条款，不由根目录Apache许可重新授权。许可汇总见third-party-licenses/ninfer-runtime/。

## 独立视觉推理包

[Blackwell视觉推理包](https://github.com/chauko0423/xiaomanbox/releases/tag/ninfer-blackwell-vision-v1)使用自行构建的FFmpeg9.0 LGPL2.1-or-later共享库，关闭GPL、nonfree与version3选项，zlib1.3.1静态链接。4份DLL合计约5.5MiB；完整FFmpeg/zlib对应源码、修改补丁、生成配置与Windows构建脚本在同一Release提供。许可文本见third-party-licenses/ffmpeg-vision/。此新包不分发旧的BtbN GPL候选；NInfer及NVIDIA部分仍保持各自许可。FFmpeg动态库可按许可修改和替换，需维护兼容ABI与自己的摘要清单。

安装包显式附带第三方许可目录、归属说明和自有源码许可；Electron自带LICENSE及Chromium归属页面保留在安装目录。6份字体与锁定Excalidraw 0.17.6资源逐字节一致，对应OFL文本随安装包分发。记录见 validation/third-party-licenses.json。
