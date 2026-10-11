# 开发说明

面向要改这个插件的人。用户侧的安装和使用见 [README.md](README.md)。

## 布局

```
Source/UnrealAgentLink/
  Private/Commands/    命令处理，一个文件对应一组 RPC（editor.*、blueprint.* …）
  Private/Core/        WebSocket 连接、请求分发
  Private/Network/     网络管理
  Private/Utils/       JSON、跨版本兼容
  Public/              对应的头文件
Resources/Docs/        各组命令的接口文档
Content/               插件自带资产
Config/                默认配置
```

`Binaries/` 和 `Intermediate/` 由编译生成，不进版本控制。

## 编译

需要对应版本的虚幻引擎。挂在一个宿主工程下编译最省事：

```bash
"<引擎目录>/Engine/Build/BatchFiles/Build.bat" <工程名>Editor Win64 Development \
  -Project="<工程路径>/<工程名>.uproject" -WaitMutex
```

宿主工程可以是任意一个对应引擎版本的空工程 —— 把本目录用目录联接挂进它的
`Plugins/` 下，编译产物会落回本目录的 `Binaries/`：

```powershell
New-Item -ItemType Junction -Path "<工程>/Plugins/UnrealAgentLink" `
  -Target "<本仓库检出路径>"
```

两个容易卡住的地方：

- **目标名是 `<工程名>Editor`**，不是插件名。
- **备份/副本目录不要留在 `Plugins/` 下。** UBT 会把它和真目录当成两个插件，
  报 `already contains a definition for 'UnrealAgentLink'`。

**编译前必须关掉编辑器。** Live Coding 开着会直接报
`Unable to build while Live Coding is active`。而且 Live Coding 可能锁在一个
你没装的 MSVC 版本上（遇到过锁 14.38 而机器上只有 14.44），那种情况下热重载
根本走不通，只能关编辑器全量重编 —— 一次约 20 秒，不值得纠结。

## 踩过的坑

### 截图走 SceneCapture，不要用 HighResShot

`HighResShot` 的本质是**排队等视口下一次重绘**。编辑器窗口不在前面时虚幻跳过
绘制，那一帧永远不来 —— 请求不会失败，而是一直挂着直到超时。实测中它会在
用户自己点回编辑器之后才完成，那已经是 90 秒之后了。

从外部想办法让窗口回到前台是**走不通的**，以下都试过并排除：

| 做法 | 结果 |
|---|---|
| `SetForegroundWindow` | 受 Windows 前台限制，同一段代码两次调用一次成功一次失败 |
| `AttachThreadInput` + `SetForegroundWindow` | `GetForegroundWindow()` 会报出目标句柄，但真实焦点没转移，截图照样超时 |
| `ShowWindow(SW_RESTORE)` | 只对**最小化**的窗口有效；被别的窗口盖住时对前台毫无影响 |
| 关节流 `Slate.bAllowThrottling 0`、解限帧 `t.MaxFPS 0` | 与结果不相关 |
| 靠 `render_thread_ms == 0` 探测「没在渲染」再提前报错 | 判据是错的，这个引擎全局量在某些机器上任何时候都是 0，会 100% 误报 |

所以默认走 SceneCapture：用 `USceneCaptureComponent2D` 在游戏线程上**主动**
渲染到 RenderTarget，跟窗口可见性、焦点、节流都无关，也不需要轮询目录找新文件。

只有 `show_ui=true`（要连编辑器界面一起拍）才退回 HighResShot —— 那种需求
本来就要求窗口可见。

改这块时有三个细节别踩回去：

- **alpha 必须强制成 255。** SceneCapture 出来的 alpha 常常是 0，直接编码会得到
  一张全透明的 PNG —— 看着「截图成功」，打开是空白。
- **用组件，不要 `SpawnActor`。** `ASceneCapture2D` spawn 出来会在大纲视图里
  闪一下，还要负责销毁。`NewObject<USceneCaptureComponent2D>` +
  `RegisterComponentWithWorld()` 不进大纲，用完 `DestroyComponent()` 即可。
- **曝光和视口不完全一致**，实测偏暗一档：编辑器视口默认用固定曝光，而
  SceneCapture 走引擎默认的自动曝光，两边压根不是同一套。视口的 `bFixed` 为真时
  照抄它的 `FixedEV100`；否则只能把 `AutoExposureSpeedUp/Down` 调到 100 逼它一帧
  内收敛。调用方不该拿这张图去判断画面亮度。

### 预热帧必须跨帧渲，同一帧里连调 `CaptureScene()` 没有任何作用

这条踩过两次，别再踩第三次。曾经的写法是在一个函数里连调两次 `CaptureScene()`，
注释写着「第一帧建眼适应历史，第二帧才稳定」—— 实测亮度纹丝不动（143 vs 180）。
引擎源码解释了原因：

- `SceneView.cpp` 里 `FSceneViewFamily::ConstructionValues` 的时间取自
  `World->GetTime()`，是**引擎上一次 tick 的时间和 DeltaTime**，不是墙上时钟。
  同一个游戏线程函数里的两次捕获拿到同一份时间。
- `SceneCaptureComponent.cpp::SetFrameUpdated()` 用 `GFrameCounter` 判定，
  同帧内的第二次捕获被明确识别为 `bRepeatOfThisCapture`。

眼适应、Lumen 表面缓存、VSM、TSR 的历史都是**每帧走一步**，不换帧就原地不动。
**同理，在盒子那侧 sleep 一两秒也没用** —— 等待期间这台相机一帧都没渲。

现在的做法（`UAL_TickAsyncCapture`）：挂 `FTSTicker`，每个 tick 只渲一帧。
组件和 RenderTarget 要 `AddToRoot()`，否则中途会被 GC 掉。

PIE 收尾那条路仍然用同步的 `CaptureSceneToFile()` —— 它下一句就是
`RequestEndPlayMap()`，游戏世界马上销毁，等不到下一帧。

### 截图前等的是确定的信号，不是拍脑袋的秒数

三样，都在 `UAL_TickAsyncCapture` 里：

| 等什么 | 怎么等 | 不等会怎样 |
|---|---|---|
| 资产编译 | `FAssetCompilingManager::Get().GetNumRemainingAssets()` 归零 | 网格体/纹理还是占位的 |
| 着色器编译 | `GShaderCompilingManager->GetNumRemainingJobs()` 归零 | 材质是灰的/默认棋盘 |
| 贴图流送 | `IStreamingManager::Get().StreamAllResources(2.0f)` | 贴图糊 |

两个坑：

- **`FAssetCompilingManager` 不管着色器**（查过 5.5 的 `AssetCompilingManager.cpp`，
  里面一次都没提 `GShaderCompilingManager`）。两个都要查，少一个等于没等。
- **流送器不认识 SceneCapture 的机位。** `AddViewInformation` 只有
  `GameViewportClient` 会调，`SceneCaptureRendering.cpp` 里一次都没有。编辑器窗口
  被遮挡时视口根本不出帧，流送器不知道这个机位需要哪些贴图，直接
  `StreamAllResources` 是空转。必须**先渲一帧、再自己把机位
  `AddViewInformation` 进去**，然后才冲。

等到上限仍然没干净的残留，原样回到响应的 `pending_shaders` / `pending_assets` /
`streaming_in_flight`。**不许咽掉**：模型光看图分不出「材质坏了」和「材质还没编完」，
会去修一个不存在的问题。

### 命令的返回要真的送回去

有过一批命令「干了活但结果没送到调用方」：Python 的 `print()` 被丢掉、控制台
输出没捕获、截图渲染出来了但路径没回传。这类问题不会报错，调用方拿到
`ok: true` 就以为成功了。

加命令时确认 `SendResponse` 里带上了**实际产出**，而不是只回一个状态位。
