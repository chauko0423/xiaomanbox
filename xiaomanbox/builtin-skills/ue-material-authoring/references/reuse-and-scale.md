# Reuse and scale — parameter collections, material functions, references

Read this when the ask is bigger than one material:

- **"全场景下雨"、"按队伍改配色"、"全体受击闪白"** → parameter collection
- **同一串节点要用在好几个材质里** → material function
- **要改或要删一个被别处引用的资产** → check the referencers first

---

## One switch driving the whole level — parameter collections

When the ask is "make it rain across the level", "tint everything by team colour", or
"flash all characters white on hit", the answer is a **Material Parameter Collection**, not
a pass over every material instance.

`material_parameter_collection` creates and edits the collection:

```json
{ "action": "create", "collection_name": "MPC_Weather",
  "scalars": [{ "name": "Wetness", "value": 0 }] }
```

Then reference the parameter from each material that should react to it, with
`material_apply_graph`:

```json
{ "path": "/Game/M_Ground", "nodes": [
  { "id": "wet", "node_type": "CollectionParameter",
    "collection_path": "/Game/Materials/MPC_Weather", "node_name": "Wetness" }] }
```

`collection_path` and `node_name` are **both required** — with either missing the node is
created but greyed out, and the material only fails at compile time with "Missing Parameter
Collection". The tool reports `collection_applied: false` when that happens; do not wire a
node up after seeing that.

Two reasons not to reach for per-instance parameters instead:

- it does not scale past a handful of materials, and
- instance parameters are edit-time only, so nothing can change them while the game runs.

Scalars and vectors are capped at **16 each** by the engine. Past that the extra parameters
are rejected and named in `rejected` — they are not silently dropped, but they are also not
there, so check the message rather than assuming the write landed.

## Reusing a group of nodes — material functions

The same chain of nodes appearing in three materials should be a **MaterialFunction**: edit
it once, all three follow. Rebuilding it per material means that after the third copy nobody
dares change it.

`material_create_function` makes one:

```json
{ "function_name": "MF_Wetness", "destination_path": "/Game/Materials" }
```

The function is created **empty, and these tools cannot fill it.** Every graph command
loads a `UMaterial`; handed a function path they answer `Material not found`, and
`FunctionOutput` is not among the node types that can be created. Do not try — it costs
several round trips and leaves a permanently empty asset in the project. Either ask the
user to author the function in the material editor, or skip the abstraction and build the
same nodes inside each material.

A function that is already filled in works normally. Call it from a material:

```json
{ "path": "/Game/M_Ground", "nodes": [
  { "id": "wet", "node_type": "MaterialFunctionCall",
    "function_path": "/Game/Materials/MF_Wetness" }] }
```

`function_path` is required. Without it the node is created with **no pins at all**, and the
first sign of trouble is a missing-pin error on the wire two steps later.

## Before you change or delete anything — check who uses it

`material_get_referencers` answers "what depends on this asset". It is read-only.

Use it before editing a master material (it may have a dozen instances inheriting from it,
all of which shift with your change) and before deleting a texture (something may still
reference it). The response tags each referencer with its class, so a material instance and
a level are distinguishable without a second lookup.

Reporting "I updated the material" without mentioning that it changed twelve instances is
a partial answer. Check first, then say what the blast radius was.
