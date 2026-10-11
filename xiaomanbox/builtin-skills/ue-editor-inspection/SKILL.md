---
name: ue-editor-inspection
description_zh: 查看视口画面，读取工程设置和 .uproject 元数据，检查世界由哪些关卡组成，以及修改引擎配置值。当用户需要看场景、确认某项工程设置、查项目用了哪些插件和模块、或排查游戏画面和编辑器里不一样的原因时使用；编辑资产或 Actor 不适用于。
description: Look at the viewport, read project settings and .uproject metadata, check which levels make up the world, and change engine config values. Use when you need to see the scene, confirm a project setting, check which plugins and modules the project uses, or work out why the game looks different from the editor. Do not use for editing assets or actors.
---

# Editor inspection

Verified against UE 5.5.4 with UnrealAgentLink.

## When the user says "this", look before you ask

`ue_get_selection` reports what the user currently has open and selected. Call it whenever a
request points at something without naming it — "change **this** node", "**this** actor is too
tall", "rename **these**". It is cheaper and more accurate than asking "which one?".

What it gives you:

- `focusedEditor` — the asset editor actually in front (by last activation time), not just any
  open one.
- `focusedGraph` — the graph a Blueprint editor is currently showing.
- `selectedNodes` — nodes selected in that graph, each with a `node_id` you can pass straight to
  `ue_bp_*` / `ue_material_*`. No need to fetch the graph first to look the id up.
- `selectedActors` — actors selected in the viewport or outliner. `label` is what the user sees
  and says; `name` is the internal one.
- `contentBrowser` — assets and folders selected in the Content Browser.

Lists cap at 50 entries each. When `*_truncated` is true, the accompanying count is the real
total — do not act as if the visible entries are all of them.

**What it cannot see:** anything that is merely under the cursor — a button, a menu item, a row in
the Details panel. It reads selection and focus, not the widget beneath the pointer. If the user
asks "what does this button do", ask them for a screenshot.

Nothing selected is an answer, not a gap. Do not fall back to picking a plausible asset yourself —
say what you found and ask.

## Screenshot returns the picture — look at it

`ue_screenshot` captures the viewport and hands the image back to you directly. Use it whenever a
change has a visible result: lighting, materials, actor placement, level layout.

A tool reporting `success` proves the call went through, not that the scene looks right. Capture,
look, and describe what you actually see. If it does not match what the user asked for, say so
before moving on.

`show_ui: false` (the default) renders the scene directly, so it works even when the editor
window is minimised, covered or unfocused. The frame contains the scene and nothing else — no
outliner, no details panel, no dialogs.

`show_ui: true` grabs the whole editor window instead: panels, menus, and any modal dialog on
top. That is the one to use when the user mentions something on screen — an error popup, a
value in a panel, where a button is. It is a screen grab, not a render, so `resolution`,
`world` and `warmup_frames` do nothing on that path, and there is no exposure drift.

Which window it grabs: a modal dialog if one is open (even when you named a window — the
dialog is blocking it), otherwise the window the user last worked in — a floating Blueprint
or Material editor counts. The reply says which one you got in `window_title`; read it before
drawing conclusions, because a shot of the wrong window looks perfectly normal. To pick one
explicitly pass `window` with the asset name or path (`BP_Door`, `/Game/BP/BP_Door`), or
`level` for the main level window; `window` only works together with `show_ui: true`.
`window_source: fallback` means it fell back to the main window — do not treat that as what
the user is looking at.

### While PIE is running it captures the game

The editor world and the running game are two separate worlds. When PIE is running the capture
follows the game — the play world, from the player's viewpoint — because that is what the user is
looking at. The reply says which one you got: `world` is `pie` or `editor`, `view` is `player` or
`viewport`. Read those before drawing a runtime conclusion from the image.

Pass `world: "editor"` to override it — only useful when PIE is running and you specifically want
to check the editor scene itself, e.g. actor placement in the level.

### Driving the running game

Seeing the game is not the same as playing it. Two tools cover that, and both only make sense
once `ue_playtest` has something running:

