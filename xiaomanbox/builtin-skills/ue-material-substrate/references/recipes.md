# Substrate recipes

## Contents

- How to read these
- R1 Plain surface (metalness workflow)
- R2 Car paint / lacquer
- R3 Skin, wax, jade (subsurface)
- R4 Velvet, cloth
- R5 Colored glass, tinted liquid
- R6 Soap bubble, oil slick (thin film)
- R7 Two materials by a mask
- R8 Wet, dust or snow coat
- R9 Emissive
- R10 Water surface
- R11 Hair, R12 Eye
- R13 UI, post process, light function, decal

## How to read these

Each block is the `nodes` + `connections` of one `material_apply_graph` call on a fresh
master material. The value next to `blend_mode` is what to set with
`material_set_property` first; if there is none, leave the material Opaque. Swap
constants for `ScalarParameter` / `VectorParameter` (with `node_name`) wherever the user
will want to tune the value in an instance.

Every one of these was compiled in Substrate-enabled 5.8 and 5.4 editors. On 5.4/5.5
the `SubSurfaceType` setting does not exist: for `MSS_Diffusion` write
`"bUseSSSDiffusion": true` instead, and for `MSS_SimpleVolume` (glass) leave the setting
out. The glass still compiles and tints, with less accurate transmission.

## R1 Plain surface (metalness workflow)

The metalness convenience node turns classic BaseColor / Metallic / Specular into the
Slab's physical inputs.

```json
{
  "nodes": [
    { "id": "color", "node_type": "VectorParameter", "node_name": "BaseColor", "value": { "r": 0.8, "g": 0.55, "b": 0.2 } },
    { "id": "metal", "node_type": "ScalarParameter", "node_name": "Metallic", "value": 1 },
    { "id": "rough", "node_type": "ScalarParameter", "node_name": "Roughness", "value": 0.35 },
    { "id": "m2d", "node_type": "SubstrateMetalnessToDiffuseAlbedoF0" },
    { "id": "slab", "node_type": "SubstrateSlabBSDF" }
  ],
  "connections": [
    { "from": "color.RGB", "to": "m2d.BaseColor" },
    { "from": "metal.Out", "to": "m2d.Metallic" },
    { "from": "m2d.DiffuseAlbedo", "to": "slab.Diffuse Albedo" },
    { "from": "m2d.F0", "to": "slab.F0" },
    { "from": "rough.Out", "to": "slab.Roughness" },
    { "from": "slab.Out", "to": "Material.FrontMaterial" }
  ]
}
```

A pure dielectric skips `m2d`: color straight into `Diffuse Albedo`, `F0` left at 0.04.

## R2 Car paint / lacquer

Cheap version, one node:

```json
{
  "nodes": [
    { "id": "paint", "node_type": "Constant3Vector", "value": { "r": 0.6, "g": 0.02, "b": 0.02 } },
    { "id": "base_rough", "node_type": "Constant", "value": 0.4 },
    { "id": "coat_rough", "node_type": "Constant", "value": 0.03 },
    { "id": "cc", "node_type": "SubstrateSimpleClearCoatBSDF" }
  ],
  "connections": [
    { "from": "paint.RGB", "to": "cc.Diffuse Albedo" },
    { "from": "base_rough.Out", "to": "cc.Roughness" },
    { "from": "coat_rough.Out", "to": "cc.Clear Coat Roughness" },
    { "from": "cc.Out", "to": "Material.FrontMaterial" }
  ]
}
```

Physically layered version: a metallic-flake base Slab under a clear Slab. It costs two
closures; set `bUseParameterBlending` on the layer node to bring it back to one.

