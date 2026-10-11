---
name: ue-material-substrate
description_zh: 制作 Substrate 材质（UE 5.4+）：Slab BSDF、分层叠加与混合算子、Coverage、清漆、绒毛、薄膜、彩色玻璃，以及专用 UI/后处理/贴花/灯光函数节点，还能在项目未开启时切换到 Substrate。当用户提到 Substrate/Strata，要做分层或物理混合材质，说「做个车漆」「彩色玻璃」「清漆层」「湿润层/灰尘层叠上去」「Front Material 怎么接」「开启 Substrate」时使用；未开 Substrate 的普通 PBR 材质、材质实例与参数、蓝图图、导入贴图不适用于它。
description: Author Substrate materials (UE 5.4+) - Slab BSDFs, layering and blending operators, coverage, clear coat, fuzz, thin film, colored glass, and the dedicated UI / post-process / decal / light-function nodes - and switch a project to Substrate when it is off. Use when the user mentions Substrate or Strata, wants layered or physically combined materials, or says things like "用 Substrate 做个车漆"、"做一个 Substrate 材质"、"彩色玻璃"、"清漆层"、"湿润层 / 灰尘层叠上去"、"丝绒布料"、"肥皂泡 / 油膜彩虹色"、"Front Material 怎么接"、"开启 Substrate"、"这个 Substrate 材质编译不过". Do not use for ordinary PBR materials in a project that has Substrate off, for material instances and parameters, for Blueprint graphs, or for importing textures.
---

# Substrate materials

Everything below is checked against engine source for UE 5.4 to 5.8 and was run in a
live 5.8 editor through these tools. Node pin names come from the engine, so still call
`material_search_nodes` before writing a graph; the tables in `references/` are for
planning.

## The model in one paragraph

A Substrate material is built from **Slabs**, where each Slab is one physically described
layer (surface reflection plus what is underneath). **Operators** combine Slabs: side by
side through a mask, stacked as a coat, or faded by coverage. The finished tree plugs into
**one** input on the material output, `FrontMaterial`. Once `FrontMaterial` is wired,
the classic output pins (BaseColor, Metallic, Roughness, Normal, Emissive, ...) stop
counting.

## Quick start

```
1. material_search_nodes  query "Substrate"   → are nodes available? pins and properties
2. material_create                             → master material
3. material_set_property  (only if not Opaque) → blend mode, see the table below
4. material_apply_graph   nodes + connections  → last wire goes to Material.FrontMaterial
5. read the compile result in its reply, then ue_save
```

Minimal graph (a blue dielectric):

```
nodes: [
  { id: "albedo", node_type: "Constant3Vector", value: { r: 0.1, g: 0.3, b: 0.8 } },
  { id: "slab",   node_type: "SubstrateSlabBSDF" }
]
connections: [
  { from: "albedo.RGB", to: "slab.Diffuse Albedo" },
  { from: "slab.Out",   to: "Material.FrontMaterial" }
]
```

Pin names contain spaces (`Diffuse Albedo`, `Top Thickness`); write them as the search
result prints them. Every Substrate BSDF and operator has one output, `Out`.

Node **settings** that are not pins, such as the Slab's `SubSurfaceType` or an
operator's `bUseParameterBlending`, go in the node's `properties` in
`material_apply_graph`, or through `material_set_node_value` for a node that already
exists. `material_search_nodes` lists every setting with its default, and the reply
reports the value the engine actually stored.

## Step 1 decides everything: is Substrate on?

`material_search_nodes` with query "Substrate" answers it:

- **Nodes listed and no warning at the top** means Substrate is on. Continue.
- **A warning line saying Substrate is not enabled** means the project runs the classic
  renderer, and every Substrate node or `FrontMaterial` link will be refused. Do not
  work around it. Tell the user what switching costs: an editor restart, a recompile of
  every shader in the project, and existing materials change how they are handled. Ask
  them. If they agree:
  `ue_set_config` with config Engine, section `/Script/Engine.RendererSettings`, key
  `r.Substrate`, value `True`, then `ue_restart_editor`. It is read only at startup, so
  nothing changes until the restart.
- **A warning saying the engine is older than 5.4** means Substrate cannot be used. Build
  the look with the classic pins and say which part of the request that loses.

**Substrate being on does not mean every material must use it.** When the user asks for
an ordinary material ("普通的"、"传统做法"、plain plastic, metal, translucent) and the
material output lists BaseColor / Roughness / Opacity, which is the case on **5.6 or
later** while `FrontMaterial` is empty, build it with those classic pins: the engine
converts them behind the scenes, and Opacity works as usual. Do not tell the user the
classic pins stop working just because the project has Substrate on. Use Substrate nodes
when the look needs layers, coats, fuzz, thin film or colored transmission, or when the
output offers only `FrontMaterial` and a few others (5.4/5.5 with Substrate on, where the
classic pins are unsupported). Version details: **`references/versions.md`**.

## Pick the building block

