# Substrate node reference

## Contents

- Conventions
- BSDFs
- Operators
- Utility nodes (plain numbers out)
- Dedicated output nodes
- Availability by engine version

Pin names below are the ones `material_search_nodes` prints and the ones a connection
must use, spaces included. Defaults are what an unconnected pin compiles to. Settings are
node properties: pass them in `properties`.

## Conventions

- Every BSDF and operator has exactly one output, `Out`, of type Substrate.
- Substrate-typed inputs (operator inputs) accept only another BSDF or operator.
  Number-typed inputs (every BSDF pin) accept only numbers. The pin type in the search
  result tells you which is which.
- Lengths are in **centimeters** (`SSS MFP`, `Top Thickness`, `Thickness`).
- `F0` is the reflectance straight on. For dielectrics use 0.04 (plastic, paint, glass)
  or about 0.02 (water). For metals, F0 is the metal's color and `Diffuse Albedo` is 0.

## BSDFs

### SubstrateSlabBSDF — the workhorse

| pin | type | default | notes |
|---|---|---|---|
| Diffuse Albedo | float3 | 0.18 | base color of a dielectric; 0 for metal |
| F0 | float3 | 0.04 | specular color at normal incidence |
| F90 | float3 | 1 | edge tint; connecting it makes the Slab more expensive |
| Roughness | float | 0.5 | |
| Anisotropy | float | 0 | -1..1; expensive; needs Tangent |
| Normal | float3 | vertex normal | tangent space unless the material says otherwise |
| Tangent | float3 | vertex tangent | only used with Anisotropy |
| SSS MFP | float3 | 0 | mean free path in cm; ignored when a Subsurface Profile is set |
| SSS MFP Scale | float | 1 | still applies with a profile |
| SSS Phase Anisotropy | float | 0 | -1..1 |
| Emissive Color | float3 | 0 | |
| Second Roughness | float | 0 | second specular lobe (skin, haze) |
| Second Roughness Weight | float | 0 | 0..1 mix of the second lobe |
| Fuzz Roughness | float | = Roughness | |
| Fuzz Amount | float | 0 | cloth / velvet sheen |
| Fuzz Color | float3 | 0 | |
| Glint Density | float | 1 (none) | sparkle flakes; shows "(Disabled)" when `r.Substrate.Glints` is off |
| Glint UVs | float2 | 0 | |

Settings:

| property | versions | values |
|---|---|---|
| `SubsurfaceProfile` | all | asset path of a Subsurface Profile |
| `SpecularProfile` | all | asset path of a Specular Profile (expensive) |
| `SubSurfaceType` | 5.6+ | `MSS_None`, `MSS_Wrap`, `MSS_TwoSidedWrap`, `MSS_Diffusion` (default), `MSS_SimpleVolume` (glass-like, light passes through) |
| `bUseSSSDiffusion` | 5.4 / 5.5 | true (default) = diffusion, false = wrap approximation |

A Slab that is not the bottom of a stack can only use `MSS_SimpleVolume`.

### SubstrateSimpleClearCoatBSDF — one-node clear coat, cheaper than two Slabs

`Diffuse Albedo` (0.18), `F0` (0.04), `Roughness` (0.5, base layer),
`Clear Coat Coverage` (1), `Clear Coat Roughness` (0.1), `Normal`, `Emissive Color`,
`Bottom Normal` (5.6+, needs `r.ClearCoatNormal=1`).

`Clear Coat Coverage` is how much of the surface carries the coat (0 none, 1 all of it, a
mask in between for worn paint). It is **not** a thickness. The node has no thickness
input; for a thick coat use two Slabs in `SubstrateVerticalLayering` with `Top Thickness`.

### SubstrateUnlitBSDF

`EmissiveColor` (0), `TransmittanceColor` (1 = background fully visible through it),
`Normal` (only for refraction). No lighting, no specular. Cannot go into `SubstrateSelect`.

### SubstrateSingleLayerWaterBSDF

`BaseColor`, `Metallic`, `Specular` (0.5), `Roughness` (0.5), `Normal`, `EmissiveColor`,
`TopMaterialOpacity`, `WaterAlbedo`, `WaterExtinction` (1/cm), `WaterPhaseG`,
`ColorScaleBehindWater` (1). Must be the root, must be Opaque, never in parameter
blending.

