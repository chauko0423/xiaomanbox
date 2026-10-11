# Diagnosing a material that renders wrong

For an existing material that compiles into the wrong picture, or does not compile at all.
The order matters more than any individual step.

## 1. Compile first. Do not start by reading the graph

`material_compile` separates the two cases in one call:

- **errors > 0** — the material does not compile. What you see in the viewport is not what
  the graph says, and **no experiment on the level can tell you anything** while this is
  true: a broken material cannot respond to a light change, a volume being moved, or a
  parameter being set. Fix the compile before testing any theory.
- **compiles, still wrong** — the wiring is wrong. Now read the graph.

Reading a large graph first is the expensive mistake. A compile call names the broken node
directly; a 176-node graph dump does not.

## 2. The compile names the node

`error_nodes[]` gives `node_id`, `class` and the engine's own error text per broken node —
the same list the material editor uses to outline nodes in red. Take `node_id` straight to
`material_get_graph`.

A node carrying `note` is not in this material's own graph — it lives inside a
MaterialFunction, and that id will not be found by `material_get_graph`.

## 3. Read pin types, not just wires

The most common failure in a graph that looks correct is a type mismatch: a `float3`
feeding a pin that only accepts `float`. Nothing in the layout shows it.

- every node's `inputs[]` / `outputs[]` carries `type`
- every connection carries `from_type` and `to_type`
- root pins report their `expected_type` through `to_type`

Compare the two ends. The tool reports what each side is and deliberately does not judge
whether they are compatible — engine coercion has enough special cases that a verdict
would be wrong often enough to mislead.

## 4. Two things that make a graph look disconnected when it is not

**`use_material_attributes: true`** — the pins on the material output node are inert; the
only live input is `MaterialAttributes`. Wiring to `BaseColor` on such a material succeeds,
compiles, and changes nothing. Any material built around `MakeMaterialAttributes` is in
this mode.

**Named reroutes** — a wire drawn as two unconnected nodes. Following a chain backwards
stops at a usage node. It has not ended: `reroute_declaration_node` gives the `node_id` of
the matching declaration, and the chain continues above it. A usage carrying
`reroute_error` points at nothing and will fail to compile.

## 5. One wire at a time, recompile after each

Change one pin, call `material_compile`, watch the error count. `4 → 3 → 2 → 1 → 0` is a
verified repair. Changing four things and compiling once tells you nothing about which one
mattered.

## 6. Zero errors is not the goal — the picture is

A material that compiles is not a material that renders correctly. Before reporting a fix,
call `ue_screenshot` and look at the thing the user complained about. If it still looks
wrong, or the object is no longer rendering at all, say so — "compiles clean, but the grass
is still not green" is a useful report; "fixed" is not.

State plainly which part is verified and which part is still a theory.

## 7. Before touching a master material, check who else uses it

`material_get_referencers` first. Rewiring a master material changes every asset that
inherits from it. Bypassing a broken node (wiring a flat colour in place of a chain that
will not compile) is not a repair — it removes the feature and hides the real cause. If the
errors appeared in an asset that used to work, something upstream broke: a MaterialFunction,
a missing texture, a stricter shader model. Find that instead.

When a fix has to be destructive, say what it costs before doing it.

## 8. Everything you changed is undoable

Every mutating material call is recorded on the agent's own undo stack. `ue_undo_history`
lists the steps, `ue_undo` reverses them, `ue_save` writes the reversal to disk. Already
having saved does not block this.

The stack is cleared when the level changes. Undo before reloading a level, never after.

## 9. Do not read graphs through Python

`ue_run_python_script` is a dead end for this: `UMaterial.expressions` is protected and
cannot be read, and `get_material_property_input_node` returns `None` exactly on the
materials that use attribute chains. `material_get_graph` is the supported path.

## Known gaps

`material_get_graph` and `material_compile` accept a `UMaterial` only. Pointing either at a
MaterialFunction returns 404 — there is currently no way to read or compile a function's
internals. When the error is inside a function, say so rather than guessing at its contents.
