# Instances, properties, and applying to actors

Read this when the ask is "make it adjustable", when a blend mode / shading model needs
changing, or when the material has to end up on actors in the level.

---

## Material instances — the right answer to "make it adjustable"

When the user wants variants ("wood, but I want to tweak the roughness", "three colours of the
same plastic"), do **not** create three master materials. Each master compiles its own shader.
Use one master with parameter nodes plus one instance per variant:

```
material_create                                     master
material_apply_graph  nodes: [{ id: "r", node_type: "ScalarParameter",
                               node_name: "Roughness" }]   ← node_name IS the parameter name
                      connections: [{ from: "r.Default", to: "Material.Roughness" }]
                                                    (lays out and compiles for you)
material_create_instance  parent_path: <master>     returns available_params
material_set_param        path: <instance>          set the values
material_apply            path: <instance>
```

**Only parameter nodes become adjustable.** A `Constant` wired into `Roughness` is baked in.
`ScalarParameter` and `VectorParameter` are the adjustable ones, and their `node_name` is the
name `material_set_param` will need later. `material_create_instance` returns
`available_params` — if it comes back empty, the master has no parameter nodes and the
instance has no knobs; say so instead of setting parameters that do nothing.

**Never guess a parameter name.** The engine does not validate them: `material_set_param` with
a misspelled name stores a record nothing reads and still reports success. Take the names from
`material_create_instance`'s `available_params` or from `material_describe`.

`material_set_param` operates on a **MaterialInstanceConstant**, not on a master material.

## Blend mode and shading model

`material_create` sets them at creation. `material_set_property` changes them afterwards, and
takes a **master material only** — pointing it at an instance returns 404. (UE itself lets an
instance override the blend mode; this tool cannot reach that, so change it on the parent.)
Either way, run `material_compile` afterwards or the change does not take effect.

Translucency is **two steps**: set `blend_mode: "Translucent"` *and* wire something into the
`Opacity` output pin. Setting the blend mode alone leaves the material fully opaque, and
nothing reports an error.

`material_describe` reads all three back.

## Applying to actors — two things go wrong here

**Match actors by their label.** `targets.names` compares against the actor label shown in the
World Outliner. The internal object name does not match. Note that the `name` reported by
`ue_spawn_actor` is the internal name — feeding that straight into `material_apply` fails with
a bare 404. Use `targets.filter.class` or `filter.name_pattern` when you are unsure of a label.

**Check the counts.** Actors without a StaticMeshComponent, or whose component has no mesh
assigned, are matched but silently skipped. The result says how many of how many were applied:

```
材质已应用到 1/4 个 Actor：SM_SkySphere。
⚠️ 有 3 个匹配到的 Actor 被跳过 …
```

Report the real number. Do not tell the user the material was applied to everything when
three quarters of the targets were skipped.

