# PCG wiring reference

## Contents

- Graph endpoint pins
- Finding pin names
- Node types
- Copyable graphs
- Property paths

## Graph endpoint pins

| Node        | Connectable side | Pin label |
| ----------- | ---------------- | --------- |
| Input node  | output (right)   | `In`      |
| Output node | input (left)     | `Out`     |

Both are the opposite of what the direction suggests. Address them by the aliases `Input`
and `Output`; their real names are `DefaultInputNode` / `DefaultOutputNode`, reported by
`pcg_get_graph` as `input_node_id` / `output_node_id`.

## Finding pin names

Pin labels are per node type with no cross-node convention. `pcg_get_node_schema` lists
them for a type; `pcg_get_graph` lists them for nodes already placed. Both report only
pins visible in the editor, which is the connectable set.

Omit pin names by default. Supply one when the failure message lists candidates.

## Node types

| Purpose                    | Type name                      |
| -------------------------- | ------------------------------ |
| Read the level's landscape | `PCGGetLandscapeSettings`      |
| Sample points on a surface | `PCGSurfaceSamplerSettings`    |
| Deterministic point grid   | `PCGCreatePointsGridSettings`  |
| Spawn static meshes        | `PCGStaticMeshSpawnerSettings` |

`pcg_list_node_types` lists what this project actually has; the set varies by engine
version and enabled plugins. Narrow with `search: "Sampler"`. Short names resolve —
`SurfaceSampler` becomes `PCGSurfaceSamplerSettings`. Unknown types come back with the
closest matches.

## Copyable graphs

Scatter over terrain:

```
nodes: [
  { id: "land",    type: "PCGGetLandscapeSettings",   x: 0,   y: 0 },
  { id: "sampler", type: "PCGSurfaceSamplerSettings", x: 400, y: 0,
    properties: { "PointsPerSquaredMeter": 0.01 } },
  { id: "spawner", type: "PCGStaticMeshSpawnerSettings", x: 800, y: 0,
    properties: {
      "MeshSelectorParameters.MeshEntries[0].Descriptor.StaticMesh": "/Game/Trees/SM_Oak",
      "MeshSelectorParameters.MeshEntries[0].Weight": 1
    } }
],
edges: [
  { from: "land",    to: "sampler", to_pin: "Surface" },
  { from: "sampler", to: "spawner" },
  { from: "spawner", to: "Output" }
]
```

Deterministic points, independent of level content — use this to prove the pipeline works:

```
nodes: [
  { id: "grid",    type: "PCGCreatePointsGridSettings",  x: 0,   y: 0 },
  { id: "spawner", type: "PCGStaticMeshSpawnerSettings", x: 400, y: 0,
    properties: {
      "MeshSelectorParameters.MeshEntries[0].Descriptor.StaticMesh": "/Game/Trees/SM_Oak",
      "MeshSelectorParameters.MeshEntries[0].Weight": 1
    } }
],
edges: [
  { from: "grid",    to: "spawner" },
  { from: "spawner", to: "Output" }
]
```

A mixed forest is the same graph with one `MeshEntries[n]` pair per species; `Weight` sets
relative frequency.

## Property paths

```
PointsPerSquaredMeter                                          top-level scalar
MeshSelectorParameters.MeshEntries[0].Descriptor.StaticMesh    first mesh
MeshSelectorParameters.MeshEntries[0].Weight                   its weight
```

Indices beyond the current length grow the array. Asset paths may omit the object suffix.
Every write is read back and the stored value returned. `pcg_describe_node` emits the same
keys when reading, which is what makes read-modify-rewrite safe.
