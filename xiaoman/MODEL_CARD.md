# XiaoMan V6 量化候选模型卡

状态：模型卡和部署源码已公开；量化权重分片已在本地准备，尚未公开上传。这里只发布**量化 `.ninfer` 候选**；不发布 BF16 safetensors、独立 LoRA、训练样本或训练凭据。

| 制品 ID | 文件大小 GiB | 目标说明 | 当前验收 |
| --- | ---: | --- | --- |
| xiaoman-v6-sd |13.752|Blackwell 文本+MTP 候选|完整/分片SHA256完成；RTX5080真实生成与工具回环通过|
| xiaoman-v6-sd-vis |14.027|Blackwell 视觉候选|完整/分片SHA256完成；RTX5080文本与单图颜色冒烟通过|
| xiaoman-v6-fixed |15.312|Ada 文本候选|匹配引擎与硬件复验待补|
| xiaoman-v6-fixed-vis |15.921|Ada 视觉候选|匹配引擎与硬件复验待补|

精确字节数、校验值与未来下载 URL 见 ../release-manifest.json。URL 为 null 不表示已有公开下载链接。首版建议只发布完成实测的主线版本，其余留作内部候选。

## 来源与许可

训练材料记录基座 Qwen/Qwen3.8-27B，配置 model_type=qwen3_5；本地合并包含视觉塔与1层MTP。基座具体下载 revision、训练 checkpoint、视觉塔是否参与训练与教师/数据来源仍待补证。当前记录存在1 epoch与2 epoch不一致，未在此把任一值当作正式训练参数。

本次发布的小满量化权重采用 Apache-2.0，文本见 LICENSE-XiaoMan-Quantized.txt。上游 Qwen/Qwen3.8-27B 的官方模型卡同样标注 Apache-2.0：https://huggingface.co/Qwen/Qwen3.8-27B 。本地微调、合并和量化构成发布版本的修改；具体上游 revision 与训练来源记录仍需补齐，不把未核实的参数或成绩写成已确认事实。第三方推理引擎及 DLL 保持各自许可，模型权重许可不代替它们。

## 部署要求与限制

`.ninfer` 是对应 NInfer 运行时制品，并非可直接由 llama.cpp/Ollama/Transformers 读取的通用 GGUF 或 safetensors。使用匹配的 ninfer-serve 引擎、DLL 和 chat template。当前配置器只为 compute capability12.0、16GB+ 的 Blackwell GPU生成主线尝试配置，默认为8192上下文。RTX5080环境探测成功，但在显卡空闲时进行了隔离实测：2048上下文、host state 1、host KV 256 MiB；文本版显存约14710 MiB，视觉版约15054 MiB。文本生成与工具调用回环、视觉单图颜色冒烟通过，详见 validation/real-model-*.json；测试引擎均已停止。

文本、视觉、MTP、上下文扩大和并发都影响显存/内存。Ada两个文件名只表示内部候选；不承诺所有RTX40系列可用，也不使用Blackwell引擎替代匹配版本。

没有新增质量、速度、工具能力或视觉成绩；基础版集成测试采用模拟API，另补了真实模型工具回环。冒烟结果不构成模型质量、速度或泛化能力评测。发布前需记录GPU/驱动/引擎revision/参数/实际输出及资源消耗。

运行命令与手动下载流程见 ../人工部署说明.md。权重始终与桌面安装包分开。

## GitHub 分片发布

文本候选44片，视觉候选57片。已上传并验证的4片为1 GiB，其余新增片最多256 MiB；完整模型 SHA256 与原文件一致。只上传量化文件及说明；Ada候选暂无匹配硬件复验，首版保留在本地。实际公开地址以 release-manifest.json 为准；当前 URL 为空。运行时公开分发仍在整理，因此分片准备完成不等于完整本地推理部署已具备公开下载条件。
