# Windows全新复现

使用Windows、Python3.12以上、pnpm11.25.0、官方Node0.10.28/6.17.1/8.17.0及新输出目录。官方URL和精确Node哈希在runtime_recipes。依赖禁用安装脚本并使用冻结锁。仅参考模式执行五个原开发者完整套件与评分负控，服务器/生成/静止边界为模拟，模型请求为0：

```text
python -B prepare_replication.py --workspace NEW_DIRECTORY --references-only --node01028 NODE01028_EXE --node6 NODE6_EXE --node8 NODE8_EXE --pnpm PNPM_COMMAND
```

新目录不能已存在。可选--reuse-dependencies-from指向包含各case目录的上级，要求package.json/pnpm-lock.yaml相符且node_modules存在。参考锁独立，不算模型成功。

模型执行去掉--references-only，另加--engine ENGINE_EXE --model36 MODEL36_NINFER --model38 MODEL38_NINFER --run-model。准备器验证三个资产精确哈希，建立新衍生冻结和分析，执行单独标识的60条复现。需读者自备匹配检查点；不含模型权重或引擎/Node可执行文件，不允许默换不同模型，不将本地名称当上游修订证明。复制的驱动仍要求起始GPU≤700MiB、主机≤50000MiB、临时空闲≥2GiB；未满足时可在启动某条前停止。不能重置或重跑已经开始的行来提高结果。
