# Editing an existing material graph

The main `SKILL.md` covers building a graph. This file covers **changing one that already
exists** — the part where things get taken back out, not just added.

Read this when you have wired something wrong, or when a graph has accumulated leftovers
from trial and error.

---

## Rewiring — disconnect, do not rebuild

`material_disconnect_pins` removes one connection. It targets the **input** end, because an
input takes exactly one wire, so naming the input identifies the wire unambiguously. An
output can fan out to several inputs, so "disconnect this output" would be ambiguous and is
not supported.

```json
{ "material_path": "/Game/M_Wood", "target_node": "Material", "target_pin": "BaseColor" }
```

`target_node: "Material"` targets the main material node; anything else is a `node_id`.
Omit `target_pin` on a single-input node (`Sine`, `Abs`, `OneMinus`, …).

## `node_id` is positional — use `guid` across a delete or an undo

`node_id` is `ClassName_<array index>`. Delete one node and every node after it shifts
down one; undo a node's creation and the next node you add gets the freed id back. A
`node_id` you read before that point now names a **different node**, and reusing it wires
the wrong node without any error.

Every node in `material_get_graph` carries a `guid`, and `material_apply_graph` reports one
per node it creates. It is the engine's own identity for the expression — stable across
deletes, undo, and reopening the project. Every id argument accepts either form.

Read the graph, edit, done: `node_id` is fine. Anything that spans a delete or an undo:
use `guid`. This has bitten real sessions — the same `node_id` pointed at two different
nodes three times in a row in one test.

Disconnecting an input that was already empty is **not** an error — call it when you just
want to be sure a pin is clear.

Before this existed, fixing one wrong wire meant deleting the whole node and rebuilding it,
which also threw away the correct connections on that node. Do not fall back to that.

## Clean up before you finish

`material_delete_unused_nodes` finds expressions that do not reach any material output and
removes them. It is **dry run by default** — read the list, confirm none of them is an
intermediate you still need, then call again with `dry_run: false`.

Worth doing once the graph is finished: leftovers from trial and error do not affect
rendering, but every later `material_get_graph` has to read them, and an unattached
`Multiply` sitting in the graph reads like something that was meant to be wired up.

Compile after any of this — the change is not live until you do.