- `ue_input_map` — what this game can even respond to: the input actions and the keys bound to
  them. Read it first; guessing a key that is not bound looks exactly like a broken feature.
- `ue_inject_input` — simulate one input in the **running** game. Fails when nothing is running,
  which is the usual reason a "the button does nothing" report turns out to be "the game was
  not playing".

The loop for "does this interaction actually work" is: `ue_playtest` to start, `ue_input_map` to
learn the bindings, `ue_inject_input` to perform it, `ue_screenshot` to see the result.

When the check takes more than one input — walk somewhere, then press something, then wait for
a result — `ue_autoplay` (experimental) plays it for you in its own play session: give it a
goal (`reach_actor` / `press` / `until_log`) or a one-sentence `objective`. Put a PrintString
in the logic under test and pass it as `until_log`; that line appearing is the only proof, the
bot saying it is done is not. It is reliable on flat ground and navmesh-reachable targets and
often fails where the route needs jumping up steps or crossing gaps, so a "did not reach it"
is more likely terrain than a bug. It moves the character directly, bypassing key bindings, so
it never proves the player's keys work — that is still `ue_inject_input`.

### One frame cannot show you a run

A single end-of-run screenshot contains no time. Whether the character moved, when the door
opened, at what second the screen went black, whether something spawned and then vanished —
none of that information exists in one image, and the image still looks plausible enough to
support a confident wrong answer.

`ue_playtest` with `frames: 9` samples the run and returns **one** stitched timeline image,
each cell labelled with its index and second, left to right and top to bottom. Use `frame_times`
to pin the samples around an event instead of spreading them evenly. Each cell is only a few
hundred pixels wide: enough for where things are, whether they move, whether the screen goes
dark; not enough for material detail, small props or any text. Say so when you cannot see it.

### Brightness is only as trustworthy as the exposure lock

Every capture reports which exposure the frame used (`exposure`, and a line in `message`).
Read it before saying anything about brightness.

- **Manual exposure** (an unbound PostProcessVolume with Metering Mode = Manual): the capture,
  the viewport and the running game all land on the same brightness. What the image shows is
  what the player sees — judge brightness, over- and under-exposure from it.
- **Locked by the editor viewport only** (`exposure_source: viewport`): true inside the editor,
  but PIE and packaged builds still follow the project and post-process settings.
- **Auto exposure**: each view converges on its own brightness, and a darkened scene gets pulled
  back up. The image has no brightness baseline. To judge or tune lighting, lock exposure first,
  then adjust Exposure Compensation.

**Never close out a lighting problem as "screenshot bias".** That was the old advice here and it
became the excuse: on 2026-09-26 a level artist tuned lights for four rounds, every frame came back
blue and washed out, and it stopped by blaming the capture — the user's viewport was just as bright.
Unlocked: lock, then tune. Locked: the scene is the problem.

Position, material colour, whether a light is on, shadow direction and composition are reliable
whatever the exposure. For numbers instead of impressions — clipping, region brightness, palette —
pass `measure: true` (see "Measure, don't eyeball" below).

### Measure, don't eyeball

Judging "is the lighting right, does it look good" from a picture is the weakest thing you do,
and grading your own scene pulls you toward "looks great already". When the question is about
lighting, post-processing or overall look, pass `measure: true`. The reply adds a **测光** block
computed locally from the saved PNG (the tonemapped image the player sees), plus a second image:

- Brightness mean and 5 / 50 / 95 percentiles, and the 5–95 spread.
- Share of clipped-white and crushed-black pixels.
- Top, middle and bottom thirds of the frame: brightness, chroma, warm/cool (R − B) and hue.
- The five most common colours with their share.
- A value study: greyscale on the left, three fixed tones (dark / mid / light) on the right.

How to use it:

- **⚠️ lines are defects in any style**: blown highlights, crushed blacks, a spread too narrow to
  have any depth. Fix them.
