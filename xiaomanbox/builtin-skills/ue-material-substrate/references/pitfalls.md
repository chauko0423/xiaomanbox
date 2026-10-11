# Substrate errors, traps and cost

## Contents

- Refused by the tools before anything changes
- Compile errors and their fixes
- Compiles, but looks wrong
- Cost and budget

## Refused by the tools before anything changes

| reply says | meaning | do |
|---|---|---|
| "Substrate is not enabled in this project" | the renderer is classic | ask the user, then `ue_set_config` + `ue_restart_editor` (SKILL.md step 1) |
| "Substrate materials need Unreal Engine 5.4 or later" | old engine | build with classic pins; say what the look loses |
| "the Material output's X pin is ignored for this material" | `FrontMaterial` is in use, X is a classic pin | move X onto the Slab: BaseColor → `Diffuse Albedo`, Roughness → `Roughness`, Normal → `Normal`, Emissive → `Emissive Color`, Metallic via `SubstrateMetalnessToDiffuseAlbedoF0`. For Opacity: wrap the BSDF in `SubstrateWeight` and feed the opacity into `Weight` |
| "FrontMaterial takes Substrate data, but ... outputs float3" | a color went straight to the output | put a BSDF in between |
| "property 'X': no field 'X' ... Did you mean" | wrong setting name; the node was **not** created | use a name from the list in the reply |
| "'X' is an input pin, not a setting" | a pin was passed in `properties` | connect a value to it instead |

## Compile errors and their fixes

Error text as the engine prints it (`%s` = a node or asset name).

| error contains | cause | fix |
|---|---|---|
| `Missing Foreground input` / `Missing Background input` / `Missing Top input` / `Missing Base input` / `Missing A input` / `Missing B input` | an operator input is empty | connect both inputs; for Vertical the bottom pin is `Bottom` |
| `Could not find any Substrate operators or BSDFs` | nothing Substrate reaches `FrontMaterial` | the chain into `FrontMaterial` must end in a BSDF or operator |
| `does not support generating/processing/flowing Substrate data` | a math node (Lerp, Multiply, a constant...) sits on the Substrate path | blend BSDFs with Substrate operators, never with math nodes |
| `Cannot force a cast between non-numeric types` | Substrate data and numbers were mixed on one wire | usually appears with the line above; fix that one |
| `have too many Substrate Operators` | more than 15 operators + BSDFs | enable `bUseParameterBlending` on operators, or merge Slabs |
| `more unique normal/tangent basis than the allowed limit` | more than 4 distinct normal/tangent sources | feed one normal node to all Slabs |
| `not possible to uses a Substrate BSDF ... multiple times` | one BSDF's `Out` wired into two places | duplicate the BSDF node |
| `Cyclic graph detected` | a loop in the graph | break it |
| `Material has no BSDF ... material attributes are enabled` | the material uses MaterialAttributes | turn MaterialAttributes off on this material, or rebuild without it |
| `SingleLayerWater BSDF node cannot be used with parameter blending` / `cannot be the root of a parameter blending sub tree` | water BSDF inside an operator | water BSDF alone into `FrontMaterial` |
| `Convert To Decal node must be the root` | an operator after the decal node | decal node straight into `FrontMaterial` |
| `Select node requires both inputs to have a valid local basis` (5.6+) | an Unlit BSDF inside Select | use Slabs in Select |
| `Operator %s cannot be used with the following data flowing through` (5.8) | Hair / Eye / Water / Unlit into Horizontal, Vertical, Add or Decal | only Slabs go into those operators |
| `Static Switch nodes processing Substrate data do not support dynamic branching` (5.8) | a switch on Substrate data driven by a runtime value | drive it with a static parameter or constant |
| `could not be simplified to fit in Substrate per pixel` | over budget even after automatic simplification | fewer closures (parameter blending), fewer expensive features |

## Compiles, but looks wrong

- **Glass is opaque or grey.** The blend mode must be `TranslucentColoredTransmittance`
  (plain Translucent only transmits grey). The Slab needs `SubSurfaceType=MSS_SimpleVolume`
  (5.6+) and a mean free path from `SubstrateTransmittanceToMFP`. Opacity is the
  `SubstrateWeight` coverage, not the output's Opacity pin.
- **Subsurface does nothing and the blend mode went back to Opaque.** The engine forces
  Opaque when a material has SSS data and a translucent blend mode. SSS and transparency
  do not combine; use a Simple Volume Slab for translucent media.
- **The layered look collapses into an average.** `bUseParameterBlending` is on. That is
  expected: it trades layered light for cost. Turn it off only if the user accepts the
  extra closure.
- **Glints show "(Disabled)" on the pin.** `r.Substrate.Glints` is off (the default from
  5.7, turned on by the Windows platform config). It is a project setting and needs a
  shader recompile, so leave it to the user.
- **A UI or post-process material renders as a lit surface.** A Slab is at the root. Use
  `SubstrateUI` / `SubstratePostProcess`.
- **Edits to BaseColor etc. do nothing.** Look at `material_get_graph`: wires marked as not
  taking effect under Substrate are exactly these. Move them onto the Slab.

## Cost and budget

- **Closure** ≈ one full lighting evaluation per pixel. Each Horizontal / Vertical / Add
  without parameter blending adds one. From 5.7 the default per-pixel limit is 4 on
  Windows desktop and 1 on lower platforms; in Blendable GBuffer mode it is always 1.
- **Slab class** rises with features: Simple (albedo, F0, roughness) → Single (F90, Fuzz,
  SSS, second roughness, MFP) → Complex (anisotropy, specular profile, several Slabs,
  hair, eye, toon) → Complex Special (glints). Each step is a more expensive shading path.
- **Over budget** is not an error at first: the engine simplifies the material
  automatically and the material editor's Substrate panel says it did. It only fails when
  it still cannot fit (the last row of the compile table above).
- Game assets: aim for Simple/Single with one closure. Hero assets, automotive and film
  can afford two to four.
- Inspecting cost is an editor task: the viewport's Substrate view modes (Material
  Classification, Material Count, Bytes Count) and the Substrate tab in the material
  editor. Point the user there when they ask why a material is expensive.
