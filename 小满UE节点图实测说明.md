# 小满 UE 节点图实测（首次开源版）

2026-10-12，在RTX5080和UE5.7.4临时NullRHI工程，用公开主线文本量化模型实测以下流程。

| 任务 | 模型调用 | 独立核验 |
| --- | --- | --- |
| 创建Actor蓝图、自定义事件XiaoGraphHello连接PrintString | 创建蓝图→搜索精确函数与引脚→写节点图，共4轮 | 执行线连接到PrintString.execute，InString为XiaoManGraph_OK，编译UpToDate并保存 |
| 创建XiaoGraphFunction函数 | 创建函数，共2轮 | 在此函数图中继续下项测试 |
| 在函数图添加PrintString并连接已有入口 | 读取入口GUID→搜索函数→写指定函数图，共4轮 | 入口执行线、XiaoManFunction_OK字符串、编译保存通过，EventGraph原有事件逻辑仍保留 |

工作台自动回读的目标图名与模型写入图名一致，真实节点GUID、引脚category和linked_to用于核查，临时提交标签通过写入回执映射到GUID。7项模拟RPC/字段负控另验证已使用的纯函数数据输入缺省、显式常量、已有连线、未使用节点和执行节点的处理；模拟负控不等于完整模型质量评测。

本轮扩大到4096窗口、host state 1、host KV 256MiB、输出上限512、关闭思考与CUDA Graph。加载后本次总显存读数14775MiB（约14.43GiB）。默认部署仍为2048；4096只新增了此机器、此文本引擎与无渲染UE测试的记录，视觉与完整编辑器渲染占用须另测。

复验该文本配置可以使用 `node deploy.mjs model-config --id xiaoman-v6-sd --context 4096 --host-state-slots 1 --host-kv-mib 256`；运行前确认GPU空闲，并按阶段限定所需工具。不要把全部UE工具或大预设塞入短窗口。

本次验证编译和结构回读，没有运行PIE或触发事件来测最终游戏行为。真实回执见validation/real-model-ue-graph.json。此前2048的Actor与变量/编译回环也在当前后端重新通过，见validation/real-model-ue-loop.json。原工程未修改，测试子进程已停止。