- **The thirds are geometry, not detected sky and ground.** In an eye-level outdoor shot the top
  is usually distance, which normally reads lighter, less saturated and cooler than the
  foreground. Interiors, top-down shots and deliberate styles can break this. Compare against
  the reference, not against a rule.
- **Read the three-tone half of the value study.** If the subject and the background fall into the
  same tone, they will not separate in the shot whatever the colours do. A frame that is all mid
  tone has no light structure.
- **Compare, don't grade.** Measure from the same camera before and after a change, and report the
  difference. A pair of numbers moving the right way beats an adjective.
- Without manual exposure the brightness numbers describe only this frame (the reply says so).
  Lock exposure first.

## Point the camera before you shoot

The viewport shows whatever the editor camera happens to be pointed at. An empty or black frame
almost always means the camera is not looking at the thing you changed — it does **not** mean the
change failed. Never report failure on the strength of an empty frame.

`ue_focus_viewport` fixes that: give it `names` (the labels shown in the World Outliner, not class
names or asset paths) and the camera frames those actors, exactly as if a person selected them and
pressed F. Then screenshot.

```
ue_focus_viewport  names: ["BP_Chair"]
ue_screenshot
```

### To see part of a thing, use `region`

`names` alone frames the **whole** actor. Ask for a tree's trunk that way and you get the whole
tree filling the frame with the trunk a few pixels tall.

```
ue_focus_viewport  names: ["SM_OakTree2"], region: "bottom"
ue_screenshot
```

`region` splits the bounding box into vertical thirds: trunk / pillar / plinth → `bottom`;
canopy / roof → `top`; body of a crate → `middle`.

When `region` is anything but `whole`, the camera also levels out to a **horizontal** view instead
of keeping the viewport's existing downward tilt — looking at part of an object is something you
do by walking up to it, not by hovering above it.

