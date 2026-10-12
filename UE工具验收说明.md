# UE 工具接口与实际验收

## 已验证的范围

basic.4 修正蓝图变量元数据、变量删除、默认对象/组件属性、注释框、节点断线及新建关卡的参数适配，并补齐节点坐标工具的参数说明。尚未实现的 `ue_get_crash_logs` 不再放入工具清单；需要日志时可用本地文件工具读取自己的日志。

在 UE 5.7.4 的隔离临时工程中，使用真实编译插件、公开版后端和本机 WebSocket 完成了：

- 新建空白关卡，并确认指定路径下实际生成 `.umap`。
- 创建 Actor 蓝图、添加浮点变量及默认值、启用实例可编辑，独立读取结构确认属性。
- 添加 StaticMesh 组件，设置组件 Mobility，设置蓝图默认值，创建带输入/输出的函数。
- 查询节点、创建 CustomEvent 与 PrintString、连线、断开后独立读回，按给定坐标移动节点并读回。
- 创建包含指定节点的注释框，编译并保存蓝图，确认 `.uasset` 实际存在。
- 在测试关卡生成该蓝图实例，独立读取默认值 23.5，修改实例值为 45.5，再保存关卡。

记录见 `validation/ue-native-workflow.json`。测试使用无渲染模式，没有修改用户原工程；不覆盖完整编辑器 UI、全部 UE 工具、其他引擎版本或模型自主完成这条流程的效果。

## 源码检查

安装基础版依赖后，在包根目录执行：

```powershell
node tests/ue-method-registry.mjs
node tests/ue-tool-contract.mjs
```

第一项确认直接 RPC 工具能对应到导出插件的命令注册文本；第二项检查工具请求格式与缺少修改目标、缺少连线端点等负控。它们不能替代编译插件和真实编辑器测试。

## 参数约定

| 工具 | 关键参数 |
| --- | --- |
| `blueprint_set_variable_meta` | `blueprint_path`、`variable_name`；按需 `instance_editable`、`category` |
| `blueprint_set_property` | `blueprint_path`、`property_name`、`value`；`target` 为组件名，省略则修改蓝图默认对象 |
| `blueprint_comment` | `blueprint_path`、`node_ids`、`text` |
| `blueprint_disconnect_pins` | `from` 和 `to` 同时提供，格式为 `节点GUID.引脚名` |
| `blueprint_tidy_graph` | `blueprint_path`、`positions: [{node_id, x, y}]`；先读取图取得真实 GUID |
| `ue_new_level` | `level_path` 指定保存路径，如 `/Game/Maps/MyLevel`；新建的是空白关卡 |

写图返回的真实节点 GUID 位于回执 `nodes[].node_id`。调用中的临时节点标签只在那次写图有效，后续断线、注释和移动应使用真实 GUID。数值、布尔值使用对应的 JSON 类型；字符串资产路径保持字符串。

关卡新建和切换沿用破坏性工具权限控制；已有未保存修改时由插件拒绝切换。自行复验时使用独立空白工程，先保存自己的工作，不把模拟 RPC 检查当成操作成功。

Basic.6 另行通过真实小满模型的 Actor 和基础蓝图回环（每项3个工具、4轮请求），见 [小满UE模型实测说明](小满UE模型实测说明.md)。这项模型测试覆盖的范围小于上述21项原生工作流。
