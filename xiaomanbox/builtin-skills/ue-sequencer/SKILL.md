---
name: ue-sequencer
description_zh: 在关卡序列里用自算关键帧编排任意运镜——环绕、推拉、跟随、8 字、手持晃动、摇臂——并诊断序列渲出来是黑的原因、读序列内容、对比两个序列（改了什么、保护版是否被动过、迁移角色是否保留动画状态）、修复缺失的 Camera Cuts 轨道。当用户说「绕着它转一圈」「给我个运镜」「镜头推近一点」「渲出来是黑的」「轨道变红了」「绑定丢了」时使用；执行渲染本身不适用于（渲染永远由用户决定）。
description: Author any camera move in a Level Sequence by writing keyframes the model computes itself — orbit, dolly, follow, figure-of-eight, handheld shake, crane — plus diagnose why a sequence renders black, read what is inside one, compare two sequences (what changed, whether a protected original was touched, whether a migrated character kept every animated state), and repair a missing Camera Cuts track. Use when the user says "绕着它转一圈"、"给我个运镜"、"镜头推近一点"、"甩起来那种"、"渲出来是黑的"、"按播放没反应"、"轨道变红了"、"绑定丢了"、"原版动没动"、"迁过去的齐不齐", asks what is inside a sequence, or asks whether a sequence is ready to render. Also read this skill's engine-disconnect rule whenever any Unreal tool reports no connected project. Do not use for running the render itself — rendering is always the user's own decision — nor for focal-length animation, retiming, or rebinding a possessable, none of which are implemented yet.
---

# Sequencer

**立场是解释，不是替用户改。** 那些坑用户每隔几个月会再踩一次；
替他改一次，三个月后他还是不会。所以每条问题都要给出三样：
**现象是什么、为什么会这样、他自己怎么点** —— 然后才是「要不要我帮你改」。

## 引擎断连时：立刻停下，不要找补

**这条排在最前面，因为它是本 skill 目前出过的最严重的事故。**

任何 UE 工具返回「没有连接的虚幻引擎项目」时：

1. **停下来，告诉用户「编辑器可能崩了或被关了」。** 这是最可能的原因
2. **如果上一步刚做过写入操作**（建序列、放 Actor），明确提醒：
   **关卡里新建的东西可能没保存，重开编辑器后要确认**
3. **不要**去 `list_engines` / `list_local_dir` / `find_local_files` 满硬盘找工程 ——
   连接断了不是「找不到工程」，翻文件系统解决不了，还会翻到别的项目上去误导用户
4. **不要**调 `ue_restart_editor` —— 它需要一个正在运行的编辑器才能重启，编辑器没了它也没用

一句「编辑器好像崩了，你重开一下，我等你」比十次工具调用有用。

## 五个工具：三个只读，两个写入

只读：

**`sequence_audit(sequence_paths, recursive, only_blockers)`** —— 出片前体检。
一次可查多条，输出 PASS / FAIL + 每条问题的「现象 / 为什么 / 怎么改」。
默认递归子序列。**问「能不能渲」「有没有问题」时用它。**

**`sequence_describe(sequence_path, detail, bindings)`** —— 读一条序列的结构。
**问「这里面有什么」时用它。**

- `detail="outline"`（默认）：绑定名/类型 + 轨道数。**先用这个**
- `detail="tracks"`：加上段和时间范围
- `detail="keys"`：加上关键帧，**必须同时用 `bindings` 点名**，否则会被拒绝

**`sequence_diff(base_path, compare_path, binding_map?, bindings?)`** —— 两条序列逐绑定对比。
**问「改了什么」「原版动没动」「迁过去的东西齐不齐」时用它。**

- 不给 `binding_map`：找差异。受保护资产拿备份和当前比，文件哈希对不上时靠它说清差在哪；
  自己改完拿改前副本和改后比，确认只动了该动的。报到关键帧的值、插值和切线
- 给 `binding_map`（源绑定 → 目标绑定）：覆盖检查。风格迁移、换显示层之后，
  逐条轨道判已继承 / 已适配 / 缺失 / 无法判断。**`sequence_audit` PASS 不代表迁移齐了** ——
  它查的是能不能渲，不是描边、覆层、材质槽参数这些状态还在不在
