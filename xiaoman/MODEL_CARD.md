# XiaoMan V6 量化候选模型卡

小满 V6 是经专项训练、专门适配 UE5 的垂直 AI 助手，强调领域深度与强泛化能力的结合。完整产品定位与技术亮点见 [小满 V6 介绍](INTRODUCTION.md)；本模型卡记录制品、来源和实际验证边界。

发布内容：主线文本与视觉量化权重分片、校验清单及部署说明。这里只发布**量化 `.ninfer` 候选**；不发布 BF16 safetensors、独立 LoRA、训练样本或训练凭据。

| 制品 ID | 文件大小 GiB | 目标说明 | 当前验收 |
| --- | ---: | --- | --- |
| xiaoman-v6-sd |13.752|Blackwell 文本+MTP 候选|完整/分片SHA256完成；RTX5080真实生成与工具回环通过|
| xiaoman-v6-sd-vis |14.027|Blackwell 视觉候选|完整/分片SHA256完成；RTX5080文本与单图颜色冒烟通过|
| xiaoman-v6-fixed |15.312|Ada 文本候选|匹配引擎与硬件复验待补|
| xiaoman-v6-fixed-vis |15.921|Ada 视觉候选|匹配引擎与硬件复验待补|

精确字节数、校验值与下载 URL 见 ../release-manifest.json。URL 为 null 不表示已有公开下载链接。首版只公开完成上述实测的主线版本，Ada 两份保留为内部候选。

## 来源与许可

训练材料记录基座 Qwen/Qwen3.8-27B，配置 model_type=qwen3_5；本地合并包含视觉塔与1层MTP。基座具体下载 revision、训练 checkpoint、视觉塔是否参与训练与教师/数据来源仍待补证。当前记录存在1 epoch与2 epoch不一致，未在此把任一值当作正式训练参数。

本次发布的小满量化权重采用 Apache-2.0，文本见 LICENSE-XiaoMan-Quantized.txt。上游 Qwen/Qwen3.8-27B 的官方模型卡同样标注 Apache-2.0：https://huggingface.co/Qwen/Qwen3.8-27B 。本地微调、合并和量化构成发布版本的修改；具体上游 revision 与训练来源记录仍需补齐，不把未核实的参数或成绩写成已确认事实。第三方推理引擎及 DLL 保持各自许可，模型权重许可不代替它们。

## 部署要求与限制

`.ninfer` 是对应 NInfer 运行时制品，并非可直接由 llama.cpp/Ollama/Transformers 读取的通用 GGUF 或 safetensors。使用匹配的 ninfer-serve 引擎、DLL 和 chat template。当前配置器只为 compute capability12.0、16GB+ 的 Blackwell GPU生成主线尝试配置，主线文本与视觉默认采用已复验的2048上下文、host state 1、host KV 256MiB；扩大上下文与并发需另行验证。RTX5080环境探测成功，但在显卡空闲时进行了隔离实测：2048上下文、host state 1、host KV 256 MiB；文本版显存约14710 MiB，视觉版约15054 MiB。文本生成与工具调用回环、视觉单图颜色冒烟通过，详见 validation/real-model-*.json；测试引擎均已停止。

文本、视觉、MTP、上下文扩大和并发都影响显存/内存。Ada两个文件名只表示内部候选；不承诺所有RTX40系列可用，也不使用Blackwell引擎替代匹配版本。

没有新增质量、速度、工具能力或视觉成绩；基础版集成测试采用模拟API，另补了真实模型工具回环。冒烟结果不构成模型质量、速度或泛化能力评测。发布前需记录GPU/驱动/引擎revision/参数/实际输出及资源消耗。

运行命令与手动下载流程见 ../人工部署说明.md。权重始终与桌面安装包分开。

## GitHub 分片发布

文本候选44片，视觉候选57片。已上传并验证的4片为1 GiB，其余新增片最多256 MiB；完整模型 SHA256 与原文件一致。只上传量化文件及说明；Ada候选暂无匹配硬件复验，首版保留在本地。实际公开地址以 release-manifest.json 为准；公开分片的精确下载 URL、字节数与校验值在 release-manifest.json。主线文本推理包已独立公开：https://github.com/chauko0423/xiaomanbox/releases/tag/ninfer-blackwell-text-v1 。可通过部署工具下载文本引擎与许可说明，再配置和启动本地服务。视觉引擎也已独立公开：https://github.com/chauko0423/xiaomanbox/releases/tag/ninfer-blackwell-vision-v1 。采用固定FFmpeg9.0/zlib1.3.1源码构建的精简LGPL共享库，完整对应源码和许可证随Release提供；RTX5080/2048上下文下通过文本生成、PNG/JPEG颜色识别及短H264视频输入冒烟，新视觉引擎显存读数约15179MiB。Ada候选仍待匹配硬件实测；文本引擎不用于图像或视频输入。


Basic.5 工作台提供按轮工具限制与上下文估算检查。默认2048并不适合完整UE工具清单；扩大窗口需单独确认资源。Basic.6 已通过真实小满模型的 Actor 与基础蓝图回环；详见 ../小满UE模型实测说明.md，其他工作流仍待单独验证。

Basic.7 在RTX5080/UE5.7.4 NullRHI、4096文本窗口下通过模型编写事件图与函数图、节点执行连线、字符串常量及编译保存；默认配置仍为2048。详见../小满UE节点图实测说明.md，不代表复杂游戏逻辑运行时或全部工具均已验证。
