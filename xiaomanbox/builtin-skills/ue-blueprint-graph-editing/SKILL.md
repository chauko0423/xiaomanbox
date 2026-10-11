---
name: ue-blueprint-graph-editing
description_zh: 在已有蓝图里搭建或修改逻辑：一次声明式调用完成写节点、连线、引脚默认值和排版，能按编译报错自修复，并管理逻辑依赖的成员（变量及细节面板行为、事件分发器、组件事件、函数参数、父类、网络复制）。当用户要给蓝图加行为，说「走进触发区就开门」「点一下捡起来」「让策划能调这个参数」「继承 Character」「函数加个参数」时使用；新建蓝图资产、材质图、Sequencer 不适用于。
description: Build or modify logic inside an existing Blueprint — write nodes, wiring, pin defaults and layout in one declarative call, repair from compile diagnostics, and manage the members that logic depends on (variables and their details-panel behaviour, event dispatchers, component events, function parameters, parent class, replication). Use when the user wants behaviour added to a Blueprint, or says things like "走进触发区就开门"、"点一下就捡起来"、"让策划能调这个参数"、"门开了通知别的东西"、"这个 BP 应该继承 Character"、"这个函数再加个参数". Do not use for creating the Blueprint asset itself, for material graphs, or for Sequencer.
---

# Blueprint graph editing

## Quick start

```
blueprint_search_nodes      look up every function you need, in one or two calls
blueprint_apply_graph       write all nodes + connections + pin defaults at once
                            (it lays out and compiles for you)
```

Editing what is already there is the same loop you would use on a source file:
`blueprint_get_graph` returns the exact node/connection shape `blueprint_apply_graph`
accepts. Read it, edit it, write it back.

## Two rules that decide whether this goes well

**1. Never build a graph one node at a time.** There is no add-a-node tool and no
connect-a-pin tool, deliberately. Node-at-a-time editing costs a round trip per node and
loses track of what is already in the graph. Decide the whole shape first, write it in
one call. If you want to "add just one more node", re-send the complete graph with it
included.

Taking something *out* is the exception: `blueprint_delete_node` and
`blueprint_disconnect_pins` do exactly one thing each — as does `blueprint_comment`,
which frames a stretch of logic in a comment box. See `references/writing-graphs.md`.

**2. Never guess a function or pin name.** `blueprint_search_nodes` returns the exact
`member_name` and every pin's name, type and direction without creating anything:

```
blueprint_search_nodes { query: "print", blueprint_path: "/Game/BP_Door" }
→ member_name: "KismetSystemLibrary.PrintString"
  params: InString (FString, Input), bPrintToScreen (bool, Input), ...
```

Pass `blueprint_path` so the parent class's members are searched too
(`SetActorLocation`, `K2_DestroyActor`, …). One call covers many functions — search for
everything before writing anything. `is_pure: true` means no execution pins: wire data
only.

**The one thing the search cannot see: member functions on other classes.** Its range is
function libraries plus this Blueprint's own parent chain. `TextBlock.SetText`,
`ProgressBar.SetPercent`, `StaticMeshComponent.SetStaticMesh` are on none of those, so they
come back as zero matches — while `blueprint_apply_graph` resolves them fine, because it
looks the class up by name. Write `member_name: "TextBlock.SetText"` (class name without the
`U`) and it works.

So rule 2 means *do not invent names*, not *only use what the search returned*. When you are
setting something on a control or component and the search offers a
`KismetSystemLibrary.SetXxxPropertyByName` instead, **do not take it** — those write the field
by reflection and never notify Slate, so the value changes and the screen does not. Name the
class yourself.

## Writing

```json
{
  "blueprint_path": "/Game/BP_Door",
  "nodes": [
    { "id": "begin", "class": "Event", "member_name": "ReceiveBeginPlay" },
    { "id": "say", "class": "Function",
      "member_name": "KismetSystemLibrary.PrintString",
      "pin_defaults": { "InString": "door ready", "bPrintToScreen": "true" } }
  ],
  "connections": [{ "from": "begin.then", "to": "say.execute" }]
}
```

- `id` is yours, local to this call. Connections and diagnostics use it.
- **Literals go in `pin_defaults`.** Never create a variable to hold a constant.
- Variables used by `VariableGet`/`VariableSet` must exist first
  (`blueprint_add_variable`).
- Omit `position` and it is laid out for you, following execution flow.

## Decide the members before you write the graph

The graph is only half of a Blueprint, and several members change what the graph should
look like. Route on what the user actually said:

| the ask sounds like | what it needs |
|---|---|
| "让策划能调这个速度" | `blueprint_set_variable_meta` — a new variable is **not** instance-editable |
| "走进触发区就开门"、"点一下就捡起来" | `blueprint_component_event` — bind the delegate, **never** poll on Tick |
| "门开了通知别的东西" | `blueprint_event_dispatcher` — broadcast; do not make the listener poll |
| "这个函数再加个参数" | `blueprint_function_signature` |
| "这个 BP 应该继承 Character" | `blueprint_set_parent_class` — compiles and returns the breakage |
| 联机同步一个变量 | `replication` on `blueprint_set_variable_meta` |

The first three change the shape of your graph, so settle them first. Full usage,
including how to wire a bound event's returned `node_id` straight into
`blueprint_apply_graph`, is in **`references/members-and-events.md`**.

## When something goes wrong

- a call failed, came back with `warnings`, or reported compile errors →
  **`references/writing-graphs.md`** (it is all-or-nothing: fix and re-send the
  *complete* set, not the remainder)
- a node type or pin name is not behaving, or you are about to search a graph you just
  wrote → **`references/node-types-and-traps.md`** (you write `Branch`, it reads back as
  `K2Node_IfThenElse`)

## Irreversible

`blueprint_remove_variable` and `blueprint_function_signature`'s `remove_function` orphan
every node that referenced them, and neither can be undone from here. Run
`blueprint_compile` afterwards and tell the user what broke.
