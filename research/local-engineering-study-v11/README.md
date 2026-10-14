# Local engineering study v11 code and data

[English study scope](reading/en/01_Study_scope.md) | [中文研究范围](reading/zh/01_Study_scope.md)

Complete sixty-attempt frozen study on five public defects, two related artifacts, three native policies and two total budgets. Exact archive SHA256: `d137f74c6de39f5bbeefb30b387215d74905b5b45c4151c186289cba1a4b3049`. Source, original-hash lineage and deidentified records are inside the archive. Matching weights and executable assets require separate provisioning.

完整60条冻结研究，五个公开缺陷、两个相关产物、三原生策略、两档总预算。压缩包精确SHA256：`d137f74c6de39f5bbeefb30b387215d74905b5b45c4151c186289cba1a4b3049`。源码、原哈希来源及去标识记录在压缩包内；匹配权重和可执行资产需另行提供。

Download download_bundle.py and use the full immutable commit cited by the manuscript or displayed for this research directory. 下载download_bundle.py，使用论文引用或此研究目录显示的完整不可变提交。

```text
python -B download_bundle.py --ref FULL_40_CHARACTER_COMMIT --output NEW_DIRECTORY --expected-sha256 d137f74c6de39f5bbeefb30b387215d74905b5b45c4151c186289cba1a4b3049
python -B NEW_DIRECTORY/bundle/verify_public_evidence.py --self-test
```

Offline verification does not rerun models. Fresh reference/model preparation is described in both language folders. Independent human review, second-hardware validation and journal submission remain incomplete.

离线核验不重跑模型。全新参考/模型准备见双语目录。独立人类审阅、第二硬件验证与期刊投稿尚未完成。
