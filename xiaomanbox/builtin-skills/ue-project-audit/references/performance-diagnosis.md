# 专家级性能诊断方法论

## 目录

1. 总原则：先分级，再下钻，改完重测
2. 第一步：分级——三个线程谁在拖后腿
3. 确认阶段：分辨率缩放验证法
4. 假阳性：两种会骗过分级判断的情况
5. 分叉 A：GPU-bound 时查什么
6. 分叉 B：Game Thread（CPU）-bound 时查什么
7. 分叉 C：Render Thread-bound 时查什么
8. 每条技巧对应哪个工具，置信度如何
9. 操作纪律：这条诊断链会碰的副作用

内容来自 Epic 官方文档、AMD GPUOpen 性能指南、Epic 官方博客（"How to improve game thread CPU
performance"）与多篇经过交叉验证的技术文章，参考链接见 `SKILL.md` 主文件底部。所有"我们能不能
做到"的判断都对照过本仓库现有工具的真实实现，不是照搬别人的工具箱假装我们也有。

---

## 1. 总原则：先分级，再下钻，改完重测

专业的性能排查不是"看着不对劲就调设置"。行业里反复强调的一条纪律：**测量先于优化**——不先分级
就动手调分辨率、关 Lumen、怪 Nanite，十次有九次是白费。正确顺序固定是三步：

1. **分级**：三个线程（游戏/渲染/GPU）里谁在拖后腿。
2. **下钻**：钻进那个线程内部，找到具体是哪个系统/哪个 Pass。
3. **改完重测**：优化是个循环——修完当前最大的瓶颈，必须重新走第 1 步，下一个瓶颈往往在
   完全不同的系统里，不能假设一次诊断就把所有问题挖干净了。

## 2. 第一步：分级——三个线程谁在拖后腿

对应工具：`ue_capture_perf_trace`。它逐帧记录 `FrameTime` / `GameThreadTime` /
`RenderThreadTime` / `GPUTime`，是"stat unit 连续采样版"——比人工盯着屏幕读数强的地方是能
拿到 p95/p99，而不是只看一眼当下这一帧。

**判断依据**：哪个线程的耗时最接近 `FrameTime`（总帧时间），谁就是瓶颈。三个方向：

- `GameThreadTime` 最接近 `FrameTime` → 游戏线程瓶颈（蓝图、Tick、AI、动画、物理逻辑）
- `RenderThreadTime` 最接近 `FrameTime` → 渲染线程瓶颈（生成渲染命令、材质处理、drawcall 提交）
- `GPUTime` 最接近 `FrameTime` → GPU 瓶颈（光照、几何、后处理的实际渲染工作）

**一条容易漏掉的细节**：游戏线程和渲染线程在每帧末尾要同步，谁都不能在两者都跑完前开始下一帧。
所以当 `GameThreadTime` 和 `RenderThreadTime` 数值很接近、且都明显低于 `GPUTime` 时才是真的
GPU-bound；但如果 `GameThreadTime` ≈ `RenderThreadTime` ≈ `FrameTime`、`GPUTime` 反而偏低，
大概率是**渲染线程在等游戏线程**——先怀疑游戏线程，压下去之后渲染线程的数字往往跟着掉，
不要两边同时下手。

工具已有的 `bottleneck` 字段（GPU / GameThread / RenderThread / balanced）是这条判断的自动化
版本，可以直接读，但上面这条"渲染线程在等游戏线程"的细节工具没有自动判——数值挨得很近时，
自己看 `stats` 对象里的原始数字复核一遍。

## 3. 确认阶段：分辨率缩放验证法

`bottleneck` 字段判断错的概率不高，但如果结论会影响接下来大段的优化建议，值得用一个独立信号
交叉验证一次。行业里公认的验证法：**改渲染分辨率，看 GPU 时间变不变**。