```json
{
  "nodes": [
    { "id": "flake", "node_type": "Constant3Vector", "value": { "r": 0.5, "g": 0.05, "b": 0.05 } },
    { "id": "base_rough", "node_type": "Constant", "value": 0.35 },
    { "id": "base", "node_type": "SubstrateSlabBSDF" },
    { "id": "clear_rough", "node_type": "Constant", "value": 0.02 },
    { "id": "black", "node_type": "Constant3Vector", "value": { "r": 0, "g": 0, "b": 0 } },
    { "id": "clear", "node_type": "SubstrateSlabBSDF" },
    { "id": "thick", "node_type": "Constant", "value": 0.02 },
    { "id": "layer", "node_type": "SubstrateVerticalLayering" }
  ],
  "connections": [
    { "from": "flake.RGB", "to": "base.F0" },
    { "from": "black.RGB", "to": "base.Diffuse Albedo" },
    { "from": "base_rough.Out", "to": "base.Roughness" },
    { "from": "black.RGB", "to": "clear.Diffuse Albedo" },
    { "from": "clear_rough.Out", "to": "clear.Roughness" },
    { "from": "clear.Out", "to": "layer.Top" },
    { "from": "base.Out", "to": "layer.Bottom" },
    { "from": "thick.Out", "to": "layer.Top Thickness" },
    { "from": "layer.Out", "to": "Material.FrontMaterial" }
  ]
}
```

## R3 Skin, wax, jade (subsurface)

Opaque or Masked only. With a Subsurface Profile asset, add
`"properties": { "SubsurfaceProfile": "/Game/.../SP_Skin" }` to the Slab; without one,
`SSS MFP` (in cm) sets how far light travels inside. The haze node gives skin its soft
second highlight.

```json
{
  "nodes": [
    { "id": "skin", "node_type": "Constant3Vector", "value": { "r": 0.8, "g": 0.55, "b": 0.45 } },
    { "id": "mfp", "node_type": "Constant3Vector", "value": { "r": 1.0, "g": 0.4, "b": 0.25 } },
    { "id": "rough", "node_type": "Constant", "value": 0.45 },
    { "id": "haze", "node_type": "SubstrateHazinessToSecondaryRoughness" },
    { "id": "slab", "node_type": "SubstrateSlabBSDF", "properties": { "SubSurfaceType": "MSS_Diffusion" } }
  ],
  "connections": [
    { "from": "skin.RGB", "to": "slab.Diffuse Albedo" },
    { "from": "mfp.RGB", "to": "slab.SSS MFP" },
    { "from": "rough.Out", "to": "slab.Roughness" },
    { "from": "rough.Out", "to": "haze.BaseRoughness" },
    { "from": "haze.Second Roughness", "to": "slab.Second Roughness" },
    { "from": "haze.Second Roughness Weight", "to": "slab.Second Roughness Weight" },
    { "from": "slab.Out", "to": "Material.FrontMaterial" }
  ]
}
```

## R4 Velvet, cloth

```json
{
  "nodes": [
    { "id": "cloth", "node_type": "Constant3Vector", "value": { "r": 0.25, "g": 0.02, "b": 0.08 } },
    { "id": "sheen", "node_type": "Constant3Vector", "value": { "r": 0.9, "g": 0.4, "b": 0.6 } },
    { "id": "rough", "node_type": "Constant", "value": 0.8 },
    { "id": "fuzz", "node_type": "Constant", "value": 0.9 },
    { "id": "slab", "node_type": "SubstrateSlabBSDF" }
  ],
  "connections": [
    { "from": "cloth.RGB", "to": "slab.Diffuse Albedo" },
    { "from": "rough.Out", "to": "slab.Roughness" },
    { "from": "fuzz.Out", "to": "slab.Fuzz Amount" },
    { "from": "sheen.RGB", "to": "slab.Fuzz Color" },
    { "from": "slab.Out", "to": "Material.FrontMaterial" }
  ]
}
```

## R5 Colored glass, tinted liquid

blend_mode: `TranslucentColoredTransmittance`. The glass color is the color light keeps
after passing through `Thickness` cm of material, and the transmittance node turns it into
the Slab's mean free path. Coverage 1 means all light goes through the glass; lower it for
frosted or dirty glass.

```json
{
  "nodes": [
    { "id": "tint", "node_type": "Constant3Vector", "value": { "r": 0.2, "g": 0.8, "b": 0.4 } },
    { "id": "thick", "node_type": "Constant", "value": 0.5 },
    { "id": "t2mfp", "node_type": "SubstrateTransmittanceToMFP" },
    { "id": "rough", "node_type": "Constant", "value": 0.02 },
    { "id": "black", "node_type": "Constant3Vector", "value": { "r": 0, "g": 0, "b": 0 } },
    { "id": "slab", "node_type": "SubstrateSlabBSDF", "properties": { "SubSurfaceType": "MSS_SimpleVolume" } },
    { "id": "cover", "node_type": "Constant", "value": 1 },
    { "id": "cov", "node_type": "SubstrateWeight" }
  ],
  "connections": [
    { "from": "tint.RGB", "to": "t2mfp.TransmittanceColor" },
    { "from": "thick.Out", "to": "t2mfp.Thickness" },
    { "from": "black.RGB", "to": "slab.Diffuse Albedo" },
    { "from": "t2mfp.MFP", "to": "slab.SSS MFP" },
    { "from": "rough.Out", "to": "slab.Roughness" },
    { "from": "slab.Out", "to": "cov.A" },
    { "from": "cover.Out", "to": "cov.Weight" },
    { "from": "cov.Out", "to": "Material.FrontMaterial" }
  ]
}
```

