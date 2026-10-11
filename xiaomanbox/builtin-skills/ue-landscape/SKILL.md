---
name: ue-landscape
description_zh: 创建 Unreal 地形（平坦地形或从高度图文件生成），配置地形运行时虚拟纹理（颜色 RVT 与高度 RVT），并诊断 RVT 不显示的原因。当用户要建地形、用高度图生成地形、给地形配 RVT，说「建一块地形」「RVT 是黑的」「草和地面融合没效果」「地形边缘发黑」时使用；在地形上撒树石（PCG）、雕刻/绘制地形层、编辑地形材质图不适用于它。
description: Creates Unreal landscapes (flat or from a heightmap file) and sets up landscape runtime virtual textures - colour RVT and height RVT - and diagnoses why an RVT shows nothing. Use when the user wants a terrain created, a heightmap imported as a landscape, RVT added to a landscape, or says things like "建一块地形"、"用这张高度图生成地形"、"给地形配 RVT"、"RVT 是黑的"、"草和地面融合没效果"、"地形边缘发黑". Do not use for scattering trees or rocks over a landscape (that is PCG), for sculpting or painting landscape layers, or for editing the landscape material graph itself.
---

# Landscape and landscape RVT

Three tools:

```
landscape_list        what landscapes exist, real size, and the RVT five-point check
landscape_create      new landscape: flat (size in meters) or from a heightmap file
landscape_setup_rvt   colour and/or height RVT on an existing landscape
```

Trigger and non-trigger examples: `references/trigger-examples.md`.

## Quick start

"建一块 1 公里的地形，材质用 M_Ground，配上 RVT":

1. `landscape_list` — is there already a landscape? Is the level World Partition?
2. `landscape_create` with `size_x_m: 1000`, `material`, `rvt: { color: true, height: true }`.
3. Read the receipt. Size, resolution and bounds in it are read back from the engine.
4. If the receipt lists unmet RVT items, tell the user which ones and why (see below).

## Sizes are rounded, say so

A landscape can only be built from the fixed combinations in the editor's New Landscape panel.
`size_x_m: 1000` becomes 1008 m. The receipt says what was asked and what was built — pass
both numbers on to the user. Never repeat the requested size as the result.

`height_range_m` is the drop between pure black and pure white in the heightmap (default
512 m, mid grey = 0). It is not the terrain's actual height. Leave it out for flat terrain.

A heightmap whose size is not a legal landscape size is resampled to the nearest legal one;
the receipt says so. `.raw` / `.r16` files have no header: if the receipt says the engine
guessed between several sizes, prefer asking the user for a 16-bit PNG.

## RVT only works when five things line up

Missing any one gives no error — just black patches or no blending.

| # | What | Who fixes it |
|---|---|---|
| 1 | Project setting: Enable virtual texture support | **The user.** Needs an editor restart and affects the whole project. Explain; do not change it silently |
| 2 | An RVT asset of the right type | `landscape_setup_rvt` |
| 3 | The landscape and every streaming proxy draw into it | `landscape_setup_rvt` |
| 4 | An RVT volume covering the whole landscape (height RVT: full height too) | `landscape_setup_rvt` |
| 5 | The landscape material has a Runtime Virtual Texture Output node, with BaseColor (colour) or WorldHeight (height) connected | Material tools — load `ue-material-authoring` |

`landscape_setup_rvt` does 2–4 and checks 1 and 5. A receipt that says the setup was written
but items are still unmet means **RVT does not work yet**. Do not report it as done.

For "RVT 是黑的" / "融合没效果": call `landscape_list` first. It names the unmet item.
Item 5 check searches the material graph and material functions but not Material Layers;
if the material uses layer stacks, say the check could not see inside them.

## Constraints

- Both write tools refuse while PIE is running, and both are undoable with Ctrl+Z.
- Up to 8,160 quads per side. Bigger terrain: raise `quad_size_m` or split it.
- No sculpting and no layer painting. To reshape terrain, rebuild it from a new heightmap.
- New RVT assets and landscapes are left unsaved, like every other create tool. Save when the
  user asks.

## Failure handling

- `There is no landscape in the current level` from `landscape_setup_rvt`: create one first,
  or check with `ue_get_current_level` that the right level is open.
- Heightmap rejected: the message names the supported formats. 8-bit PNGs import with a
  warning and visible terracing — tell the user a 16-bit source gives smooth slopes.
- Volume does not cover the landscape on UE 5.0–5.4 in a World Partition level: the engine
  only measured loaded cells. Ask the user to load the whole landscape (World Partition
  window → select all → Load) and run `landscape_setup_rvt` again.

## Escalation

Anything that needs a hand-authored landscape material (layer blends, auto-slope materials)
belongs to `ue-material-authoring`. Scattering on the terrain belongs to `ue-pcg-scattering`.
