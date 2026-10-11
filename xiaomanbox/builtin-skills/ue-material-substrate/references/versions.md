# Substrate by engine version

## Contents

- Status and default per version
- Turning it on
- What happens to classic materials
- Cvars the user may ask about

## Status and default per version

| version | status | on in new projects | classic pins while Substrate is on |
|---|---|---|---|
| 5.0 – 5.3 | named Strata, experimental | no | not supported by these tools |
| 5.4 | experimental / beta | no | **unsupported**: build through `FrontMaterial` |
| 5.5 | beta | no | converted when the material is edited; build through `FrontMaterial` anyway |
| 5.6 | beta | no | keep working (hidden conversion) until `FrontMaterial` is wired |
| 5.7 | production | yes, unless the template turns it off | same as 5.6 |
| 5.8 | production; adds Toon BSDF (experimental) | yes, unless the template turns it off | same as 5.6 |

Projects upgraded from an older engine keep whatever they had; upgrading never turns
Substrate on by itself.

## Turning it on

The switch is `r.Substrate=True` under `[/Script/Engine.RendererSettings]` in the
project's `Config/DefaultEngine.ini` (Project Settings → Rendering → Substrate). It is
read only at startup.

With the user's go-ahead:

1. `ue_set_config` — config `Engine`, section `/Script/Engine.RendererSettings`,
   key `r.Substrate`, value `True`.
2. `ue_restart_editor`.
3. After the restart, `material_search_nodes` with query "Substrate" should list nodes
   with no warning. If the warning is still there, the value did not land. Read it back
   with `ue_get_config` before trying anything else.

Tell the user before step 1: every shader in the project recompiles on the next start,
which can take a long time on a big project, and classic materials start going through
the conversion described below.

Turning it off again is the same key set to `False`. Materials built with Substrate nodes
then render with their classic pins, which are usually empty, so they look black or
default.

## What happens to classic materials

- **5.4 / 5.5**: when a classic material is loaded (5.5: also when it is edited), the
  engine inserts a `Substrate Shading Models` conversion node, moves the classic wires
  onto it and connects it to `FrontMaterial`. Saving the material makes that permanent.
- **5.6 and later**: the asset is left alone. The compiler converts in the background, and
  the output node shows both the classic pins and `FrontMaterial`. Wiring `FrontMaterial`
  switches the material to pure Substrate, and from then on the classic pins are ignored
  (the tools refuse new wires there and flag old ones).
- The editor's "Convert To Substrate" on the output node (5.6+) is a one-way explicit
  conversion. The tools cannot trigger it; rebuilding with Slabs is the tool-side route.

## Cvars the user may ask about

Project-wide performance settings. Explain them; change them only when the user asks.

| cvar | meaning |
|---|---|
| `r.Substrate.ProjectGBufferFormat` (5.7+) | 0 Blendable (fast, one closure, some features off), 1 Adaptive (full features) |
| `r.Substrate.BytesPerPixel` | per-pixel material data budget (default 80; Blendable fixes it at 20) |
| `r.Substrate.ClosuresPerPixel` / `r.Substrate.ProjectClosuresPerPixel` (5.7+) | how many layers of light a pixel may carry |
| `r.Substrate.Glints` | glint flakes; default off from 5.7, on in the Windows config |
| `r.Substrate.OpaqueMaterialRoughRefraction` | rough refraction between stacked layers, experimental |
| `r.Substrate.ShadingQuality` | meaning flipped in 5.7: now 0 approximate, 1 accurate |