A cheaper tint with no reflections: `SubstrateUnlitBSDF` with `TransmittanceColor` set
to the tint, same blend mode.

## R6 Soap bubble, oil slick (thin film)

Bubble: blend_mode `TranslucentColoredTransmittance`, low coverage. Oil slick: Opaque,
drop the Weight node and give the Slab a dark `Diffuse Albedo`.

```json
{
  "nodes": [
    { "id": "film_thick", "node_type": "Constant", "value": 0.4 },
    { "id": "ior", "node_type": "Constant", "value": 1.33 },
    { "id": "film", "node_type": "SubstrateThinFilm" },
    { "id": "rough", "node_type": "Constant", "value": 0.05 },
    { "id": "black", "node_type": "Constant3Vector", "value": { "r": 0, "g": 0, "b": 0 } },
    { "id": "slab", "node_type": "SubstrateSlabBSDF" },
    { "id": "cover", "node_type": "Constant", "value": 0.2 },
    { "id": "cov", "node_type": "SubstrateWeight" }
  ],
  "connections": [
    { "from": "film_thick.Out", "to": "film.Thickness" },
    { "from": "ior.Out", "to": "film.IOR" },
    { "from": "film.Specular Color", "to": "slab.F0" },
    { "from": "film.Edge Specular Color", "to": "slab.F90" },
    { "from": "black.RGB", "to": "slab.Diffuse Albedo" },
    { "from": "rough.Out", "to": "slab.Roughness" },
    { "from": "slab.Out", "to": "cov.A" },
    { "from": "cover.Out", "to": "cov.Weight" },
    { "from": "cov.Out", "to": "Material.FrontMaterial" }
  ]
}
```

## R7 Two materials by a mask

`Mix` 0 shows `Background`, 1 shows `Foreground`. Parameter blending keeps the cost at
one closure, at the price of a blend that is less physically exact where the two meet.

```json
{
  "nodes": [
    { "id": "rust_col", "node_type": "Constant3Vector", "value": { "r": 0.35, "g": 0.12, "b": 0.05 } },
    { "id": "rust_rough", "node_type": "Constant", "value": 0.85 },
    { "id": "rust", "node_type": "SubstrateSlabBSDF" },
    { "id": "steel_f0", "node_type": "Constant3Vector", "value": { "r": 0.56, "g": 0.57, "b": 0.58 } },
    { "id": "black", "node_type": "Constant3Vector", "value": { "r": 0, "g": 0, "b": 0 } },
    { "id": "steel_rough", "node_type": "Constant", "value": 0.3 },
    { "id": "steel", "node_type": "SubstrateSlabBSDF" },
    { "id": "mask", "node_type": "Noise" },
    { "id": "mix", "node_type": "SubstrateHorizontalMixing", "properties": { "bUseParameterBlending": true } }
  ],
  "connections": [
    { "from": "rust_col.RGB", "to": "rust.Diffuse Albedo" },
    { "from": "rust_rough.Out", "to": "rust.Roughness" },
    { "from": "black.RGB", "to": "steel.Diffuse Albedo" },
    { "from": "steel_f0.RGB", "to": "steel.F0" },
    { "from": "steel_rough.Out", "to": "steel.Roughness" },
    { "from": "steel.Out", "to": "mix.Background" },
    { "from": "rust.Out", "to": "mix.Foreground" },
    { "from": "mask.Out", "to": "mix.Mix" },
    { "from": "mix.Out", "to": "Material.FrontMaterial" }
  ]
}
```

In a real material the mask is a texture channel, not Noise.

## R8 Wet, dust or snow coat

