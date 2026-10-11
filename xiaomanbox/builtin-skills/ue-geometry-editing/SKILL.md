---
name: ue-geometry-editing
description_zh: 用 Geometry Script 在 Unreal 内直接编辑网格几何：布尔并集/相减、平面切割、减面、重拓扑、补洞、焊接、UV 投影、生成碰撞，并把结果写回静态网格资产。当模型必须改形而不是改参数、当用户要改模型本身时使用，比如「两个模型合并成一个」「墙上开个洞」「面数太高帮我减一下」「破面补一下」「生成个碰撞」；查看网格信息、导入 FBX、摆放 Actor、以及雕刻/PolyEdit 这类交互式建模工具（无脚本接口）不适用于它。
description: Edits mesh geometry itself inside Unreal using Geometry Script - boolean union and subtract, plane cuts, simplification, remeshing, hole filling, welding, UV projection, collision generation, and writing the result back to a Static Mesh asset. Use when a model has to be changed rather than configured, or when the user says things like "把这两个模型合并成一个"、"在这面墙上开个洞"、"面数太高帮我减一下"、"这模型有破面补一下"、"给它生成个碰撞". Do not use for reading or auditing a mesh (that is mesh_describe), for importing FBX, for moving or placing actors in the level, or for the interactive Modeling Mode tools such as sculpting and PolyEdit - those have no scripting interface and cannot be driven at all.
---

# Geometry editing (Geometry Script)

一切通过 `ue_run_python_script` 调 Geometry Script。没有专用工具，也不需要。

## Quick start

```
1. 确认 GeometryScripting 插件已启用（见下）
2. mesh_describe 看清楚现状
3. ue_run_python_script 跑一段「读进来 → 改 → 写入副本 → 从副本重新读取」
4. 检查读写 outcome、操作目标和回读统计
5. ue_save，确认保存结果；要验证持久化时重开编辑器再回读
```

## 第一步永远是插件检查

GeometryScripting **默认不启用**。没启用时 `unreal.GeometryScript_*` 全部
`AttributeError`，看起来像 API 名字写错了。

```python
import unreal
output_data = {'geometry_scripting_available': hasattr(unreal, 'GeometryScript_MeshBooleans')}
```

返回 `False` 就用 `ue_manage_plugin` 启用 `GeometryScripting`，
然后**必须重启编辑器**才生效 —— 告诉用户去重启，别自己反复重试。

## 标准往返

**优化已有资产时保留它的设置：** 减面、修补和另存修改版，默认先
`EditorAssetLibrary.duplicate_asset`，再把几何写入副本。
不要用 `create_new_static_mesh_asset_from_mesh` 建空白资产代替复制原资产，
否则材质槽、碰撞和光照设置会丢失。新建 API 留给没有来源资产的程序化模型。
把原件与副本的材质槽、碰撞、LOD 和光照设置一起对照；几何变了以后碰撞是否仍适合
需要另外核实，但不能未经要求把原有碰撞清空或把材质换成默认网格材质。

**布尔的停止条件先于操作：** 只要实际读取的任一输入 `closed=False`，
当前这次实体布尔任务就应结束：**不再调用布尔、不写入、不保存、不继续试算或探查顶点**。
直接告诉用户「输入不封闭，布尔结果不可靠，因此没有生成结果」，并说明可以先确认
加厚/修补或改用切割的方案。用户点名 Plane 只是指定输入，不代表接受不可靠结果，
也不代表授权把平面变成立体。不要用「用户既然要求了就先试一下」覆盖这个停止条件。

先读 [references/api.md](references/api.md) 的签名、返回值和参数说明。
以下示例是**减法布尔并另存副本**。替换三个资产路径，并按目标位置设置刀具变换。
它只证明几何回读通过；材质、UV、碰撞和视觉效果仍需按任务检查。

