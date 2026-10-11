# 「渲出来是黑的 / 按播放没反应」的全部成因

社区里最高频的 Sequencer 问题。同一个症状至少有十种病因，**排查顺序比知识本身更重要** ——
用户卡住不是因为修不好，是因为不知道该先看哪里。

每条给三样：**现象、为什么、他自己怎么点**。

## 目录

- A. `sequence_describe` 能直接查出来的 —— A1 没有相机切轨 / A2 切轨没覆盖播放范围 /
  A3 切轨之间有空隙 / A4 绑定失效
- B. `describe` 查不到、必须逐条问用户的 —— B1 视口没锁到 Camera Cuts / B2 没打关键帧 /
  B3 子关卡加载方式不对 / B4 曝光没锁、Lumen 没收敛 / B5 景深糊成一片 / B6 PIE 里不播 /
  B7 每个 cut 首帧是脏的 / B8 spawnable 与 possessable 混用
- 排查顺序（照这个问，不要跳）

---

## A. `sequence_describe` 能直接查出来的

### A1. 没有相机切轨（Camera Cuts）

**现象**：整段全黑。
**为什么**：没有 Camera Cuts 轨，渲染器不知道该用哪台相机，输出的就是空画面。
**怎么点**：Sequencer 面板左上 `+ Track` → `Camera Cut Track`，
然后在轨道上 `+` 选中场景里的 CineCameraActor。

### A2. 切轨存在但没覆盖完整播放范围

**现象**：片子开头或结尾有一段黑。
**为什么**：没被切轨段覆盖的帧没有相机。
**怎么点**：拖动 Camera Cuts 段的两端，盖满整个 playback range。

### A3. 切轨段之间有空隙

**现象**：片子中间某几帧闪黑。**这条肉眼在时间线上基本看不出来。**
**为什么**：两段之间哪怕差一帧，那一帧就没有相机。
**怎么点**：把后一段的起点拖到与前一段末帧相接。注意 §frame-boundaries：
段是闭开区间，前一段 `end` 就是后一段应有的 `start`，**不要再 +1**。

### A4. 绑定失效

**现象**：轨道变红写着 "The object bound to this track is missing"；
或者不红但播放时那条轨完全没效果。
**为什么**：Possessable 绑定存的是**路径引用**。改名、移到别的子关卡、
删了重放、把关卡另存为新名字，绑定就断，而且**不报错**。
**怎么点**：Level Sequence 编辑器 → 扳手图标 `Actions` → `Advanced` →
`Rebind Possessable References`（有的版本叫 `Fix Actor References`）。
修不好就右键那条轨道 → `Assign Actor` → 选中场景里正确的 Actor。

> **两类故障要分开说。** 资产引用（子序列、动画、材质）改名后可以靠
> Redirector 修；**关卡里 Actor 的 possessable 绑定不走 Redirector**，
> 只能重绑。给建议前先分清是哪一类。

> **5.4+ 的注意**：`Rebind Possessable References` 本身有回归
> （[UE-256345](https://issues.unrealengine.com/issue/UE-256345)）——
> 跑完再保存重开，component track 的绑定反而可能丢。5.3 没这问题。
> 建议用户跑完先检查一遍再保存。

---

## B. `describe` 查不到、必须逐条问用户的

### B1. 视口没锁到 Camera Cuts（「按播放没反应」的头号原因）

**现象**：播放时画面不动，看到的还是编辑器相机。
**为什么**：Sequencer 的相机切换只在**锁定视口**时生效。
**怎么点**：Camera Cuts 轨道左侧那个小相机图标点亮。

### B2. 根本没打关键帧

**现象**：把相机挪到新位置、按播放，什么都不动。
**为什么**：Sequencer 记录的是**关键帧之间的变化**，没有关键帧就没有任何信息。
移动 Actor 不会自动产生关键帧。
**怎么点**：在时间线上定位到目标帧，改完属性后点属性旁边的关键帧按钮（或按 S）。

### B3. 子关卡加载方式不对（灯光/资产在渲染时不存在）

**现象**：视口正常，渲出来全黑或缺东西。
**为什么**：灯光只放在 Persistent 层、或者子关卡设成了 Blueprint 加载，
渲染时那些关卡没被加载。
**怎么点**：`Window` → `Levels`，把需要的子关卡改成 `Always Loaded`。

### B4. 曝光没锁 / Lumen 首帧没收敛

**现象**：不是纯黑，是极暗；或者开头几帧忽明忽暗。
**为什么**：Sequencer 的后处理与自由视口不同，自动曝光要时间适应；
TSR、Lumen、粒子在第一帧不会收敛。
**怎么点**：Post Process Volume 里 `Metering Mode` 设 `Manual`；
渲染时加 `Engine Warm Up Count` / 打开 `Render Warm Up Frames`。

### B5. Cine Camera 的景深把画面糊成一片

**现象**：不黑，但整个画面是糊的，看起来像「引擎渲染质量差」。
**为什么**：Cine Camera 默认开物理景深，`Manual Focus Distance` 常见默认值是 100cm，
而主体在 500cm 外。
**怎么点**：相机的 `Focus Settings` → 改 `Focus Method` 为 `Disable`，
或者用 `Tracking` 对准目标 Actor。
**注意**：Sequencer 里的关键帧会覆盖 Details 面板的值 —— 在面板上改半天没用，
要在时间线上改。

### B6. PIE 里不播（编辑器里正常）

**现象**：编辑器里好好的，一进 PIE 什么都不发生。
**为什么**：关卡里的 Level Sequence Actor 没开 `Auto Play`。
**怎么点**：选中场景里的 Level Sequence Actor，Details 里勾 `Auto Play`。

### B7. 每个 cut 的首帧是脏的

**现象**：每次切镜的第一帧有粒子没起来、布料在「沉降」、或位置不对。
**为什么**：粒子在 shot 开头才 activate，物理/布料需要预热帧。
**怎么点**：把 Activate 关键帧挪到 pre-roll 区（Camera Cut 起点之前），
或渲染时加 warm-up 帧。**每加一个新 shot 都要重设一遍。**

### B8. spawnable / possessable 混用

**现象**：切镜边界闪一帧错误画面，或渲到了错误的相机实例。
**为什么**：同一台相机在序列里同时以两种形态存在。
**怎么点**：统一成一种。
**⚠️ 警告必须说给用户听**：`Convert to Spawnable` 会**复制并删除关卡里的原 Actor** ——
名字看着像格式转换，实际是对关卡的破坏性改动，而且会影响同时在这个关卡里工作的其他人。

---

## 排查顺序（照这个问，不要跳）

1. 跑 `sequence_describe`，先看体检结果 → A1–A4
2. 全黑还是很暗？极暗 → B4；纯黑继续
3. 编辑器里播有没有画面？没有 → B1、B2
4. 编辑器里正常、只有渲染黑 → B3、B4
5. 只有 PIE 里不播 → B6
6. 只有切镜瞬间不对 → A3、B7、B8
7. 不黑但糊 → B5
