---
name: ue-project-audit
description_zh: 给 Unreal 工程或打开的关卡做体积、性能和资产问题体检，并把数据整理成带具体修复建议的分级报告。当用户要分析评审项目、要优化建议、问什么最占空间、打包为什么那么大、关卡为什么卡，说「分析一下这个项目」「哪些资源最占地方」时使用；整理大纲、修改或删除它查到的资产、Sequencer 和蓝图工作不适用于它。
description: Audits an Unreal project or the open level for size, performance and asset problems, and turns the numbers into a ranked report with concrete fixes. Use when the user asks to analyse or review their project, wants optimisation advice, asks what is taking up space, which assets are heaviest, why the packaged build is large, or says things like "分析一下这个项目"、"给点优化建议"、"哪些资源最占地方"、"这个关卡为什么这么卡"、"帮我看看场景有什么问题". Do not use for organising the World Outliner, for editing or deleting the assets it finds, or for Sequencer and Blueprint work.
---

# Project and level audit

Verified against UE 5.5.4 with UnrealAgentLink. Eight read-only tools. Nothing here changes the
user's project — this skill produces a report, not edits.

Handing part of an audit to a sub-agent? Send it with `read_only: true`. A sub-agent that can
write will "fix" what it finds — a real session had three review subtasks move six actors and
save the level, while the prompt said in so many words not to touch anything.

Report format and worked examples: `references/report-format.md`.
Which requests route here and which do not: `references/trigger-examples.md`.
Full performance diagnostic methodology — how a professional actually narrows "it's slow" down
to a specific cause, mapped onto the tools below: `references/performance-diagnosis.md`.
Turning a diagnosis into proof — duplicate the level, remove the suspect, measure the delta:
`references/experimental-verification.md`.

## The eight tools and what each one answers

| Tool | Answers | Scope |
|---|---|---|
| `ue_project_asset_ranking` | which assets take up space | whole project, any class |
| `ue_find_heavy_assets` | which meshes cost frame time here | the open level |
| `ue_asset_size_map` | why *this* asset is big | one asset + its dependencies |
| `ue_content_audit_optimization` | is the project configured well | project-wide settings |
| `ue_get_performance_stats` | is it fast right now | this instant, this editor |
| `ue_capture_perf_trace` | is it fast over a stretch of time, and what's the bottleneck | N seconds, this editor |
| `ue_insights_trace` (`action: capture`) | record a raw Unreal Insights trace | N seconds, this editor |
| `ue_insights_trace` (`action: analyze`) | which timer ate the most time, per thread (game / render / GPU) | one `.utrace` file |
| `ue_insights_trace` (`action: compare`) | did my change make it faster, and which timer moved | two `.utrace` files, before and after |

`ue_project_asset_ranking` reads asset-registry tags and file sizes without loading a single
asset, so it is safe to run over all of `/Game`. `ue_capture_perf_trace` and
`ue_insights_trace` both use engine-native profiling (the CSV Profiler and Unreal
Insights respectively) — they are the professional-grade path when a single
`ue_get_performance_stats` snapshot is not enough, see the dedicated section below.

## When one instant sample is not enough

`ue_get_performance_stats` is a snapshot — a rolling average that a single frame spike can
easily miss entirely. When the user asks "is there stuttering", "can you stress-test this",
or reports something that only happens sometimes, reach for the time-window tools instead:

- **`ue_capture_perf_trace`** — samples FrameTime / GameThreadTime / RenderThreadTime / GPUTime
  every frame for `duration_seconds`, using the engine's built-in CSV Profiler. Returns
  avg/min/max/p50/p95/p99 per stat, the single worst frame's breakdown, and a `bottleneck`
  verdict. This is the right default for "is it fast" over a stretch of time — cheap, no
  external process, answers in one call. **The call blocks for the full duration** — say so
  before running a 60+ second capture.
- **`ue_insights_trace`**, `action: "capture"` then `action: "analyze"` — for when the question
  is finer-grained than "which subsystem" — "which specific function or render pass". `capture`
  records a full Unreal Insights trace (`.utrace`); `analyze` takes that `utrace_path` and spawns
  Unreal Insights headlessly to rank timers separately for the game thread, render thread and
  GPU, plus the game thread's frame times and its longest single calls (hitch suspects).
  Generic task waits (`WaitForTasks` and the like) are summed separately, not ranked — a thread
  waiting on others is not the slow part. `start_seconds` / `end_seconds` narrow it to a slice
  of the trace when the start is loading or entering PIE. `compare` takes a before
  (`baseline_utrace_path`) and an after (`utrace_path`) trace and lists the biggest per-frame
  changes; only compare two recordings of the same activity. Slower, and the Insights CLI export path has a
  documented flaky-empty-output issue (not this tool's bug — say so if it happens, and offer to
  retry). Reach for `ue_capture_perf_trace` first; only go here when the user needs event-level
  detail `ue_capture_perf_trace`'s five stats cannot give.