```
ue_run_console_command('r.ScreenPercentage 150')   # 提高渲染分辨率，加大 GPU 负载
ue_capture_perf_trace(duration_seconds: 10)          # 重新采样
# 对比 GPUTime：明显升高 → 真 GPU-bound；几乎不变 → 真 CPU-bound（游戏或渲染线程）
ue_run_console_command('r.ScreenPercentage 100')    # 改完一定要恢复，见 §9
```

这一步不是必须的，是**结论存疑或者要写进正式报告时**的加固手段，别每次分析都跑一遍——
两个 `ue_run_console_command` 调用都是 destructive 风险，需要用户批准，别为了一个已经很清楚
的结论多打扰用户一次。

## 4. 假阳性：两种会骗过分级判断的情况

- **GPU 数字纹丝不动，卡在一个固定值**：疑似撞上了垂直同步（vsync）上限，不是真的 GPU 瓶颈到顶。
- **所有线程的数字都稳定卡在 16.6ms 附近（对应 60 FPS）**：疑似撞上了帧率上限
  （`t.MaxFPS` 或类似的平滑设置），不是引擎真的刚好每次都卡在这个数字上。

两种情况都要先怀疑限制器本身，而不是直接去分析"为什么正好卡在这个数"。

## 5. 分叉 A：GPU-bound 时查什么

UE5 默认打开的几套系统本身就是常见的 GPU 大户，尤其是场景其实用不上它们默认强度的时候：

- **Nanite overdraw**：半透明/遮罩材质堆叠是 Nanite 最常见的 overdraw 来源，植被密集区尤其
  明显。检查方向：`ue_find_heavy_assets` 里高实例数、`nanite: false` 的资产——没开 Nanite
  的密集小物件走传统光栅化，是重灾区；已经开了 Nanite 但材质是遮罩/半透明的，也值得怀疑。
- **Virtual Shadow Map 的 coarse page 开销**：对非 Nanite 的动态几何体尤其明显，会退化成大范围
  低分辨率阴影，造成 draw call 瓶颈。同样对应"高实例数、没开 Nanite"的那批资产。
- **Lumen 开着默认强度，但场景不需要**：室内场景仍然套着为大型开放世界调的 Lumen/阴影设置，
  持续消耗 GPU 却收益很小。对应 `ue_content_audit_optimization` 的 Lumen 配置检查。
- **半透明材质本身开销大**：多次纹理采样、按顶点法线混合这类 shader 逻辑在半透明表面上代价
  很高，实测案例里仅仅把 Shading Model 从 Thin Translucent 换成 Lit、关掉不必要的雾效和光线
  追踪阴影，帧率就翻了一倍多。

## 6. 分叉 B：Game Thread（CPU）-bound 时查什么

- **Tick 太多太重**：`dumpticks` 控制台命令会把当前所有注册了 Tick 的对象列出来（含类型、
  是否启用），是查"谁在每帧都跑"最直接的手段。
  **置信度：中等，别假装能自动读到结果**——官方文档写得很明确，这条命令**不会**在控制台本身
  打印任何东西，内容只写进 Output Log。也就是说 `ue_run_console_command` 大概率拿不到文本
  （它捕获的是命令直接回写的输出，dumpticks 走的是持久日志这条完全不同的路）。跑完之后要去
  项目的 `Saved/Logs/<项目名>.log` 里找 `"Tick Functions (All)"` 这个标题开始的段落——
  先用 `ue_get_project_info` 拿到项目路径，再用 `read_local_file` 读那个日志文件。没试之前
  不要跟用户说"我已经拿到 Tick 列表了"。
- **蓝图 VM 开销**：大量蓝图里的重计算、频繁 Tick 事件、低效循环会成为 CPU 瓶颈——C++ 编译成
  机器码，蓝图走虚拟机，本身开销更高。这条我们没有直接读取手段，只能作为解读 Insights 计时器
  排行时的背景知识：如果 `ue_insights_trace`（analyze）的排行前几名里出现和蓝图执行相关的计时器
  （名字含 Blueprint/Kismet 之类字样），指向的就是这里。
