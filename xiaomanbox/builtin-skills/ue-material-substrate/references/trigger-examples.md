# Trigger examples

## Should trigger

- 用 Substrate 做一个车漆，要清漆层
- 做个彩色玻璃杯，绿色的
- 给这块石头叠一层灰尘，灰尘多少用遮罩控制
- 丝绒沙发的材质
- 肥皂泡那种彩虹色的材质
- Front Material 该怎么接？
- 帮我把项目的 Substrate 打开
- 这个 Substrate 材质编译报 Missing Top input
- 皮肤材质，要次表面

## Should not trigger

- 做个红色塑料材质，项目没开 Substrate → `ue-material-authoring`
- 给这个材质做三个颜色的实例 → `ue-material-authoring`
- 把地形材质改成按坡度混合草和岩石 → `ue-material-authoring`
- 导入这批贴图 → `ue-content-import-organize`
- 蓝图里加个开关控制材质参数 → `ue-blueprint-graph-editing`

## Ambiguous

- 「做个玻璃」 in a project with Substrate off: classic Translucent glass works without
  switching. Say that colored transmission needs Substrate, and ask whether it is worth a
  restart and a full shader recompile. Do not switch on your own.
- 「车漆」 without saying how good it must be: default to `SubstrateSimpleClearCoatBSDF`
  (one closure). Offer the two-Slab layered version only if the user asks for a
  showroom-quality look.
- 「把这个老材质转成 Substrate」: the editor's one-way converter is not reachable. Read the
  old graph with `material_get_graph`, rebuild the same inputs on a Slab, and wire it to
  `FrontMaterial`. Tell the user it is a rebuild, not an in-place conversion.

## Failure handling example

`material_search_nodes` for "Substrate" comes back with a first line saying Substrate is
not enabled. Reply to the user: this project renders with the classic pipeline, and
colored glass needs Substrate. Switching means an editor restart and recompiling every
shader in the project, and existing materials go through automatic conversion. Ask
whether to switch. Only after a yes, run `ue_set_config` and `ue_restart_editor`, then
search again to confirm before building anything.

`material_apply_graph` stops at "the Material output's Metallic pin is ignored". Nodes
before it exist already. Do not resend the whole graph: add a
`SubstrateMetalnessToDiffuseAlbedoF0` node, wire the metallic value there and its outputs
into the Slab, using the node ids from the report.