Both capture tools flag `idle_sample` the same way `ue_get_performance_stats` does: if the
render thread and GPU are near-zero for the whole capture, the editor viewport was not actually
drawing (backgrounded/unfocused). Never report that as a performance finding.

All three tools already try to bring the Unreal Editor window to the foreground before sampling
(PowerShell `AppActivate` on Windows, targeted AppKit activation on macOS; not guaranteed to win focus on every OS/window-manager
configuration). So on `idle_sample: true`, **retry once yourself immediately** — do not stop and
ask the user to alt-tab manually first; that is asking them to do something the tool already
attempted. Only surface it to the user if it is still idle after that retry (rare — usually
multi-monitor focus quirks), and at that point say plainly what you observed ("auto-focus didn't
take, please click the Unreal Editor window and enter PIE") rather than re-explaining the whole
mechanism.

## Diagnosing "why is it slow" like a professional, not just reporting numbers

A bottleneck verdict alone ("GPU-bound") is not a diagnosis — it is step one of one. Professional
UE profiling is a fixed sequence: **classify which thread is the bottleneck → confirm it →
narrow down to the specific cause within that thread → fix → re-measure**, not "run a tool, read
whatever field it returns, done." `references/performance-diagnosis.md` has the full workflow —
sourced from Epic's own docs, AMD's GPUOpen guide, and Epic's game-thread optimization blog post,
mapped onto exactly which of our tools plays which role (and honest about the one classic step —
reading the on-screen `stat gpu`/`stat game` overlay — that this toolset genuinely cannot do).

Read it before telling a user "this is GPU-bound" or "this is a CPU problem" — that sentence
should come with *why*, not just a verdict field echoed back.

A structural diagnosis ("SM_Grass2 is 53% of the level's triangles and has Nanite off") is still
an inference, not proof. When the user pushes back with "what's the *real* cause" or asks you to
back up a claim, that is a request for evidence, not a longer explanation of the same guess —
go to `references/experimental-verification.md`: duplicate the level, remove the suspect, measure
the before/after delta. A before/after number is something the user can check themselves; a
confident-sounding inference is not.

## Workflow

Do not start by asking the user what they want audited. Run steps 1–3, report, then offer depth.
A concrete finding is a better question than "what would you like me to look at".

**1. Say where you are.** `ue_get_project_info` and `ue_get_current_level`. One line in the
report: which project, which engine version, which level, how many actors. Numbers without a
subject are unreadable a week later.

**2. Whole project by size.** `ue_project_asset_ranking` with no arguments (defaults to `/Game`,
sorted by disk size). Read `by_class` first — "textures are 68% of the project" decides where to
look before any individual asset does. Then the top rows.

**3. The open level by cost.** `ue_find_heavy_assets` sorted by `TotalTriangles`. Read
`finding_counts` and lead with `breaks_gameplay`: a user chasing frame rate still wants to know
their floor has no collision. Then `costs_frame_time`.

**4. Then go deeper on what step 2 or 3 turned up**, one thread at a time:

- biggest asset in the ranking → `ue_asset_size_map` on it, to see what it drags in
- lots of high-poly meshes without Nanite → `ue_content_audit_optimization` for the config side
- user says it feels slow *right now* → `ue_get_performance_stats`
- user says it stutters, asks for a stress test, or asks *why* it's slow → `ue_capture_perf_trace`
  over a real stretch of time, then follow `references/performance-diagnosis.md` to narrow the
  verdict down to a cause instead of stopping at "GPU-bound"

## Constraints

**Never invent a threshold.** `min_triangles` has no default and must not get one. "High poly"
is a project decision — a mobile title and an archviz render have nothing in common here. Either
ask what counts as heavy, or pass a number and say in the report which number you used and that
you chose it. `ue_find_heavy_assets` will not produce the `high_triangles_no_nanite` finding
without a threshold, and that is deliberate.

**Three different "sizes" — never mix them.**

- `disk_size` — the compressed package on disk. What ships. Nanite meshes and BC7 textures
  expand well past this when loaded.
- `resource_bytes` — the engine's estimate of resident memory for that asset.
- `texture_bytes` — estimated texture memory for the textures one mesh pulls in.

Saying "this mesh uses 40 MB" without saying which of the three is how a user ends up optimising
the wrong thing.

**A missing field is not a zero.** Asset-registry tags are written when an asset is *saved*. An
asset last saved by an older engine may have no `triangles` or `nanite` tag at all, so the field
is absent from the response. Absent means unknown. Reporting "0 triangles" from an absent tag
inverts the conclusion.

**Report what did not get ranked.** `ue_find_heavy_assets` returns `not_ranked_by_class` — lights,
skeletal meshes, decals, Niagara. They cost something; they just have no metric comparable to
triangles. Say "plus 240 lights and 12 skeletal meshes that this ranking does not cover", and
use `ue_project_asset_ranking` with `class_filter` if the user wants those looked at.

**Editor numbers are not shipped numbers.** Everything here is measured in the editor. Say so
once in the report. It matters most for `ue_get_performance_stats`.

**Engine and temp content is not the user's project.** `/Engine/…` and `/Temp/…` rows should not
appear in a report about their assets.

**Bound the output.** A production project has thousands of assets. Report the top rows you
actually looked at, say how many there were in total, and offer to go deeper. Do not paste 200
rows into the conversation.

## Failure handling

- **No engine connected** — every tool here fails the same way. Tell the user to start Unreal
  with the UnrealAgentLink plugin installed; do not retry.
- **`ue_asset_size_map` returns 404** — the path was wrong, usually an asset *name* was passed
  where a path belongs. Run `ue_content_search` for the name, then retry with the real path.
- **Frame rate reported near zero with render thread and GPU both at 0** — the editor window is
  in the background and is not drawing. That is not a performance problem. Ask the user to bring
  the editor forward and enter PIE, then sample again. Never report this sample as a finding.
- **`truncated: true`** — you saw a prefix, not the set. Any total you quote from it is a lower
  bound and must be labelled as one.
- **`ue_insights_trace` (`analyze`) reports an empty export** — this is a documented, not fully
  understood flakiness in the Insights CLI export path, not a bug in this tool. Say that plainly,
  and offer to capture again rather than guessing at numbers from nothing.
- **`ue_insights_trace` (`analyze`) can't find a matching engine install** — it looks up the engine
  version the connected project reports and searches installed engines for that exact version. If
  the engine lives at a non-standard path, the user needs to register it as a custom engine path
  in Preferences first.

## Escalation — what these tools still cannot do

Say this plainly rather than approximating it:

- **Real runtime memory** — `memreport -full` in the editor console writes a full report to
  `Saved/Profiling/MemReports/`. It is the only source of engine-computed memory numbers, and it
  is not wrapped as a tool. `ue_run_console_command` can trigger it and `read_local_file` can read
  the file, but the numbers describe the *editor*, which has far more loaded than the game will.
- **Draw calls, shader complexity, overdraw** — the optimization viewmodes and `stat gpu` /
  `stat scenerendering` render to the viewport as a Slate overlay, and the agent cannot read it.
  `ue_screenshot` does not help here: its default path renders the scene directly (SceneCapture),
  which never includes that overlay. See `references/performance-diagnosis.md` §8 for what to use
  instead (Insights timer ranking, or asset-structure clues) when the user needs this.
- **Memory and asset-loading data from Insights** — `ue_insights_trace` (`analyze`) exports timer
  statistics per thread and nothing else. The engine has no command-line export for Memory
  Insights or Asset Loading Insights: the `memory` / `loadtime` channels can be recorded, but the
  user has to open the `.utrace` in the Unreal Insights UI to read them. Say this rather than
  guessing at memory numbers from timer names.
- **RenderDoc frame captures** — requires the separate UE4RenderDocPlugin plus a standalone
  RenderDoc install; `renderdoc.CaptureFrame` opens an interactive GUI, it does not hand back
  data. Out of scope here; tell the user to use it directly if they need draw-call-level GPU
  debugging.

Recommending the right external tool is a complete answer. Approximating one of these numbers
from what is available is not.

## Sources for the diagnostic methodology

`references/performance-diagnosis.md` is not invented — it is Epic's and the wider engineering
community's own documented workflow, mapped onto this toolset:

- [Introduction to Performance Profiling and Configuration in Unreal Engine](https://dev.epicgames.com/documentation/en-us/unreal-engine/introduction-to-performance-profiling-and-configuration-in-unreal-engine) — Epic
- [How to improve game thread CPU performance in Unreal Engine](https://www.unrealengine.com/en-US/blog/how-to-improve-game-thread-cpu-performance) — Epic
- [Common Memory and CPU Performance Considerations in Unreal Engine](https://dev.epicgames.com/documentation/en-us/unreal-engine/common-memory-and-cpu-performance-considerations-in-unreal-engine) — Epic
- [Virtual Shadow Maps in Unreal Engine](https://dev.epicgames.com/documentation/unreal-engine/virtual-shadow-maps-in-unreal-engine) — Epic
- [Unreal Engine Performance Guide](https://gpuopen.com/learn/unreal-engine-performance-guide/) — AMD GPUOpen
- [UE5 Performance Profiling 101: Finding and Fixing Bottlenecks](https://www.strayspark.studio/blog/ue5-performance-profiling-101) — StraySpark Studio
- [DumpTicks tip](https://unrealdirective.com/tips/dumpticks/) — Unreal Directive