- 「无法判断」= 目标绑定不在序列里，状态可能由运行时逻辑接管。别说成缺失，也别说成已继承

写入：

**`sequence_camera_keys(sequence_path, camera_label, keys, …)`** —— 写相机关键帧。
**任意运镜都走这一个工具**：环绕、推轨、跟随、8 字、手持晃动、先升后俯、绕柱螺旋，
在它眼里都是「一串带位置和朝向的帧」。序列不存在会新建，相机不存在会新建，
相机切轨顺带建好。

**新建相机时会把整个当前关卡存盘** —— 不存的话相机不落盘，绑定扛不过一次编辑器重启。
引擎没有「只存这一个 Actor」的接口，所以用户在这个关卡里别的未保存改动会一起落盘。
工具会在返回里明说这件事（红线第 4 条），你转述给用户时别漏掉。

**写入是一笔事务，Ctrl+Z 撤得回编辑器里的状态。** 但序列资产紧接着就存盘了，
磁盘上那一版已经换掉 —— 撤销之后要再存一次才和磁盘一致。

**轨迹是你算的。** 这份 skill 不告诉你什么镜头好看、转几圈合适、俯角该是多少 ——
那是你和用户的判断，不是工具的。典型步骤：

1. `ue_get_actor(return_bounds: true)` 拿到目标的位置和体积
2. 自己算每一帧的 location / rotation
3. `sequence_camera_keys` 一次写进去

**别把一串关键帧数值列给用户、让他自己去 Sequencer 里打** —— 你有工具。

**`sequence_camera_cuts(sequence_path, camera_label?, rebuild?)`** —— 给已有序列补相机切轨。
`audit` 报「没有切轨」「切轨没盖满」时用它；`sequence_camera_keys` 的切轨那步失败时
也用它接上。已经盖满就什么都不改。

**它补全的做法是把切轨上已有的段全删掉、换成一整段。** 所以切轨上已经有内容却没盖满时，
它直接报错不动手 —— 那些段可能是别人排好的多机位剪辑，一帧对不齐就被抹成单机位，
而且撤不回来。确认要删才带 `rebuild: true`，删之前先拿 `audit` 的结果跟用户说清楚差在哪。

`sequence_camera_keys` 同理：切轨上已有段时它**不动**，关键帧照写，要重建得带
`rebuild_camera_cuts: true`。

### 算轨迹时，这几件是引擎的事，不是审美

- **yaw / roll 连续累加，不要归一化到 -180..180。** 359°→0° 会让画面在接缝处反甩一圈
- **匀速运动配 `cubic` 会过冲**（忽快忽慢），用 `linear`；要缓入缓出才用 `cubic`
- **等角度采样的圆天然匀速**，不需要弧长重参数化；沿样条走才需要
- **播放范围是闭开区间 `[0, end)`，末帧不渲染。** 要无缝循环就别在末帧打重复的键
- **同一帧只能给一个键。** 引擎打键不去重（`InsertKeyInternal` 只做插入），同帧两个键
  会两个都留下，切线按零时间差算，求值取哪个不定。要让镜头停一拍，用两个相邻帧的相同数值
- 新建的 CineCamera 会自动关掉景深（引擎默认对焦 100cm，主体在 5 米外就是糊的）

### 覆盖已有曲线：可以，但要说

写进一条已经有关键帧的轨道时，`sequence_camera_keys` 默认清空重写，返回里报清掉了
多少个。**这个数字必须转述给用户**，别让他下次打开 Sequencer 才发现自己 K 的东西没了。
不想覆盖就把 `replace_existing_keys` 关掉。

**改已有序列的时间**（重定时、批量改属性、变体批处理）还没有实现，
possessable 的重新绑定也没有 —— 见「做不到的时候」。

## 诊断路径

用户说「渲出来是黑的」「按播放没反应」「什么都没发生」时，**先跑 `sequence_audit`，
不要先猜**。然后：

1. 报告里已经指出问题 → 照 `references/black-screen-causes.md` 的 A 类展开
2. **PASS 但用户仍说有问题** → 走那份文件的 B 类：子关卡加载方式、曝光没锁、
   景深默认 100cm、视口没锁到 Camera Cuts、Auto Play 没开。**逐条问，按文件里的顺序**