```python
import unreal, math

if not hasattr(unreal, 'GeometryScript_MeshBooleans'):
    raise RuntimeError('GeometryScripting 未启用；启用插件后必须重启编辑器')

source_path = '/Game/Meshes/SM_Wall'
tool_path = '/Game/Meshes/SM_DoorHole'
destination_path = '/Game/Meshes/SM_Wall_Holed'
if not destination_path.startswith('/Game/') or unreal.EditorAssetLibrary.does_asset_exist(destination_path):
    raise RuntimeError('目标必须是 /Game 下尚不存在的资产；不覆盖原件')

def read_mesh(asset):
    if not isinstance(asset, unreal.StaticMesh):
        raise RuntimeError('输入不是有效的 StaticMesh 资产')
    mesh, outcome = unreal.GeometryScript_AssetUtils.copy_mesh_from_static_mesh(
        asset, unreal.DynamicMesh(), unreal.GeometryScriptCopyMeshFromAssetOptions(),
        unreal.GeometryScriptMeshReadLOD(lod_type=unreal.GeometryScriptLODType.SOURCE_MODEL, lod_index=0))
    if outcome != unreal.GeometryScriptOutcomePins.SUCCESS or mesh.get_triangle_count() == 0:
        raise RuntimeError('读取网格失败：' + str(outcome))
    return mesh

def stats(mesh):
    area, volume = unreal.GeometryScript_MeshQueries.get_mesh_volume_area(mesh)
    return {'triangles': mesh.get_triangle_count(), 'volume': volume,
            'closed': unreal.GeometryScript_MeshQueries.get_is_closed_mesh(mesh)}

source = unreal.load_asset(source_path)
mesh = read_mesh(source)
tool = read_mesh(unreal.load_asset(tool_path))
before = stats(mesh)
if not before['closed'] or not stats(tool)['closed']:
    raise RuntimeError('输入网格不封闭，布尔结果不可靠；未写入资产。先确认修补方案')

unreal.GeometryScript_MeshBooleans.apply_mesh_boolean(
    mesh, unreal.Transform(), tool, unreal.Transform(),
    unreal.GeometryScriptBooleanOperation.SUBTRACT,
    unreal.GeometryScriptMeshBooleanOptions())
after = stats(mesh)
print({'before': before, 'after': after})
if (not after['closed'] or after['triangles'] == 0 or
        not 0 < after['volume'] < before['volume'] or
        math.isclose(after['volume'], before['volume'], rel_tol=1e-6)):
    raise RuntimeError('未确认有效的减法布尔结果；未写入资产')

# 加工和检查通过后才创建副本，保留原件及其材质槽配置。
destination = unreal.EditorAssetLibrary.duplicate_asset(source_path, destination_path)
if destination is None:
    raise RuntimeError('创建副本失败')
_, outcome = unreal.GeometryScript_AssetUtils.copy_mesh_to_static_mesh(
    mesh, destination, unreal.GeometryScriptCopyMeshToAssetOptions(),
    unreal.GeometryScriptMeshWriteLOD(lod_index=0))
if outcome != unreal.GeometryScriptOutcomePins.SUCCESS:
    raise RuntimeError('写入失败：' + str(outcome))

# 新建 DynamicMesh 从目标资产读；不能拿刚加工的 mesh 冒充回读。
readback = stats(read_mesh(destination))
if (not readback['closed'] or readback['triangles'] != after['triangles'] or
        not math.isclose(readback['volume'], after['volume'], rel_tol=1e-5)):
    raise RuntimeError('目标资产回读不一致，不能报告完成')
output_data = {'before': before, 'after': after, 'readback': readback,
               'destination': destination_path, 'geometry_verified': True,
               'save_required': True}
```

写回只改内存里的资产，**最后必须 `ue_save`**。
`ue_save` 的成功是保存回执；随后在同一编辑器里 `load_asset` 或 `mesh_describe`
通常仍读内存缓存，不能称为「从磁盘重新读取」。需要持久化验收时，另启编辑器进程后回读。

## 常用函数（类名就是 Python 里的名字）