### SubstrateHairBSDF

`BaseColor`, `Scatter`, `Specular` (0.5), `Roughness` (0.5), `Backlit`, `Tangent`,
`EmissiveColor`. Root only.

### SubstrateEyeBSDF

`DiffuseColor`, `Roughness` (0.5), `CorneaNormal`, `IrisNormal`, `IrisPlaneNormal`,
`IrisMask`, `IrisDistance`, `EmissiveColor`. Setting: `SubsurfaceProfile`. Root only.

### SubstrateVolumetricFogCloudBSDF

`Albedo`, `Extinction` (1/m), `EmissiveColor`, `AmbientOcclusion` (1). Makes the material
a Volume-domain, Additive material. Setting `bEmissiveOnly` (5.5+).

### SubstrateToonBSDF (5.8, experimental)

`BaseColor` (0.18), `Metallic`, `Specular` (0.5), `Roughness` (0.5), `Normal`,
`EmissiveColor`, `PatternUVs`, `Anisotropy`, `Tangent`. Setting: `ToonProfile`.

## Operators

| node_type | pins | behaviour | settings |
|---|---|---|---|
| SubstrateHorizontalMixing | `Background`, `Foreground`, `Mix` (0.5) | side by side: Mix 0 = Background, 1 = Foreground | `bUseParameterBlending` |
| SubstrateVerticalLayering | `Top`, `Bottom`, `Top Thickness` (0.01 cm) | Top coated over Bottom; thickness applies to Top | `bUseParameterBlending` |
| SubstrateAdd | `A`, `B` | adds light energy; not physical, avoid | `bUseParameterBlending` |
| SubstrateWeight | `A`, `Weight` (1) | coverage: 1 solid, 0 invisible | — |
| SubstrateSelect (5.6+) | `A`, `B`, `SelectValue` (0) | picks B when SelectValue > Threshold, else A; always parameter-blended | `Threshold` (0.5) |

Both inputs of Horizontal / Vertical / Add must be connected; a missing one is a compile
error.

## Utility nodes (plain numbers out)

These output ordinary values that feed Slab pins.

| node_type | inputs (default) | outputs |
|---|---|---|
| SubstrateMetalnessToDiffuseAlbedoF0 | `BaseColor` (0.18), `Metallic` (0), `Specular` (0.5) | `DiffuseAlbedo`, `F0` |
| SubstrateTransmittanceToMFP | `TransmittanceColor` (0.5), `Thickness` (0.01 cm) | `MFP`, `Thickness` |
| SubstrateHazinessToSecondaryRoughness | `BaseRoughness` (0.1), `Haziness` (0.5) | `Second Roughness`, `Second Roughness Weight` |
| SubstrateThinFilm | `Normal`, `F0` (0.04), `F90` (1), `Thickness` (1 = 10 µm, 0 = off), `IOR` (1.44) | `Specular Color`, `Edge Specular Color` |

The Slab has no thin-film pins of its own. Thin film is ThinFilm's two outputs wired into
the Slab's `F0` and `F90`.

## Dedicated output nodes

Each goes straight into `FrontMaterial` and sets the material domain.

| node_type | pins | domain |
|---|---|---|
| SubstrateUI | `Color`, `Opacity` (1) | UI |
| SubstratePostProcess | `Color`, `Opacity` (1, used when the material outputs alpha) | Post Process |
| SubstrateLightFunction | `Color` | Light Function (forced Opaque) |
| SubstrateConvertToDecal | `DecalMaterial` (a Slab, required), `Coverage` (1) | Deferred Decal; must be the root |

## Availability by engine version

- 5.4 to 5.8: every node above except the two below.
- `SubstrateSelect`: 5.6 and later.
- `SubstrateToonBSDF`: 5.8 only.
- `SubstrateSimpleClearCoatBSDF.Bottom Normal`: 5.6 and later.
- `SubSurfaceType` replaces `bUseSSSDiffusion` from 5.6.

Pins were not renamed between 5.4 and 5.8; later versions only added the ones listed.
