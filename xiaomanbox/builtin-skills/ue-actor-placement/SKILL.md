---
name: ue-actor-placement
description_zh: 在打开的关卡里摆放、查询、移动、改参数和删除 Actor，可单个操作也可按选择器批量执行。当用户想把东西放进场景或修改场景里的东西时使用；编辑蓝图资产、导入网格本身不适用于它。
description: Place, query, move, re-parameterise and delete actors in the open level — one at a time or in bulk by selector. Use when the user wants things put into or changed in the scene. Do not use for editing Blueprint assets, or for importing the meshes themselves.
---

# Actor placement and scene edits

Verified against UE 5.5.4 with UnrealAgentLink: spawn → query → inspect → set property →
bulk transform → delete, checked by reading the scene back rather than trusting the return value.

## Units: every length is centimetres

**1 metre = 100.** A 13 m building is `1300`. Two props 20 m apart differ by `2000`.
This applies to `location`, to `bounds`, and to anything else that is a length.
`scale` is a multiplier and `rotation` is degrees — neither converts.

Any number that came from Blender, a reference image, a floor plan, or your own reasoning
about how big something should be is **in metres, and must be multiplied by 100** before it
goes into a tool. Nothing enforces this. Skip the step and every tool still returns
`success` — the scene is just 100× too small, piled up near the origin.

This has actually happened: 23 assets, 72 actors, a whole reference scene laid out in metres
and fed straight in. Several rounds of "verification" passed because the check being run was
*"does what I read back match what I wrote?"* It did. **Self-consistency cannot detect a unit
error** — both sides are wrong in exactly the same way. Only an external reference catches it.

So use one:

1. **Before laying anything out**, spawn a `cube` preset at the origin with default scale.
   It is exactly 100 cm on each side. Read it back with `return_bounds: true` and confirm the
   bounds come back as `100 × 100 × 100`. That pins the unit for the rest of the session.
2. **After a bulk placement**, read the message tail. `ue_spawn_actor`, `ue_set_transform` and
   `ue_get_actor` all report the placement span **converted to metres**. Compare it against the
   scene you meant to build. A city block that spans 0.4 m is not a rounding error.
3. If the span is two orders of magnitude off, it is metres-vs-centimetres. Fix it in one call —
   `{ filter: {} }` with `operation: { multiply: { location: { x: 100, y: 100, z: 100 } } }` —
   rather than nudging actors one at a time.

## When the user says it looks wrong

Believe them, and suspect the coordinate system first. The user is looking at the viewport;
you are looking at numbers that are internally consistent. When those two disagree, the numbers
are the weaker evidence — they cannot distinguish a correct layout from one that is uniformly
100× off.

Do not answer a visual report with a data argument. "The transforms read back correctly" is
not a rebuttal to "everything is tiny and stacked up" — both can be true at once, and when they
are, the unit convention is the thing that is wrong. Check the span in metres, or ask the user
for a screenshot, before offering any other theory.

Do not rely on `ue_screenshot` alone to settle this either. Check its `camera_source` field
first: `fallback` means it could not find the editor viewport and used an arbitrary camera, so
the framing proves nothing about what is or is not in the scene. Aim with `ue_focus_viewport`
and shoot again, or ask the user to send their own screenshot.

## The selector is the whole game

Every tool except `ue_spawn_actor` takes a `targets` selector. Four forms:

```
{ selection: true }                        whatever the user has selected right now
{ names:  ["MyCube", "MySphere"] }        exact actor labels
{ paths:  ["/Game/.../PersistentLevel.X"] }
{ filter: { class: "PointLight" } }        also: name_pattern, exclude_classes
```

**`selection: true` is the answer to every demonstrative.** "这个"、"我选中的那个"、
"this one" — the user is pointing at the viewport or the World Outliner (they are the same
selection set). Do not write Python against `EditorActorSubsystem` to read it, and do not try
to work out which actor they meant from context: pass `selection: true` and the engine tells
you. If nothing is selected you get `count: 0` with a message saying so — ask the user to
select something rather than falling back to a guess.

It combines with the other forms; `{ selection: true, names: ["Floor"] }` is the union.

