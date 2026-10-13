# Local engineering study: code and data / 本地工程研究：代码与数据

[English](English/01_Read_me.md) | [中文](中文/01_阅读说明.md)

Complete 96-row component cohort plus separately reported v7/v8/v9 records. Archive SHA256: `fca2354ae7acfc9dec86f1126d6c861078f5929edc3fc935ef358c9e52fddd7f`. The manifest lists all part digests. Code/data are inside the hash-bound archive; exact model/engine assets require separate provisioning.

完整96条组件批次以及单独报告的v7/v8/v9记录。压缩包SHA256：`fca2354ae7acfc9dec86f1126d6c861078f5929edc3fc935ef358c9e52fddd7f`。清单列出全部分片哈希。代码与数据在哈希绑定压缩包内；精确模型及引擎资产需另行提供。

Download `download_bundle.py` and use the full immutable commit shown by this branch or cited by the paper:

下载`download_bundle.py`，使用此分支显示或论文引用的完整固定提交：

```powershell
python -B download_bundle.py --ref FULL_40_CHARACTER_COMMIT --output C:/study/public_download --expected-sha256 fca2354ae7acfc9dec86f1126d6c861078f5929edc3fc935ef358c9e52fddd7f
python -B C:/study/public_download/bundle/verify_public_evidence.py --self-test
```

The second command audits records offline; it does not execute models or UE. See bilingual instructions for the separate reference/model replication. Independent human review and journal submission remain incomplete.

第二条命令离线审计记录，不运行模型或UE。单独的参考/模型复现见双语说明。独立人类审阅与期刊投稿仍未完成。
