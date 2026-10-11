# Wiring a material graph — pin names and the traps around them

Read this before wiring a graph you did not just build, or when `material_apply_graph`
comes back with a pin error.

---

## Chained graphs work — read the pin names first

`material_apply_graph` wires expression to expression as well as expression to the material
output; both ends of a connection may be nodes you are creating in this call, real node ids
already in the graph, or a node's `guid`.

```
Time            → Sine.Input        → Material.EmissiveColor
TextureSample   → Multiply.A        → Material.BaseColor
```

Input pin names differ per node and are not guessable — `Sine` takes `Input`, `Lerp` takes
`A`/`B`/`Alpha`, `TextureSample`'s UV input is called **`Coordinates`**, not `UVs`.

**Call `material_search_nodes` before you write the graph.** One call returns the full pin
signature for a dozen node types — names, types, and which extra fields the type needs to
not be a dead node. Do not build a scratch node just to read its pins back, and never build
a whole probe material for this; that costs a round trip per node type and leaves junk
assets in the user's project.

`material_get_graph` reads pins too, but only for nodes that already exist in that graph.
Use it for the graph in front of you; use `material_search_nodes` for types you have not
placed yet.

`material_get_graph` also returns every connection. Use it to confirm your wiring actually
landed — a graph that compiles clean can still have nothing connected.

Material output pins: `BaseColor`, `Metallic`, `Roughness`, `Specular`, `Normal`,
`EmissiveColor`, `Opacity`, `AmbientOcclusion`, `WorldPositionOffset`.

## Output pins: channels have names, use them

Multi-output nodes are addressed by channel name on the `from` side: `TextureSample` gives
`RGB`/`R`/`G`/`B`/`A`/`RGBA`, and `Constant3Vector` gives `RGB`/`R`/`G`/`B`. Write
`from: "tex.A"` or `from: "c3.G"`. Single-output math nodes (`Add`, `Multiply`, `Sine`)
report one output named `Out`; `Default` is also accepted there.

To take one channel out of a float2/float3, use a `ComponentMask` node with
`value: "R"` / `"G"` / `"RG"` (xyzw spelling also works). Do **not** reach for
`DotProduct` against `(1,0)` — that doubles the node count for something one node does.
A `ComponentMask` without a `value` is rejected at creation: its four channel bits default
to off, which compiles to a constant zero and is invisible on the graph.

## Textures in one step

A `TextureSample` node in `material_apply_graph` takes `texture_path` directly. Do not
create the node and then set the texture with `material_set_node_value` — one call does it.

## There is no per-node write path

Adding one node or one wire goes through `material_apply_graph` too (`nodes` may be an
empty array). There is deliberately no add-a-node tool — do not go looking for one.

<details>
<summary>旧写法</summary>

A per-node write path existed once. It never laid the graph out, so anything built that
way came out as a pile of overlapping nodes; that is why the whole path was removed
rather than documented.

</details>