3. 报告里出现「无法判定」的绑定 → **它们不一定坏了**，见下
4. 报告说「绑定有效性检查没有执行」→ 那项**根本没查**，
   不要把「没报绑定问题」说成「绑定都好着」

## 三条最容易搞错的事

**「无法判定」≠「已失效」。** World Partition 里没加载的 Actor、正在 PIE、
拿不到编辑器世界，都会让好绑定看起来解析不了。**不要让用户去修一个没坏的东西。**

**帧边界不要自己算。** Sequencer 起始帧含、结束帧不含；MRQ 渲染不含末帧；
AnimSequence 首尾都含；播放头压在两段交界上显示的是**后一段**。
细节和换算见 `references/frame-boundaries.md`。

**能力随引擎版本变。** 同一件事在 5.0 和 5.6 上不一样，`describe` 的返回里带
`capabilities`。**看到能力受限就要如实告诉用户**，不能静默降级。
版本差异表见 `references/engine-version-matrix.md`。

## 绑定断了：这一步只能用户自己点

序列里的 possessable 指向一个已经不存在的对象时（相机被删了，或者关卡没保存
就崩了），**重新绑定没有 Python 接口** —— 引擎侧的 `ReplacePossessable` 没导出。
`sequence_camera_cuts` 会把这种情况查出来并如实说明，不会假装修好。

告诉用户这两步，按顺序：

1. 打开序列 → 左上角扳手 → **Actions → Advanced → Rebind Possessable References**
2. 没生效就右键那条轨道 → **Assign Actor** → 选关卡里那台相机

如果这条序列的相机动画本来就是 `sequence_camera_keys` 写的，**重新写一条通常比手修更快**
—— 一次调用的事，而且它会先存关卡，不会再断一次。

## 做不到的时候

还没有的：**变焦动画**（焦距关键帧）、**重定时**、批量改属性、变体批处理、
可见性/材质等其它轨道、possessable 重新绑定。

不要假装做了。**没做的事不许说成做了**，这一条没有例外。

至于 `ue_run_python_script`：原来这里写的是「也不要用它绕过去改用户的序列」。
那句话太宽了。用户的需求整个就是一条需要新建轨道的序列时，照着它做的唯一结局是
回一句「做不了」—— 而这件事引擎的 MovieScene Python API 明明做得到
（2026-09-21 的用户反馈：268 个绑定的片子就是这么手写出来的，交付了）。
「工具没覆盖」不等于「用户该空手回去」。

所以按下面两类分：

- **用户已有的序列**：默认不用 Python 改。要动就先说清楚你打算动什么、动哪几条
  轨道，让用户点头再动。理由是改坏了他没法回滚 —— 不是这条路不能走。
- **你自己为这次任务新建的序列**：可以用 Python 建绑定、轨道、段、关键帧。
  但有两条硬要求：
  1. **写完必须回读核实**（`sequence_describe`，或自己 `get_bindings()` 数一遍）。
     引擎侧有函数返回「真值但无效」的代理对象（`find_binding_by_name` 对不存在的
     名字就是这样：`bool()` 为真、`is_valid=False`），拿它当存在性判据会让整批
     写入被静默跳过，而脚本自己打印的「成功」是假的。**脚本的自述不算数，回读才算。**
  2. 交付时如实说明哪部分是绕路手写的，以及它没经过工具那层的校验。

轨道类工具补齐之前，这就是当前的分界。补齐之后这一段会收回去。

做不到又没有绕路时的正确做法：说清楚这一件还没做，然后按
`references/black-screen-causes.md` 里的菜单路径告诉他自己怎么点。

**但先确认一遍是不是真的做不到。** 相机怎么走是 `sequence_camera_keys` 的事，
它不挑形状 —— 用户要环绕、推轨、跟随、手持晃，都是你算好帧然后写进去。
对这些说「没有实现」，或者把数值列给用户让他自己打，都是错的答案。

**永远不要提议渲染、也不要替用户渲染。** 渲染是不可逆的资源消耗（几小时机器时间、
几十 GB 磁盘），交付责任在人。我们的价值是**让他按下渲染键之前就知道会不会白等**。

其余红线（不许删、不许动手 K 的曲线、不许改命名、不许产生序列之外的副作用）
见 `references/red-lines.md`。**动手之前先读它。**

该不该由这个 skill 接手，边界例见 `references/trigger-examples.md`。