| the user wants | build | recipe |
|---|---|---|
| plain surface, metal or dielectric | `SubstrateSlabBSDF` (+ `SubstrateMetalnessToDiffuseAlbedoF0` for a metalness workflow) | R1 |
| car paint, lacquer | `SubstrateSimpleClearCoatBSDF`, or two Slabs in `SubstrateVerticalLayering` | R2 |
| skin, wax, jade | Slab with SSS (`SubsurfaceProfile` or `SSS MFP`), Opaque only | R3 |
| velvet, cloth sheen | Slab `Fuzz Amount` / `Fuzz Color` | R4 |
| colored glass, liquid | Slab + `SubstrateTransmittanceToMFP`, `SubSurfaceType=MSS_SimpleVolume` (5.6+), `SubstrateWeight`, blend `TranslucentColoredTransmittance` | R5 |
| soap bubble, oil slick | `SubstrateThinFilm` into the Slab's `F0` / `F90` | R6 |
| two materials by a mask | `SubstrateHorizontalMixing` | R7 |
| wet / dust / snow coat | `SubstrateVerticalLayering`, top coat faded by `SubstrateWeight` | R8 |
| glowing, unlit | `SubstrateUnlitBSDF` or Slab `Emissive Color` | R9 |
| water, hair, eye | the dedicated BSDF, alone at the root | R10-R12 |
| UI, post process, light function, decal, fog volume | the dedicated node, plugged straight into `FrontMaterial` | R13 |

Graphs for every row: **`references/recipes.md`**. Every node's pins, defaults and
settings: **`references/nodes.md`**.

## Rules the engine enforces

- **`FrontMaterial` takes Substrate data only.** A color or number wired into it is
  refused. Put a BSDF in between.
- **With `FrontMaterial` wired, the classic output pins are ignored**, and connecting to
  them is refused. Pins that still count: `WorldPositionOffset`, `AmbientOcclusion`,
  `OpacityMask` (Masked), `Refraction` (translucent with distortion), `PixelDepthOffset`,
  `Displacement` (tessellation). `material_get_graph` lists only the pins in effect and
  flags leftover wires as not taking effect.
- **In a Substrate material, opacity is coverage.** Fade a BSDF with `SubstrateWeight`
  (`A` = the BSDF, `Weight` = 0 transparent … 1 solid). Once `FrontMaterial` is wired,
  the output's `Opacity` pin only matters in AlphaComposite. A classic material (nothing
  on `FrontMaterial`) keeps using the `Opacity` pin.
- **Subsurface scattering needs Opaque or Masked.** On a translucent material the engine
  forces the blend mode back to Opaque.
- **One BSDF output feeds one place.** Wiring the same Slab's `Out` into two operators
  fails to compile. Add a second Slab.
- **At most 15 operators and BSDFs in total, and at most 4 distinct normal/tangent
  inputs.** Share one normal source between Slabs.
- **The material domain follows the graph.** A `SubstrateUI`, `SubstratePostProcess`,
  `SubstrateLightFunction`, `SubstrateConvertToDecal` or `SubstrateVolumetricFogCloudBSDF`
  at the root sets the domain. A Slab at the root of a UI material turns it back into a
  surface material.
- **One material represents one kind of BSDF.** Do not mix Hair, Eye or water with Slabs.
  In 5.8, Horizontal, Vertical and Add accept only Slabs; earlier versions accept others
  but still render them wrong.

## Blend modes

| look | blend_mode |
|---|---|
| solid, SSS | Opaque (default) |
| cut-out (leaves, fences) | Masked, and wire `Material.OpacityMask` |
| colored glass, tinted liquid | `TranslucentColoredTransmittance`, coverage via `SubstrateWeight` |
| grey-tinted transparency, cheaper | Translucent |
| glow only | Additive |

`TranslucentColoredTransmittance` is accepted only when Substrate is on.

## Cost

Each operator that is not parameter-blended adds a closure, which is roughly a whole extra
lighting pass per pixel. For game assets keep one closure: set
`bUseParameterBlending: true` on Horizontal / Vertical / Add operators. The look becomes a
blend of parameters instead of two layers of light, and it usually holds up. Keep true
multi-layer results for hero assets and film. Anisotropy, glints, specular profiles,
Fuzz, SSS diffusion and F90 each push the Slab into a more expensive class. Details and
the out-of-budget behavior: **`references/pitfalls.md`**.

## Failure handling

- **The compile result is in `material_apply_graph`'s reply.** Match the error text
  against the table in **`references/pitfalls.md`**: each line there names its fix.
- **A Substrate node is refused with "not enabled"** means you skipped step 1. Go back
  to it.
- **"is ignored for this material"** when connecting means you wired a classic pin while
  `FrontMaterial` is in use. Move that input onto the Slab (BaseColor maps to
  `Diffuse Albedo`, Roughness to `Roughness`, Normal to `Normal`, Emissive to
  `Emissive Color`, Metallic via `SubstrateMetalnessToDiffuseAlbedoF0`, Opacity into
  a `SubstrateWeight`'s `Weight`).
- **A property is refused** with "no field ...": the reply lists the settable names.
  Use one of those.
- **It compiles but looks wrong.** Check the blend mode against the table above, then
  whether the SSS or coverage rules apply, before changing values.

## Escalation

These are not reachable through the tools. Say so and hand them to the user:

- Creating Subsurface Profile or Specular Profile assets. The tools can point a Slab at
  an existing one through `properties`.
- The editor's one-way "Convert To Substrate" on an existing classic material. Rebuilding
  the graph with Slabs through `material_apply_graph` is the tool-side alternative.
- Material layer stacks (`r.Substrate.EnableLayerSupport`, experimental).
- Per-platform budget cvars such as `r.Substrate.BytesPerPixel` or closures per pixel.
  Changing them affects the whole project's performance, so the user decides.

Trigger boundaries and worked failure replies: **`references/trigger-examples.md`**.