**Match on the label, not the internal object name.** The label is what the World Outliner
shows and what every tool reports back. An actor spawned as `UACube` may live at
`…PersistentLevel.UACube_1` — feeding that internal name into a selector finds nothing.

When you are unsure of exact labels, use `filter.name_pattern` (`"UA*"`) rather than guessing
`names`. It is the difference between a working call and a 404.

**Names that matched nothing come back in `unmatched_targets`.** Pass seven names, six exist:
the call still succeeds, operates on six, and lists the seventh with a ⚠️ in the message. That
is not a soft warning to skim past — on `ue_destroy_actor` it means one actor you meant to
delete is still there, and on `ue_focus_viewport` it means the screenshot you are about to
take is missing something. Read the line back to the user rather than reporting "done".

## Spawn

`ue_spawn_actor` has **no required fields** in its schema but rejects an empty call. Supply
either a single actor (`class` + `name` + optionally `mesh`, `location`, `rotation`, `scale`)
or several at once via `instances`. One call with an `instances` array is a single round-trip —
use it for more than one actor rather than looping.

**50 instances per call, maximum.** That is the engine-side cap (`ual.MaxBatchCreate`) and
going over it rejects the *whole* batch, not just the extras. Laying out 200 props is four
calls of 50, not one call of 200.

## Modular kits: read the pivot before you compute a single coordinate

> For a **row** of objects — a street, a fence, a shelf — skip this arithmetic entirely and use
> `ue_set_transform`'s `arrange` operation (see "Laying out a row" below). It works off measured
> bounding boxes, so the pivot cannot trip it up. The rest of this section is for single objects
> and for "this one edge must sit at this coordinate".

A floor, four walls and a ceiling only line up if you know **where each mesh's origin sits
relative to its geometry**. That convention is not uniform, not even inside one asset pack:
floors are often pivoted at a corner and extend towards +X/+Y, while walls and ceilings are
centred. Assume "centred" for all of them and you get a ceiling hovering outside the walls
and a door lying flat on the ground — which is exactly what happened on a real run that then
spent eight extra round-trips reverse-engineering the truth.

Two places give it to you, both read-only:

- **Before spawning** — `mesh_describe` on the asset prints an 原点 line: which axes are
  centred, which are flush against an end, and the two corner points in asset space. The
  `location` you pass to `ue_spawn_actor` lands the *origin*, so a mesh flush at -Z sits on
  the floor at `z: 0` while a centred one needs `z: height / 2`.
- **After spawning** — `ue_get_actor` with `return_bounds: true` adds `bounds_min`,
  `bounds_max` and `pivot_offset` (= location − bounds_min) in world space.

**Do not use `ue_focus_viewport` to work this out.** It flies the user's camera, changes their
selection, and only reports the union box of everything you passed it — three side effects for
a number two read-only tools already hand you.

Give `mesh` a real asset path (`/Engine/BasicShapes/Cube.Cube`) when the user wants to see
something. A `StaticMeshActor` with no mesh assigned is invisible, and later material
operations will silently skip it.

## Reading the scene back

`ue_get_actor` with `return_transform: true` gives you positions. **A query that matches nothing
returns `count: 0`, not an error** — so it is safe to use as an existence check before acting.

Do this after any bulk change. `ue_set_transform` reporting success does not prove the actors
moved; read the transforms back and confirm the numbers changed as intended.

`return_bounds: true` gives the **full** world-space size (length × width × height, in
centimetres) — not a half-extent, not a pair of corner points. Doubling it to "get the full
size" produces an object twice as big as the real one. It already includes the actor's scale
and rotation.

For a Blueprint instance, the reported `class` (`BP_Door_C`) is a class name, not an asset
path. The same result carries `blueprint_path` (`/Game/PartyMVP/Props/BP_Door`) — hand that
straight to `blueprint_describe`. Searching the content browser for something that looks like
the class name costs two extra round-trips and lands on the wrong asset when the name repeats.

## Transforms

`ue_set_transform` takes `targets` plus an `operation`. Its two parameters are typed as "any"
in the schema, so the shapes only exist in the tool description — read it before calling.

