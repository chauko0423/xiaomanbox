# 什么算这个 skill 的活

盒子里和「分析 / 优化」沾边的说法很多，但其中一半是要**动手改**，不是要报告。
这一层分不清，用户说「优化一下这些贴图」会得到一份报告，然后什么都没发生。

## 应该触发

| 用户说 | 第一个工具 |
|---|---|
| 分析一下这个项目 | `ue_project_asset_ranking` |
| 帮我看看有什么可以优化的 | `ue_project_asset_ranking` |
| 哪些资源最占地方 / 什么最占空间 | `ue_project_asset_ranking` |
| 打包出来怎么这么大 | `ue_project_asset_ranking` → `ue_asset_size_map` |
| 这个关卡为什么这么卡 | `ue_find_heavy_assets` |
| 场景里什么最费 / 面数最高的是哪个 | `ue_find_heavy_assets` |
| 帮我看看场景有什么问题 | `ue_find_heavy_assets`（先报 breaks_gameplay） |
| 这张关卡为什么这么大 | `ue_asset_size_map` |
| 这个蓝图拖了多少东西进来 | `ue_asset_size_map` |
| Nanite 我用上了吗 | `ue_content_audit_optimization` |
| 现在多少帧 | `ue_get_performance_stats` |
| 感觉会掉帧 / 有没有卡顿 | `ue_capture_perf_trace` |
| 压力测试一下性能 | `ue_capture_perf_trace` |
| 具体是哪个函数/哪个渲染 Pass 花的时间 | `ue_insights_trace`：先 `action="capture"`，再 `action="analyze"` |
| 导致我帧率下降的真正原因是什么 / 到底是哪里卡 | 已经采过样就直接查 `references/performance-diagnosis.md`，别只报 bottleneck 字段 |
| 是 CPU 瓶颈还是 GPU 瓶颈 | 同上，走分级+确认那两步，不要凭 `bottleneck` 字段的一个词下结论 |
| 你确定是这个原因吗 / 能证明吗 / 删了它真的会变快吗 | `references/experimental-verification.md`——先问能不能在副本上做对照实验，别继续用同一套推断反复解释 |

## 不应该触发

| 用户说 | 该去哪 |
|---|---|
| 把没用的资产删了 | 删资产是 `ue_content_delete`，这个 skill 只出报告 |
| 把贴图分辨率降到 2K | 改资产属性，不是审计 |
| 把场景里的灯归到一个文件夹 | `ue-level-organize` |
| 这个材质被谁用了 | `ue_content_describe`，一层引用就够，不用整棵依赖树 |
| 这个序列的绑定是不是断了 | `ue-sequencer` |
| 蓝图编译报错 | `ue-blueprint-graph-editing` |
| 编辑器崩了 | `ue-system-diagnostics` |

## 边界模糊的两个

**「优化一下这个关卡」** —— 先当审计做，报告完了问一句要不要动手。
直接开改是越界：用户说的「优化」可能是删东西，而删掉的东西找不回来。

**「帮我看看这个模型有没有问题」** —— 单个资产用 `ue_content_describe`
（面数、LOD、材质槽、Nanite 一次给全），不要为一个资产跑全关卡排行。
只有在要看「它拖了多少依赖」时才用 `ue_asset_size_map`。

## 失败处理的正例

用户问「分析一下项目」但引擎没开：

> 需要先启动虚幻编辑器、并装上 UnrealAgentLink 插件 —— 这些数据都要从运行中的
> 编辑器里读。装好之后我可以直接开始。

不要退化成「我可以为您介绍一些通用的 UE 优化建议」。用户问的是**他的**工程。
