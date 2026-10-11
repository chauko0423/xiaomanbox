---
name: ue-animation-retargeting
description_zh: 在不同骨架之间重定向身体动画、测量姿势并预览结果。当用户说「这批动画换个骨架」「把动画重定向到角色」「接进 GASP/运动匹配」「重定向后手臂没下来/还是 T-pose」「手臂穿模」「量一下低头角度」，或第三方角色要用别的骨架的动画走路跑步时使用；FBX 导入、序列相机轨道、Control Rig 制作、网格几何编辑不适用于它。
description: Retargets body animation between skeletons, measures poses and previews results in Unreal. Use when 用户说「这批动画换个骨架」「把动画重定向到角色」「把这个角色包接进 GASP / 运动匹配」「重定向后手臂没下来 / 还是 T-pose」「手臂穿模」「量一下低头角度」「把这段改成固定姿势」, or when a third-party character needs to walk, run or jump with animations made for another skeleton. Do not use for FBX import, Level Sequence camera tracks, Control Rig authoring, or mesh geometry editing.
---

# Body animation: measure, retarget, preview

## Quick start

1. `anim_measure(path, describe=true)` 查动画帧率、帧数、骨轨道。
2. 已有调好的 IK Retargeter 就把路径传给 `anim_retarget.retargeter`；不要重新建 Rig 或自动对齐它。
3. 没有配置时，由 `anim_retarget` 自动建源/目标 Rig、严格同名映射和姿势对齐。
4. 批量传 `animations: [{ animation, output_path }]`，插件每次只导出一段，保存并回读骨架、关键帧和文件。
5. 对本次产物调用 `anim_measure`，再用 `anim_preview` 看同一时刻，回复给一张图加本轮数字。

首版在 UE 5.5 编译和验收。旧插件不认识 `anim.*` 时先更新插件；低于 5.5 返回不支持，不能反复调用。
5.2–5.4 尚未被新工具替代的脚本接口保留在 [references/api.md](references/api.md)。
测量口径、写姿势边界和会话教训见 [references/measurement.md](references/measurement.md)。

## 链映射与姿势判断

一条链都没映射上必须停，不导出废动画。返回的映射表和未映射链要检查：未映射的肢体不算完成。
自动匹配采用 EXACT，不偷偷退到模糊匹配；FUZZY 可能把名字相近、含义不同的骨链配到一起。

源 A-pose、目标 T-pose 不一致时，生成成功和帧数正确不能证明姿势正确。
新建配置会自动对齐，但仍须看预览；肩塌、手穿身先查姿势和链，别靠调参数掩盖问题。
现有配置保留用户的 Rig 和姿势。需要检查或修正时用 [接口参考](references/api.md) 的控制器读写，
修好后再传同一 retargeter 路径导出，不能用一次自动创建覆盖它。

链只负责骨骼对应；脚不滑、手贴住道具还需要 IK goal/solver。自动生成不代表这些约束已经满足。

## 写姿势

`anim_write_pose` 只在同一骨架的非叠加序列间复制姿势。主区间是闭区间，过渡帧在前后。
工具核对全部局部变换；七个摘要数字一致不能证明手腕、手指等全部写对。
缺轨道按参考姿势补齐，写入和过渡范围外保持原关键帧。

## 判断姿势对没对：量，不看截图，不进 PIE

重定向姿势对不对，唯一可靠的通路是**导一段、量一段**：

1. `anim_retarget` 传现有 retargeter 和一段短动画（idle 最好），导到临时路径；
2. `anim_preview` 看导出的那段 0 秒那一帧，front 和 side 各一张；
3. `anim_measure` 用 `angle_bones` 量上臂对脊柱的夹角（四个骨名：脊柱起止、上臂起止），
   源动画和导出动画各量一次，把差值和预览图一起报给用户；对齐没有固定的度数门槛，
   同一动作两边差得越小越好，差多少能接受由用户看图定。`bones` 能回骨骼世界坐标序列。

不要用 `ue_screenshot` 拍重定向编辑器的预览窗来判断 —— 那张图是编辑器界面的一块像素，
手臂「下来没下来」一眼看不准，2026-09-22 就有两次把 T-pose 读成已对齐、错报给用户。
也不要为了读骨骼数值起 PIE：`ue_playtest` 回的是 PrintString 和画面，不回骨骼变换；
在 Python 里 `editor_play_simulate()` 之后 `sleep` 只是把游戏线程挡住，世界永远是 None。

## 自动对齐与手工偏移

- `auto_align_all_bones` **只调一次**，调完立刻用上面的三步验。调用后连接断了就不要再调，
  先 `ue_session_health` 看编辑器进程还在不在，还在就等它回来、再回读；报给用户时说清是哪一步断的。
- `anim_retarget` 不传 `retargeter` 时自动建配置，**内部同样会调一次 auto_align**。
  骨架已经在自动对齐上断过连接的，后面一律显式传现有 retargeter，别让它再建一次。
- `set_rotation_offset_for_retarget_pose_bone` 写的是局部空间的后乘增量，回读的四元数看不出方向，
  空间与合成顺序见 [references/api.md](references/api.md)。手工推偏移前先用 `anim_measure`
  量出目标和源的实际夹角，再决定要不要碰它。
- 5.6 起链设置和根设置在 op 栈上，控制器方法名先 `dir()` 再调，不要猜；
  op 栈 API 与常见 `AttributeError` 对照表见 [references/api.md](references/api.md)。

## Failure handling

- 输出已存在：先回读，再复用配置或另选新输出路径；工具不覆盖原件。
- 自动建链失败：检查骨架是否支持自动识别，已有手工配置就显式传入。
- 保存失败：可能已经改了内存，不要按“什么都没发生”重试。
- 超时或停止：停止只是当前这段完成后不发下一段；先回读已产生的资产。
- `unmeasurable`：报告缺骨或固定朝向限制，不能当作 0 或通过。
- 动画能播但姿势歪：先查重定向姿势，再查链映射，最后才查参数。
- `Failed to convert parameter`：该传枚举传了字符串（`set_ik_rig` 第一个参数要
  `unreal.RetargetSourceOrTarget.SOURCE`），或该传 int 下标传了名字（`get_retarget_op_enabled`）。
- 用户放弃这个角色、要把整包移出工程：`ue_content_delete` 直接传目录路径（结尾带 `/`），
  先带 `dry_run: true` 拿清单给用户看、他点头再真删，一批提交；不要在 Python 里循环 `EditorAssetLibrary.delete_asset`，那会按资产数做 GC、把编辑器占死。
