# Node types, pin names, and two traps that cost a whole task

Look here when a node or pin name is not behaving, or when you are about to search a
graph you just wrote.

## Trap — you write one vocabulary and read back another

`blueprint_apply_graph` takes friendly names. `blueprint_get_graph` returns engine class
names, and node titles are localised to the editor's language. **Match on `class`.**
Never search the graph for the type name you passed in, and never match on title.

| you write | reads back as `class` |
|---|---|
| `Branch` | `K2Node_IfThenElse` |
| `Sequence` | `K2Node_ExecutionSequence` |
| `InputAction` | `K2Node_InputAction` |
| `Event` | `K2Node_Event` |
| `CustomEvent` | `K2Node_CustomEvent` |
| `VariableGet` / `VariableSet` | `K2Node_VariableGet` / `K2Node_VariableSet` |
| `Function` | `K2Node_CallFunction` |
| `Cast` | `K2Node_DynamicCast` |
| `CallDispatcher` | `K2Node_CallDelegate` |
| `BindEvent` | `K2Node_AddDelegate` |
| `UnbindEvent` / `UnbindAllEvents` | `K2Node_RemoveDelegate` / `K2Node_ClearDelegate` |

The dispatcher row is the worst case of this trap: the editor titles those nodes
"Call On Door Opened" and "Bind Event to On Door Opened", which are strings built from
the property name. No function table contains them. Pass the dispatcher's own name with
one of the four classes above.

## Trap — a wildcard pin does not take its type until the wire is really made

`blueprint_apply_graph` validates every connection first and only then makes them all, so it
can abandon the whole batch without having touched the graph. The cost: within one call, type
propagation is invisible. A ForEach's `Array Element`, an array function's `TargetArray` — those
pins settle their type when the upstream pin is *actually connected*, which happens after every
connection in the batch has already been checked.

So a ForEach feeding a Cast fails in one batch and works in two:

```
cannot connect 'foreach.Array Element' (wildcard Output) -> 'cast.Object' (wildcard Input)
```

The graph is right. Send it without those wires, then send a second call with `nodes: []`
carrying only them. Do not go rewrite the node types — that error with **wildcard on both
ends** is this trap, not a mistake of yours.

## Trap — the graph must already exist

`graph_name` defaults to `EventGraph`. Any other name must already exist; passing a new
one returns `404 Graph not found`. Need a new function graph? `blueprint_create_function`
first, then populate it.

## Custom events carry their own parameters

A `CustomEvent` node's pins come from `params`, not from anything it is wired to:

```json
{ "id": "ev", "class": "CustomEvent", "member_name": "OnHitBy",
  "params": [{ "name": "Instigator", "type": "object", "object_class": "Actor" },
             { "name": "Damage", "type": "float" }] }
```

`type` takes a scalar keyword (`bool` `int` `int64` `float` `double` `string` `name`
`text` `byte`), a struct or enum name (`Vector`, `Transform`, `EMyEnum`), or
`object` / `class` / `soft_object` / `soft_class` with `object_class`. Add
`container: "array" | "set" | "map"` for collections.

The params are the node's **output** pins, named exactly as given. The event's
`OutputDelegate` pin is what a `BindEvent` node's `Delegate` pin needs — see
`members-and-events.md`.

`params` is rejected on any other `class`; it is not silently ignored.

## Common pin names

Branch: `execute`, `then`, `else` in/out; `Condition` for data.
Event nodes expose `then`. `InputAction` exposes `Pressed`, `Released`, `Key`.
For anything else read it from `blueprint_search_nodes` or `blueprint_get_graph`.

Component event nodes carry their pins in the bind response — see
`members-and-events.md`; you do not need a second read to wire them.

## Uncommon node types

The named types cover the common cases. For a node with no named type, pass
`raw_class: "K2Node_Something"`. That path only works for nodes that allocate their own
pins with no extra configuration — if it comes back saying the node created no pins,
the node needs a named type and cannot be built this way.
