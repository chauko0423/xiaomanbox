# 离线核验

Python3.11以上即可；下载解压后不需要GPU、模型、原私有盘、第三方Python包或网络。在解压目录运行：

```text
python -B verify_public_evidence.py --self-test
```

验核完整公开清单/哈希、原评分重算、60锁/nonce、逐行时钟、已发出样本截止/峰值、引擎/客户端请求及用量、未补参数的原生投影、有效模式、汇总分母、匹配对与精确引擎源码字节；拒绝篡改峰值和重复原生键。未知客户端用量仍null，预声明不完整轨迹仍失败。离线通过不是模型重跑或独立人类审阅。

source/operative_inputs内旧受保护清单标识私有来源字节；去标识源码要先重新绑定资产、路径、依赖才能执行。Windows新复现用prepare_replication.py。