```
{ set:      { location: { z: 200 } } }     absolute — 200 cm = 2 m
{ add:      { location: { z: 50 } } }      relative (0.5 m), negatives allowed
{ multiply: { scale: { x: 2, y: 2, z: 2 } } }        multiplier, not a length
{ multiply: { location: { x: 100, y: 100, z: 100 } } } the metres→cm rescue
{ space: "Local", add: { ... } }           default space is World
{ snap_to_floor: true }                    on the operation itself, never inside `set`
{ arrange: { axis, gap, start?, align? } } lay a set out along one axis, no overlap
```

### Laying out a row — do not compute the spacing yourself

`arrange` is the operation for "put these N things in a line": a street of houses, a fence, a
shelf of props.

```
{ arrange: { axis: "y", gap: 200, start: -800,
             align: { axis: "x", edge: "min", value: -1310 } } }
```

`gap` is **edge to edge on the real bounding box**, so objects of different sizes never overlap
and never leave a gap you did not ask for. `align` pins each object's `min`/`max`/`center` to a
line on a second axis — that is how you get a row flush against a road or a wall. Without
`start` the row is laid out in place, beginning at the current front-most edge. With
`targets.names` the order you wrote is the order on the ground.

**This replaces the pivot arithmetic in the section above for rows.** Computing `x += width +
gap` from `mesh_describe` is where placement actually goes wrong: the origin sits in a different
spot on every asset, so the arithmetic is right for half your kit and silently wrong for the
other half. `arrange` never touches the origin — it pushes each object's measured `min` edge to
a cursor — so that whole class of failure does not exist. Read the pivot section for single
objects and for "this edge must sit at this coordinate" cases; use `arrange` for rows.

Limits worth knowing before you call it: 100 actors per call, mutually exclusive with
`set`/`add`/`multiply`/`snap_to_floor` (arrange first, then snap in a second call), and it
refuses rather than half-doing the job — an unresolved name, or a mix of `names` with
`filter`/`paths`, comes back as an error naming what it could not place. If a move fails
part-way it says how many already moved and gives you the `start` to pass on the retry.

Undo is **one entry per actor**, not one per call, because each actor is moved by its own
command. Arranging 20 buildings puts 20 entries on the stack — `ue_undo` with `steps: 1` will
put back exactly one of them.

### Putting something on the ground

`snap_to_floor` traces straight down from the bottom of the actor's bounds and lands that
bottom on the first surface it hits (`WorldStatic` first, then `Visibility`). It can be the
only field in the operation, or ride along with `set`/`add`.

Two things to know, both of which cost real round-trips on a real machine:

- **Writing it inside `set` is rejected** (400). It belongs on the operation.
- **A miss leaves the actor exactly where it was**, and the readback says so out loud
  (`⚠️ 贴地：… 位置没有变`). That happens when the ground mesh has no collision. Do not
  read a plain success as "it is on the floor" — read the snap line.

**The top of a bounding box is not the walkable surface.** A road mesh whose bounds top is
`+36` can still be walked on at `z≈0`; the difference is geometry above the road surface
(kerbs, signage) inflating the box. Deducing ground height from `bounds_max.z` floats
characters in the air. Prefer `snap_to_floor`; if it misses, read `bounds_min.z` of a prop
that is visibly already resting on the ground (a barrel, a lamp post) and use that.

One operation applies to every actor the selector matched, which is what makes "raise all the
lights by 50" a single call. It also means a sloppy selector changes more than intended —
run `ue_get_actor` with the same selector first and tell the user the count before mutating.

## Rotation: do not compute pitch signs in your head

UE rotation is `{ pitch, yaw, roll }` in degrees. Local **+X is the "front"**; lights,
cameras and scene captures all shoot along +X. The conventions that get people:

- **pitch: positive is nose-up.** A directional light that should shine *down* needs a
  **negative** pitch: dusk `-5…-15`, noon `-60…-90`. `pitch: +30` lights the scene from
  underground and the whole frame goes grey.
- **yaw:** 0 faces +X, 90 faces +Y, 180 faces -X, -90 faces -Y.
- **roll:** positive tilts right. It means nothing on a directional light — leave it 0.
  A plane or fog card uses `roll: ±90` to stand upright.
