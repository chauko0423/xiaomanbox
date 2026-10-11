# XiaoMan V6 量化候选模型卡

状态：本地准备，尚未正式发布。这里只发布**量化 `.ninfer` 候选**；不发布 BF16 safetensors、独立 LoRA、训练样本或训练凭据。

| 制品 ID | 文件大小 GiB | 目标说明 | 当前验收 |
| --- | ---: | --- | --- |
| xiaoman-v6-sd |13.752|Blackwell 文本+MTP 候选|复制/大小/SHA256完成，真实推理待复验|
| xiaoman-v6-sd-vis |14.027|Blackwell 视觉候选|同上，图片推理待复验|
| xiaoman-v6-fixed |15.312|Ada 文本候选|匹配引擎与硬件复验待补|
| xiaoman-v6-fixed-vis |15.921|Ada 视觉候选|匹配引擎与硬件复验待补|

精确字节数、校验值与未来下载 URL 见 ../release-manifest.json。URL 为 null 不表示已有公开下载链接。首版建议只发布完成实测的主线版本，其余留作内部候选。

## 来源与许可

训练材料记录基座 Qwen/Qwen3.8-27B，配置 model_type=qwen3_5；本地合并包含视觉塔与1层MTP。基座具体下载 revision、训练 checkpoint、视觉塔是否参与训练与教师/数据来源仍待补证。当前记录存在1 epoch与2 epoch不一致，未在此把任一值当作正式训练参数。

模型授权文本待作者确认并补齐上游材料。根目录 Apache LICENSE 是代码整理方案，不能代替模型卡许可或所有依赖的许可。不得宣称未经记录的商业授权、成绩或能力。

## 部署要求与限制

`.ninfer` 是对应 NInfer 运行时制品，并非可直接由 llama.cpp/Ollama/Transformers 读取的通用 GGUF 或 safetensors。使用匹配的 ninfer-serve 引擎、DLL 和 chat template。当前配置器只为 compute capability12.0、16GB+ 的 Blackwell GPU生成主线尝试配置，默认为8192上下文。RTX5080环境探测成功，但本轮未加载大模型以避免干扰正在运行的工程服务；不能由探测结果声称推理已经成功。

文本、视觉、MTP、上下文扩大和并发都影响显存/内存。Ada两个文件名只表示内部候选；不承诺所有RTX40系列可用，也不使用Blackwell引擎替代匹配版本。

没有新增质量、速度、工具能力或视觉成绩；本轮基础版文件工具测试使用模拟API，完全不构成小满模型评测。发布前需记录GPU/驱动/引擎revision/参数/实际输出及资源消耗。

运行命令与手动下载流程见 ../人工部署说明.md。权重始终与桌面安装包分开。