| 要干的事 | 调用 |
|---|---|
| 合并 / 挖洞 / 求交 | `GeometryScript_MeshBooleans.apply_mesh_boolean` (UNION / SUBTRACT / INTERSECTION) |
| 平面切开 | `GeometryScript_MeshBooleans.apply_mesh_plane_cut` |
| 镜像 | `GeometryScript_MeshBooleans.apply_mesh_mirror` |
| 减面到指定面数 | `GeometryScript_MeshSimplification.apply_simplify_to_triangle_count` |
| 减面到误差容限 | `GeometryScript_MeshSimplification.apply_simplify_to_tolerance` |
| 均匀重网格 | `GeometryScript_Remeshing.apply_uniform_remesh` |
| 体素封边 / 加壳 | `GeometryScript_MeshVoxelProcessing.apply_mesh_solidify` / `apply_mesh_morphology` |
| 补洞 | `GeometryScript_MeshRepair.fill_all_mesh_holes` |
| 焊接重合顶点 | `GeometryScript_MeshRepair.weld_mesh_edges` |
| 修退化面 | `GeometryScript_MeshRepair.repair_mesh_degenerate_geometry` |
| 重算法线 | `GeometryScript_Normals.recompute_normals` |
| 自动展 UV | `GeometryScript_UVs.auto_generate_x_atlas_mesh_uvs` |
| 生成碰撞 | `GeometryScript_Collision.set_static_mesh_collision_from_mesh` |
| 读统计 | `GeometryScript_MeshQueries.get_mesh_info_string` / `get_is_closed_mesh` |
| 存成新资产 | `GeometryScript_NewAssetUtils.create_new_static_mesh_asset_from_mesh` |

不确定参数时用 `help(unreal.GeometryScript_MeshBooleans.apply_mesh_boolean)`
打出真实签名，别猜。

## 五个会咬人的地方

**面数不在 MeshQueries 上。** `GeometryScript_MeshQueries.get_triangle_count`
不存在（引擎里那行是注释掉的）。面数在 mesh 对象自己身上：`mesh.get_triangle_count()`。

**布尔之前先确认封闭。** `get_is_closed_mesh` 返回 `False` 时结果不可靠，
而引擎可能**不报错**。停止写入并说明原因，不要自行把 Plane 补成另一种模型。
封闭也不保证没有自交，仍要检查操作结果。只有用户要求修补时才补洞并重新检查。

**不要用 `copy_mesh_from_static_mesh_v2`。** 它只有 UE 5.5+ 有。
不带 `_v2` 的那个九个版本都在，5.5+ 里就是转发到 V2 的包装，功能一样。

**减面之后检查 UV。** 是否保留接缝取决于简化方法和参数。不要自动重展已有 UV，
那会改变贴图对应关系；先检查失真，再按用户需求处理 UV、LOD 和光照。

**先保留原件。** `copy_mesh_to_static_mesh` 会改目标资产；不要依赖撤销作为备份。
默认另存副本，只有用户明确要求时才覆盖。新建网格的材质槽和碰撞也需要检查。

## Failure handling

- **`AttributeError: module 'unreal' has no attribute 'GeometryScript_...'`** —— 插件没启用或没重启。
- **脚本跑很久没回来** —— 正常。执行是同步的，编辑器在这期间就是卡住的，上限 5 分钟。
  这是调用方等待上限，超时或停止等待不等于 UE 停止执行。先回读，别中途重发。
- **改完面数没变** —— 单凭面数不能判断：变换、法线和 UV 编辑本来就可能不改面数。
  减面看目标面数，减法布尔看封闭性与体积减少，UV/法线看对应数据与效果。
  证据不足就说「未确认」，不能凭脚本没抛异常报告完成。

## Escalation

雕刻、手动 PolyEdit、CubeGrid 这些建模模式里的交互式工具**够不着**：
引擎没给它们任何脚本接口，只吃鼠标输入。碰到这类需求直接告诉用户要手工做，
别用 Python 硬凑一个近似的。