- **That `(pitch, yaw, roll)` order is for JSON parameters only.** In UE Python,
  `unreal.Rotator`'s *positional* arguments are `(roll, pitch, yaw)` — it follows
  `MakeRotator`. `unreal.Rotator(0, 90, 0)` gives you pitch=90, not yaw=90: a 5 m wall
  lies flat, the viewport camera points at the sky, and nothing errors. Always write
  `unreal.Rotator(roll=0, pitch=0, yaw=90)`; `ue_run_python_script` refuses the
  positional form outright. Read placements back with `ue_get_actor`, which translates
  the rotation into words ("front faces +Y") instead of three bare numbers.

This has actually happened: a sun set to `(pitch 30, yaw 180, roll -135)`, several rounds of
"the scene looks washed out" spent adjusting fog and exposure, and the user pointing out the
light was aimed upwards. Nothing rejected it; every call returned `success`.

So don't derive the rotator yourself — say what you mean and let the tool convert:

```
{ set: { sun: { elevation: 8, azimuth: 300 } } }   directional light: how high the sun is
                                                   (0 horizon … 90 overhead) and which way it
                                                   sits (0 = +X, 90 = +Y). Mutually exclusive
                                                   with set.rotation.
{ set: { face_direction: { x: -1, y: 0, z: 0 } } } point +X along a world vector, roll = 0.
                                                   "Face the camera" = camera - self.
```

Then **read the orientation line**. `ue_set_transform`, `ue_spawn_actor` and `ue_get_actor`
translate the rotation the engine reports back into words: *"方向光：太阳高度角 8°（黄昏），光往
+X 偏 -Y 30° 照下来"*, or *"正面(+X)朝 -X 方向，水平；顶面(+Z)朝 -Y（竖立着）"* for a fog card.
A directional light aimed upwards is flagged with ⚠️ in that line. It is computed from what
the engine has, not from what you sent, so it is the one check that cannot agree with your
own mistake. `ue_focus_viewport` and `ue_screenshot` say the same thing about the camera —
"仰视 34°" next to a frame full of sky means the camera drifted, not that the scene is gone.

## Which way does a character's face point?

An actor's rotation tells you where the actor's **+X** points. It does not tell you where the
**mesh's face** points — a skeletal mesh can be authored facing +X, +Y or anything else, and
that offset is why a character walks sideways ("crab walk") while every number you read looks
correct. Screenshots are a bad check here: the viewport shows a T-pose, the game shows an
animated pose, and at any distance the face is a few grey pixels.

Ask instead. `mesh_describe` on a skeletal mesh reports a facing line derived from the
reference skeleton's left/right bone pair:

> 朝向：正面偏离网格 +X 轴 -90.0°（依据 foot_l / foot_r 两根骨骼）

Then `actor yaw = the direction you want it to travel − that offset`. Walking along +X with a
`-90` offset means `yaw = 90`. When the rig uses unrecognised bone names the line says so
outright — only then fall back to a front-on playtest frame.

Put that compensation on the **actor's rotation**, and keep it as an editable variable rather
than a literal, so "it's backwards" is a one-value fix instead of a rebuild.

Do **not** try to fix facing with `SetRelativeRotation` on the **root** component.
`AActor::SetActorRotation` is implemented as `RootComponent->MoveComponent(...)` — the actor's
rotation *is* the root component's world rotation, one storage slot. Anything that also drives
the actor's rotation (your walk logic, a movement component, `SetActorRotation` on a timer)
overwrites your compensation, and the graph still reads as correctly wired and compiles clean.
A non-root mesh component is a different slot and does work, but there is no read-back for
component transforms at runtime, so you would be flying blind — use the actor rotation.

## Deleting

`ue_destroy_actor` is irreversible and will prompt the user for approval. It takes the same
selector as `ue_get_actor`, so one call deletes everything the selector matched. Name what will be deleted, by label and count, before calling — "delete 3 actors:
UACube, UASphere, UALight" is reviewable; "delete matching actors" is not.

After deleting, confirm with `ue_get_actor` on the same selector — an empty result is now a
normal success, so it is a clean way to prove the scene is in the state you claimed.