A thin top Slab faded by its own coverage mask, layered over the surface. Wet coat: dark
clear top (`Diffuse Albedo` 0, low roughness). Dust or snow: light rough top.

```json
{
  "nodes": [
    { "id": "stone_col", "node_type": "Constant3Vector", "value": { "r": 0.4, "g": 0.38, "b": 0.35 } },
    { "id": "stone_rough", "node_type": "Constant", "value": 0.7 },
    { "id": "stone", "node_type": "SubstrateSlabBSDF" },
    { "id": "dust_col", "node_type": "Constant3Vector", "value": { "r": 0.75, "g": 0.72, "b": 0.65 } },
    { "id": "dust_rough", "node_type": "Constant", "value": 0.95 },
    { "id": "dust", "node_type": "SubstrateSlabBSDF" },
    { "id": "dust_mask", "node_type": "Constant", "value": 0.6 },
    { "id": "dust_cov", "node_type": "SubstrateWeight" },
    { "id": "thick", "node_type": "Constant", "value": 0.005 },
    { "id": "layer", "node_type": "SubstrateVerticalLayering", "properties": { "bUseParameterBlending": true } }
  ],
  "connections": [
    { "from": "stone_col.RGB", "to": "stone.Diffuse Albedo" },
    { "from": "stone_rough.Out", "to": "stone.Roughness" },
    { "from": "dust_col.RGB", "to": "dust.Diffuse Albedo" },
    { "from": "dust_rough.Out", "to": "dust.Roughness" },
    { "from": "dust.Out", "to": "dust_cov.A" },
    { "from": "dust_mask.Out", "to": "dust_cov.Weight" },
    { "from": "dust_cov.Out", "to": "layer.Top" },
    { "from": "stone.Out", "to": "layer.Bottom" },
    { "from": "thick.Out", "to": "layer.Top Thickness" },
    { "from": "layer.Out", "to": "Material.FrontMaterial" }
  ]
}
```

## R9 Emissive

Lit surface that also glows: wire `Emissive Color` on the Slab. Pure glow:

```json
{
  "nodes": [
    { "id": "glow", "node_type": "Constant3Vector", "value": { "r": 4, "g": 1.5, "b": 0.3 } },
    { "id": "unlit", "node_type": "SubstrateUnlitBSDF" }
  ],
  "connections": [
    { "from": "glow.RGB", "to": "unlit.EmissiveColor" },
    { "from": "unlit.Out", "to": "Material.FrontMaterial" }
  ]
}
```

## R10 Water surface

Opaque, the water BSDF alone at the root, no operators around it.

```json
{
  "nodes": [
    { "id": "albedo", "node_type": "Constant3Vector", "value": { "r": 0.05, "g": 0.25, "b": 0.3 } },
    { "id": "ext", "node_type": "Constant3Vector", "value": { "r": 0.3, "g": 0.1, "b": 0.08 } },
    { "id": "rough", "node_type": "Constant", "value": 0.05 },
    { "id": "water", "node_type": "SubstrateSingleLayerWaterBSDF" }
  ],
  "connections": [
    { "from": "albedo.RGB", "to": "water.WaterAlbedo" },
    { "from": "ext.RGB", "to": "water.WaterExtinction" },
    { "from": "rough.Out", "to": "water.Roughness" },
    { "from": "water.Out", "to": "Material.FrontMaterial" }
  ]
}
```

## R11 Hair, R12 Eye

Same shape as R10: `SubstrateHairBSDF` or `SubstrateEyeBSDF` alone at the root, pins as
in the node reference. The Eye BSDF also needs its `SubsurfaceProfile` setting.

## R13 UI, post process, light function, decal

The dedicated node goes straight into `FrontMaterial`, and the domain follows. UI example:

```json
{
  "nodes": [
    { "id": "col", "node_type": "Constant3Vector", "value": { "r": 1, "g": 0.8, "b": 0.2 } },
    { "id": "ui", "node_type": "SubstrateUI" }
  ],
  "connections": [
    { "from": "col.RGB", "to": "ui.Color" },
    { "from": "ui.Out", "to": "Material.FrontMaterial" }
  ]
}
```

Decal: build a Slab as usual, wire it into `SubstrateConvertToDecal.DecalMaterial`, the
decal mask into `Coverage`, and the decal node into `FrontMaterial`. Nothing may come
after the decal node.