Do **not** fall back to computing camera coordinates and setting them from Python. That path has a
trap: `unreal.Rotator`'s positional arguments are `(roll, pitch, yaw)`, so passing
`(pitch, yaw, roll)` silently drops your yaw into the pitch slot and aims the camera at the ground.
Nothing errors; every step reports success; you only find out by looking at the picture. If you
genuinely need a custom angle, pass `direction` and `distance` to this tool instead. If you must
script the camera, write `unreal.Rotator(roll=…, pitch=…, yaw=…)` with keywords —
`ue_run_python_script` refuses the positional form. Whenever a tool of yours moved the viewport,
the next `ue_screenshot` of it says so ("the viewport was last moved 12 s ago by
`ue_run_python_script`"): a sky-only frame after that line means your camera call, not a missing
scene.

`direction` is `current` (default), `horizontal`, or `top`. There is deliberately no
`front`/`left`/`right` — the engine does not know which face of a prop is its front, so those would
be guesses that are wrong about half the time.

### Check these before you screenshot

- `aim.on_target: false` — the camera is not actually pointing at the target (`aim.error_degrees`
  says how far off). Fix the framing before spending a screenshot.
- `occluded_by` — something is between the camera and the target. `occluded_by_self: true` means
  the target's own geometry is in the way, which is exactly the canopy-hiding-the-trunk case.
  **A clear result does not prove visibility**: foliage and decorative meshes often have no
  collision at all, so the trace passes straight through them.
- `bounds` — the target's `center` / `extent` / `min` / `max` / `height`, returned with every
  focus. Use it to plan a shot or to reason about shape ("this canopy is wide and low, so the
  trunk will be hidden from above"). You do not need a separate query for it.
- `moved: false` — the camera did not go anywhere.
- `transition_settled: false` — the camera is still flying; a screenshot now catches it mid-flight.
- `hidden_in_editor: [...]` — aimed correctly, but those actors are hidden, so the frame will be
  empty. Do not go hunting for a bug that is not there.
- `distance` far larger than the object warrants — its bounding box was inflated by some huge
  invisible component (a trigger volume, a landscape). The subject may be a speck in the frame.

One limit worth knowing: **it changes what the user is looking at**, and it replaces their
selection (selecting is how focusing works). Use it to verify a visual result or to show someone
something — not reflexively.

## Reading one component, not the first match

`ue_get_actor(properties)` takes top-level names and reads the first component that has the
property. When an actor has two meshes — an old hidden driver and a new visible one — that is
the wrong one half the time. `ue_inspect_components` reads per component: the material each
slot actually renders with, the overlay material, Custom Depth and stencil (outlines),
visibility, mesh and anim blueprint, plus any dotted property path
(`BodyInstance.CollisionProfileName`). With `blueprint_path` it reads the class defaults and
component templates instead (`object_properties` for CDO fields such as montage mappings).
It is read-only, so read-only sub-tasks get it too — no Python needed.

## "It looks right in the editor but wrong when I play" — check the levels first

A world is usually more than one level. Sublevels have **two independent switches**: visible in
the editor, and loaded in the game. The defaults are not the same, so a sublevel that shows up
perfectly in the viewport and never loads at runtime is an ordinary, unremarkable state — not a
bug anywhere in the content.

Backdrops, floors, lighting rigs and post-process volumes are routinely packaged as one sublevel.
When that level does not load, the game is black or flat while the log stays clean and every tool
reports success.

`ue_get_levels` answers this in one call. Read `editor_only_levels` first — those are visible in
the editor and not visible in the game. `mismatch_reason` says which of the two problems it is,
and they need different fixes: `not_loaded` means the game never loads that level
(`always_loaded: true`), while `loaded_but_hidden` means it loads and ticks but does not render
(`should_be_visible: true`). Reaching for `always_loaded` on the second one does nothing — that
switch does not touch the visibility flag.

**Go here before you inspect materials, `bHidden`, or construction scripts.** Those three are the
usual first guesses and they are usually wrong for this symptom; a real 2026-09-07 session spent
most of its time eliminating all three before the answer turned out to be a sublevel that was
never loaded at runtime.

Apply the fix with `ue_set_level_streaming`. Three things to say out loud when you do:

- changing the streaming method is **not undoable** with Ctrl+Z — to revert, call the tool again
  with the original value and then confirm with `ue_get_levels` that it actually went back;
- if the level is not loaded, pass `visible_in_editor: true` in the same call — that loads it first;
- streaming settings live in the persistent level, so `ue_save_level` afterwards or it is lost.

The reply's `changed` lists only fields that actually moved. An empty `changed` means nothing was
applied — do not report success on it.

`is_world_partition: true` with zero sublevels is normal. World Partition splits content by a
spatial grid instead, and what loads at runtime depends on distance and data layers — the
sublevel flags above do not apply. During play it also generates runtime cells; those are counted
separately as `world_partition_runtime_cells` and deliberately kept out of the mismatch lists,
because they are transient objects that do not exist in the editor world.

`ue_playtest` also returns `levels` for the world it actually ran, so a single playtest tells you
whether anything failed to make it into the game.

## Project information

- `ue_get_project_info` — name, paths, engine version, current level, Lumen/Nanite state,
  modules, enabled plugins and target platforms. Cheap; call it when you need the project's
  shape rather than guessing. It answers "what does this project use".
- Pass `include_disabled_plugins: true` when taking a plugin inventory — the default only
  lists what is enabled.

## Reading and writing config

`config_name` is one of `Engine`, `Game`, `Editor`, `EditorPerProjectUserSettings` — **not** a
filename. Passing `DefaultEngine` is rejected with the valid list, so read the error and retry.

```
config_name: "Engine"
section:     "/Script/Engine.RendererSettings"
key:         "r.Nanite.ProjectEnabled"
```

An empty value means the key is not written in that ini — which usually means the engine default
applies, **not** that the feature is off. Do not report "Nanite is disabled" on the strength of an
empty string; confirm with `ue_get_project_info` or a console command.

`ue_set_config` writes project-wide settings and takes values as strings (`"True"`, `"1"`). Most
renderer settings need an editor restart to take effect — tell the user, and prefer proposing the
change over making it unasked.
