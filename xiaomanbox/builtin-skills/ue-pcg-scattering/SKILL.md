---
name: ue-pcg-scattering
description_zh: 在 Unreal 里搭建和调试 PCG 图：在地形上程序化撒网格、一次调用生成整张图、回读嵌套生成器设置，以及排查 PCG 图产出为 0 个实例的原因。当用户要撒树、石、草或道具，要新建或重连 PCG 图，说「撒一片森林」「PCG 跑出来是 0 个实例」「随机撒点石头」「这张图为什么不出东西」时使用；手动摆单个 Actor、蓝图图、材质图、Sequencer 不适用于。
description: Builds and debugs PCG graphs in Unreal - scattering meshes over landscape, authoring a whole graph in one call, reading nested spawner settings back, and working out why a graph produced zero instances. Use when the user wants trees, rocks, grass or props scattered procedurally, wants a PCG graph created or rewired, or says things like "在地形上撒一片森林"、"PCG 跑出来是 0 个实例"、"随机撒点石头"、"这张 PCG 图为什么不出东西"、"铺满整片大世界"、"混交林". Do not use for placing individual actors by hand, for Blueprint or material graphs, or for Sequencer.
---

# PCG scattering

Verified against UE 5.5.4. PCG ships from UE 5.2, is Beta in 5.4-5.6, production-ready
from 5.7. It is an optional plugin, off by default.

## Quick start

```
pcg_scene_report     ← always first: which level, what terrain, which volumes
pcg_apply_graph      the whole graph in one call; creates the asset if missing
pcg_spawn_volume     place it, sized to the area to cover
pcg_execute          run it; returns how many instances actually appeared
```

Editing an existing graph also needs `pcg_get_graph` (node ids are engine generated) and
`pcg_describe_node` (reads nested settings back in full). `pcg_graph_parameters` adds the
knobs the user tunes in the details panel.

Nothing writes to disk. Call `ue_save` when done.

No landscape in the level (`pcg_scene_report` says 0)? Build one with `landscape_create`
before scattering — skill `ue-landscape`. It splits World Partition levels into streaming
proxies the same way the editor does, so samplers find the surface.

## Five traps that report success and produce nothing

**A sampler fed from the graph Input samples nothing.** It needs an explicit landscape
source:

```
PCGGetLandscapeSettings -> SurfaceSampler.Surface -> StaticMeshSpawner -> Output
```

**The graph endpoints have inverted pin names** — Input's output pin is `In`, Output's
input pin is `Out`. Omit `from_pin` / `to_pin` entirely; the tool picks the only visible
pin in that direction and confirms the edge by reading the graph back. Name a pin only
when the error says a node has several, as with `Surface` above.

**Spawner meshes are four levels down.** Use path syntax; indices grow the array:

```
"MeshSelectorParameters.MeshEntries[0].Descriptor.StaticMesh": "/Game/Trees/SM_Oak"
"MeshSelectorParameters.MeshEntries[0].Weight": 5
```

**Attribute selectors are written as one string, not as an object.** `InputSource`,
`TargetAttribute`, `ThresholdAttribute` and `OutputTarget` carry their own text format:

```
"InputSource":        "PCGBegin(Distance)PCGEnd"    a custom attribute
"TargetAttribute":    "PCGBegin($Density)PCGEnd"    a point property, with $
"ThresholdAttribute": "PCGBegin(@Last)PCGEnd"       the previous attribute
```

`{"AttributeName": "Distance"}` is rejected, and the error shows the current value in
the format above. It is rejected because it used to half-apply: the name landed, the
attribute-vs-point-property flag stayed at its old value, and the graph then generated
zero instances without an error. `pcg_describe_node` echoes selectors in this same
format — read one and copy the shape.

**`pcg_apply_graph` defaults to `mode: "replace"`** — everything but Input and Output is
cleared first, and a failed clear rolls the whole call back. `mode: "merge"` keeps
undeclared nodes; check `graph_nodes_after` in the reply for leftovers.

Pin tables, node type names and copyable graphs: **`references/wiring.md`**.

## "I want to tune these values in the details panel"

That is the graph's **user parameters**, and `pcg_graph_parameters` is the whole
interface — read, create, retype-guard, set, remove, in one call:

```
pcg_graph_parameters  graph_path, parameters: [{name, type, value}]
```

Types: `bool int32 int64 float double name string text object softobject class`.
Creating needs `type`; changing a value does not. Naming an existing parameter with a
different `type` is refused rather than silently retyped — remove it first.

Wire a parameter into the graph with a `PCGUserParameterGetSettings` node feeding the
target node's **Overrides** pin. Changing the value on the component then regenerates.

**Create the parameters before `pcg_spawn_volume`, not after.** A placed PCG component
keeps its own copy of the bag. Measured 2026-09-18: a volume spawned before the
parameters existed sat on `is_overridden=true` with a stale value, and later edits to the
graph never reached it (20 → 20 → 20 across three changes). Right order: create the
parameters, wire them, then spawn. If a volume is already placed, edit the value on the
component itself, or delete and re-spawn the volume — editing the graph will not do it.

Do not build a separate "parameter actor" with instance-editable variables and
`PCGGetActorPropertySettings` to fake this. It works, but the tuning UI ends up on an
unrelated actor instead of on the PCG component, which is not what the user asked for.
The one exception is UE 5.2-5.4, where `pcg_graph_parameters` returns 501: the property
bag lives in an experimental plugin that is off by default there, so the actor route is
the only one, and you should say so rather than retrying.

## Zero instances: check in this order

1. `engine_log` in the `pcg_execute` reply — the engine states the reason there
2. re-run with `verbose: true` — every node reports what its pins received
3. `pcg_diagnose` — unwired Output, unwired Input, unfed Surface pin, orphan nodes,
   spawner without a mesh, World Partition
4. only then the level: does the volume overlap terrain at all

To separate a broken graph from a bare level, run a `PCGCreatePointsGridSettings` graph.
It generates points from nothing, so instances prove the pipeline works.

`instance_count` counts meshes on the volume. A partitioned component puts them on
`PCGPartitionActor`s instead, reported separately as `partitioned_instance_count`.

## World Partition

Unloaded cells hold no landscape proxy actor at all, so a sampler correctly finds no
surface. Load the region before generating.

To cover the whole world, the user ticks **Is Partitioned** on the PCG component; PCG then
generates per grid cell. These tools cannot set it — the engine function that registers
the change is not reflected, and this plugin deliberately does not link PCG so it can
install where PCG is absent. Baking every cell is the World Partition PCG Builder
commandlet's job; for gameplay use runtime generation with hierarchical generation.

## Code 501

The plugin is missing or disabled, not a bad request. `pcg_status` distinguishes: engines
older than 5.2 have no PCG; otherwise the user enables it under Edit > Plugins and
restarts. `node_type_count` above zero means PCG is alive.