- **物理/碰撞设置过度**：默认网格同时开着物理和碰撞，如果实际只需要查询（比如玩家移动检测），
  该设成 QueryOnly——同时开着两者，移动对象一多就白白吃 CPU。对应线索：`ue_find_heavy_assets`
  里 `missing_collision: false` 且 `instance_count` 很高的资产，值得追问是不是真的需要
  Simulate Physics。
- **GC 峰值**：手动 GC 调用位置不对、频繁生成销毁对象（该用对象池的没用）会造成周期性尖峰。
  对应线索：`ue_capture_perf_trace` 的 `worst_frame` 明显高于 p95 一大截、且是孤立的单次尖峰
  而不是持续偏高，值得怀疑是不是 GC 或者资源加载造成的一次性卡顿。

## 7. 分叉 C：Render Thread-bound 时查什么

最常见的原因是 draw call 太多——没合批的静态网格、没开 Nanite 的密集小物件，每一个都是一次
独立的渲染命令提交。对应线索：`ue_find_heavy_assets` 按 `InstanceCount` 排序，`nanite: false`
且实例数很高的资产是头号嫌疑——这正是"关卡资产开销排行"这个工具存在的意义，直接把嫌疑对象
列出来，不用再去猜。

## 8. 每条技巧对应哪个工具，置信度如何

| 诊断技巧 | 对应工具 | 置信度 |
|---|---|---|
| 三线程分级 | `ue_capture_perf_trace` | 高——直接读引擎自己的逐帧数据 |
| 分辨率缩放验证 | `ue_run_console_command('r.ScreenPercentage N')` + 重采样 | 高——数值对比，因果关系清楚 |
| GPU 具名 Pass 排行 | `ue_insights_trace`：先 `action="capture"`，再 `action="analyze"`，看 `threads.gpu` | 中——依赖 Insights CLI 导出，见 SKILL.md 主文件的已知局限 |
| 改前改后对比 | `ue_insights_trace`：改前、改后各 capture 一份同样的操作，再 `action="compare"` | 中——同上；两次录的操作不一样时比出来的差别没有意义 |
| Tick 对象清单 | `dumpticks` + `read_local_file` 读日志 | 低——命令能跑，但读取路径没有专门工具，纯手动拼 |
| 资产结构线索（Nanite/实例数/贴图） | `ue_find_heavy_assets` / `ue_project_asset_ranking` / `ue_content_audit_optimization` | 高——已经是这个 skill 的核心工具 |
| 屏幕叠加层读数（`stat gpu`/`stat game`） | `ue_screenshot(show_ui=true)`，但**没有实测过** | 低——原理上抓的就是用户屏幕那一份，叠加层应该在画面里；数字认不认得出来没人验过。读不清就直接说读不清，别猜 |

表格最后一行是特意写出来的：这正是"专业排查"最经典的一步（切到屏幕上看 `stat gpu`/
`stat game` 的实时读数）。默认的截图路径拍的是渲染出的画面、不含 Slate 叠加层，所以
那条路是真的做不到；`show_ui=true` 改成整窗抓屏之后**可能**能读到，但那是推理不是实测。
拿不准就承认拿不准，比让模型编一组读不到的数字要好。

## 9. 操作纪律：这条诊断链会碰的副作用

- `r.ScreenPercentage` 是全局渲染设置，验证完必须改回 `100`（或用户原来的值）。别在诊断流程
  走完之后留一个被悄悄改掉的分辨率给用户。
- 这条链路里的 `ue_run_console_command` 调用全部是 destructive 风险，auto-edit 模式下也会
  停下来问用户——先说清楚要跑什么命令、为什么，不要连续弹好几次审批却不解释。
- PIE 里测出来的数字包含编辑器自身开销，比 Standalone 或打包版本偏重。报告里如果要下"能不能
  达到 60 帧"这种绝对结论，要提醒用户这组数字只能做相对比较，不是最终性能保证。
