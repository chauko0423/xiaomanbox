---
name: ue-material-authoring
description_zh: 创建母材质、连线改线、编译、创建材质实例、应用到 Actor、诊断材质显示不对或编译不过的问题，并用参数集合、材质函数、引用查询扩展规模。当用户要新材质，要改现有材质的图/属性/参数，说「接错线改一下」「材质显示不对」「草是紫的没颜色」「编译不过」「全场景下雨」「按队伍改配色」「这串节点想复用」时使用；Substrate 图（Slab、分层、彩色玻璃）、蓝图图、导入贴图、Sequencer 不适用于。
description: Create a master material, wire and rewire its graph, compile it, create instances, apply it to actors, diagnose one that renders wrong or will not compile, and scale it up with parameter collections, material functions and reference lookups. Use when the user wants a new material, wants an existing material's graph, properties or parameters changed, or says things like "接错线了改一下"、"图里有一堆没用的节点"、"这个材质显示不对"、"草是紫的没颜色"、"材质编译不过"、"全场景下雨"、"按队伍改配色"、"这串节点想复用"、"这个材质被谁用了". Do not use for Substrate graphs (Slab, FrontMaterial, layering, colored glass), for Blueprint graphs, for importing textures, or for Sequencer.
---

# Material authoring

The limits described here and in `references/` are real limits of the current engine
plugin, not style advice. Verified against UE 5.5.4, except the instance/property/value
parts, which compile but have not been run against a live editor yet.

## Quick start

```
material_search_nodes  pin signatures for the node types you are about to use
material_create        master material
material_apply_graph   the whole graph in one call — nodes, wiring, layout, compile
material_apply         put it on actors in the level
```

**`material_apply_graph` is the only way to add nodes or wires.** You give nodes your own
local ids and wire them as `"myNode.Pin"`; it maps them to the engine's real node ids,
lays the graph out and compiles. There is no add-a-node tool and no connect-a-pin tool,
deliberately — that path never laid anything out, so a graph touched that way ended up as
overlapping nodes. One-off touch-ups go through this tool as well; `nodes` may be an empty
array.

```
nodes: [{ id: "base", node_type: "Constant3Vector", value: { r: 0.8, g: 0.1, b: 0.1 } }]
connections: [{ from: "base.Default", to: "Material.BaseColor" }]
```

One caveat it states in its own output: the engine has no atomic graph write, so
**it stops at the first failure and does not roll back**. Read the report — it lists the
ids already created — and continue from there rather than re-sending the whole graph.

Editing something that already exists: `material_get_graph` first (node ids are not
guessable), `material_describe` first (it reports master vs instance, blend mode, shading
model, two-sided, parent, and every parameter's current value). Both endpoints of a
connection may be real node ids, so `material_apply_graph` also appends to an existing
graph.

On a large master material (hundreds of nodes) `material_get_graph` lists only part of the
graph. When you care about one input, read that part in full with `material_graph_slice`:
`from: ["OpacityMask"]` (or a node id / guid) walks upstream `depth` layers, follows named
reroutes, and lists the nodes where it stopped so you can continue from there.

**Repairing something that renders wrong is a different job from editing it.** Start with
`material_compile`, not with the graph — a material that does not compile cannot respond to
any experiment you run, so every theory tested before that point is untestable.
Full order of operations: **`references/diagnosing.md`**.

**Nothing here writes to disk.** Call `ue_save` when the material is done.

**Substrate (UE 5.4+).** A project having Substrate switched on is not a reason to build
with Substrate. When the user asks for an ordinary material and the material output still
lists BaseColor / Roughness / Opacity (5.6+ converts those behind the scenes), build it the
classic way described here. Switch to skill `ue-material-substrate` when the user asks for
Substrate by name or for layered coats, colored glass, fuzz or thin film, or when the
material output offers only `FrontMaterial` and a few others (5.4/5.5 with Substrate on).

Every tool takes `path` for the asset it acts on, `destination_path` for where a new
asset goes, and `texture_path` / `function_path` / `collection_path` for assets it merely
references. There is no table to memorise.

## Two things that silently produce a wrong result

**Pin names are per-node and not guessable.** `Sine` takes `Input`, `Lerp` takes
`A`/`B`/`Alpha`, `TextureSample`'s UV input is `Coordinates` and not `UVs`. Call
**`material_search_nodes`** before writing a graph — one call returns the full signature
for a dozen node types. Use `material_get_graph` for nodes that are already in the graph
in front of you. Never build a scratch node, or a whole probe material, to read pin names
back. Channel names, `ComponentMask` and the material output pin list:
**`references/wiring.md`**.

**Parameter names are never validated.** `material_set_param` with a misspelled name
stores a record nothing reads and reports success. Take names from
`material_create_instance`'s `available_params` or from `material_describe` — never from
memory.

Also: a `TextureSample` node takes `texture_path` directly. One call, not add-then-set.

## Route on what the user is actually asking for

| the ask sounds like | where to go |
|---|---|
| "想调粗糙度"、"三个颜色的同一种塑料" | instances — **`references/instances-and-apply.md`** |
| 改混合模式 / 着色模型、上到场景物体 | same file |
| "接错线了"、"图里一堆没用的节点" | **`references/graph-editing.md`** |
| "这个材质显示不对"、"草是紫的"、编译不过 | **`references/diagnosing.md`** |
| "全场景下雨"、"按队伍改配色"、"这串节点想复用"、"这个材质被谁用了" | **`references/reuse-and-scale.md`** |

The one to get right up front: **variants are instances, not copies.** Three master
materials means three compiled shaders. One master with `ScalarParameter` /
`VectorParameter` nodes plus one instance per variant is the answer, and only parameter
nodes become adjustable — a `Constant` wired into `Roughness` is baked in.

`material_duplicate` is for the other thing: forking a material into a genuinely different
one, or keeping a copy before you rewire a graph you might break. It copies the whole asset,
so the copy has its own graph to edit — which is exactly what an instance cannot give you.
Reach for it for "改一版但原来那份留着", never for "同一种材质来五个颜色". Name clashes get a
suffix, so trust `new_path` in the reply rather than the name you asked for.

## Three failures that look like success

- **Translucency is two steps.** Setting `blend_mode: "Translucent"` without wiring
  `Opacity` leaves it fully opaque and nothing reports an error.
- **A `MaterialFunctionCall` without `function_path`, or a `CollectionParameter` without
  `collection_path` + `node_name`, is created anyway** — greyed out or with no pins at
  all. The response carries `collection_applied: false`; do not wire past it.
  (A `ComponentMask` without `value` is the same class of trap, but that one is refused
  at creation rather than left in the graph — its channel bits default to off, which
  compiles to a constant zero and looks perfectly normal on the graph.)
- **`material_get_referencers` only sees saved packages.** "Nothing references this" is
  not a safe basis for deleting anything while unsaved packages exist. The response says
  so; save first rather than talking past it.
