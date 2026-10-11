---
name: ue-content-import-organize
description_zh: 把外部文件导入工程，查找和检查已有资产，批量改名或迁移，执行命名规范，追踪依赖与被引用关系，清理重定向器，以及带着依赖把资产迁移到别的工程。当用户要导入文件、整理 Content 目录结构、规范命名、回答「什么在用这个资源」、把资产挪到另一个工程时使用；往关卡里摆东西、编辑材质节点图不适用于它。
description: Bring external files into the project, find and inspect existing assets, batch-rename or relocate them, enforce a naming convention, trace dependencies and referencers, clean up redirectors, and migrate assets with their dependencies to another project. Use when the user wants files imported, the content folder tidied or restructured, names normalised, "what uses this" answered, or assets moved to another project. Do not use for placing things in the level, or for editing a material's node graph.
---

# Content import and organisation

Verified against UE 5.5.4 with UnrealAgentLink by running the whole job end to end:
import three textures → find them → inspect → rename to a convention → audit → clean up.

## Before you start: get real paths, never guessed ones

`ue_content_import` needs **absolute file paths**, and a wrong one fails the whole call.
Three ways to get them, in order of preference:

- **On disk** — `list_local_dir` for one folder, `find_local_files` with a glob
  (`**/*.fbx`) when the files could be at any depth. "把 D:/Downloads/kit 里的都导进来"
  starts here: list it, show the user what you found, then import.
- **In the box's asset library** — `search_assets` first, take `realPath` from each result.
- **Neither** — ask the user for the paths. Do not guess filenames from a folder's name.

## Import does more than you asked

Importing image files also **auto-generates PBR material instances** from them. Three textures
came back as five assets: the three textures plus `MI_wood_Mat` and `MI_rock_Mat`, each flagged
`auto_generated: true`.

Always compare `imported_count` against `requested_count` and tell the user what extra assets
appeared. They will otherwise find material instances in their project that they never asked
for and have no idea where they came from.

Naming matters at import time: a file named `*_normal.png` is detected as a normal map and
imported with `TC_Normalmap` / BC5 / sRGB off. Passing badly named files through gives wrong
compression settings that are tedious to fix afterwards — say so before importing rather than
after.

Check `saved_count` too. It should equal the number of assets imported; a `save_warning` means
some assets exist only in the current editor session and will be lost when the user closes UE.

**Name them on the way in, not afterwards.** `asset_names` takes a per-file map —
`{ "hero.fbx": "SK_Hero", "wood_color.png": "T_Wood_D" }` — and the rename happens while the
asset is brand new and nothing references it yet, so it leaves no redirectors. Renaming later
with `ue_content_move` rewrites every referencer but leaves a redirector behind at each old
path — cleaning those up is a separate `ue_fixup_redirectors` step. A name already taken is **skipped**, not forced: the asset keeps its
original name and the reply carries `rename_failed` plus a line in the message — read it, do
not assume the name you asked for is the name you got. Only rename when the user asked for a
convention; they may be looking for the file under its original name.

**Scale is a parameter, not a fact of life.** Meshes come in at the format's default: OBJ ×100
(the format carries no unit, so the plugin assumes metres), glTF/GLB and FBX ×1 (the engine
already converts metres to centimetres for glTF, and FBX carries its own units). When a model
lands a hundred times too small or too large, re-import it with `scale` set — do not tell the
user to go fix Build Scale in the editor themselves.

## Same-named textures inside meshes silently overwrite each other

FBX and GLB carry their textures inside the file, and the names are whatever the author chose —
AI-generated models are almost always `Color`, `Normal`, `Roughness`, `Metallic`. Import two such
models into one folder and the engine behaves **asymmetrically**: the meshes get deduplicated
(`model` → `model_1`), the textures do not. The second file's textures are skipped without an
error because the target names already exist, so every model's material points at the *first*
model's textures. Different UV layouts then sample the wrong texture and the surfaces render as
scrambled fragments. `imported_count` still reports success.

`isolate` defaults to `auto`, which puts each mesh file in its own subfolder whenever there is
risk — two or more mesh files in one call, or a destination folder that already has assets in it.
So the normal case is handled and you do not pass the parameter. Two things you still owe the
user:

- Read `isolated` in the response. The assets are in `<destination_path>/<filename>/`, not
  directly in `destination_path` — say so, and use those paths for anything downstream.
- Treat `reuse_warning` as a failure even though `ok` is true. It means textures were reused
  rather than created; re-import with `isolate: "always"` or a fresh `destination_path`.

Pass `isolate: "never"` only when the user explicitly wants everything flat in one folder and
accepts that same-named textures will collide.

## Third-party packs: the materials usually arrive broken

An FBX pack imports "successfully" and the meshes render as red/blue/green candy. That is not a
failed import — it is the normal outcome. The FBX material definitions are Phong, which the
engine cannot represent, so it parents every material instance to
`FBXLegacyPhongSurfaceMaterial` (a placeholder) and leaves the PBR textures **imported but
unreferenced**. Nothing in the import response says this.

Run this after importing any pack you did not author, before telling the user it worked:

```python
import unreal
reg = unreal.AssetRegistryHelpers.get_asset_registry()
root = "/Game/Imported"   # the destination_path you imported into
broken, textures, used = [], [], set()
for data in reg.get_assets_by_path(root, recursive=True):
    asset = data.get_asset()
    if isinstance(asset, unreal.MaterialInstance):
        parent = asset.get_editor_property("parent")
        if parent is None or "Legacy" in parent.get_name():
            broken.append((data.package_name, parent.get_name() if parent else "None"))
        for dep in reg.get_dependencies(data.package_name,
                                        unreal.AssetRegistryDependencyOptions()) or []:
            used.add(str(dep))
    elif isinstance(asset, unreal.Texture):
        textures.append(str(data.package_name))
output_data = {
    "broken_materials": broken,
    "orphan_textures": [t for t in textures if t not in used],
}
print(output_data)
```

`broken_materials` non-empty means the pack needs real materials built by hand —
`material_create` plus `material_apply_graph` wiring BaseColor / Metallic / Roughness / Normal /
AO, then `material_apply` onto the slots. Say that to the user before they discover it in the
viewport; it is a job of its own, not a detail of the import.

**Material slot names are not a mapping.** Maya exports them as `blinn2`, `blinn3` … which carry
no meaning at all. Do not guess which texture set belongs to which slot — assign one, look at it,
then move on. A guessed mapping looks plausible in the content browser and wrong in the game.

## Geometry you generated yourself

Writing an OBJ with a script and importing it is a legitimate way to make simple shapes.
Get the face syntax right or the import can take the whole editor down with it.

In `f a/b/c`, the three slots are **vertex / texture coordinate / normal**. The middle slot is
the UV index — it is not the normal. When the file has normals but no UVs, the middle slot must
be left empty with a double slash:

```
vn 0.0 0.0 1.0
f 1//1 2//2 3//3      ← correct: vertex // normal
f 1/1 2/2 3/3         ← wrong: claims UV #1 exists when the file has no 'vt' lines
```

The second form crashed UE 5.5 outright on 2026-09-02 — the Interchange OBJ translator indexes
an empty UV array and the assertion is fatal, so everything unsaved in the editor is lost. The
plugin now checks OBJ files before handing them over and returns `rejected` with the offending
line instead of importing, but do not rely on that to catch it: write the file correctly.

Rules that keep a generated OBJ importable:

- Indices start at **1**, not 0, and must not exceed the number of `v` / `vt` / `vn` lines.
- Emit `vt` lines if any face references a UV slot. No UVs at all is fine — use `v//vn`.
- One `vn` per face corner if you want flat shading; per-vertex normals are averaged and the
  result stops looking faceted.
- Units are metres, so the mesh arrives 100× small in UE unless you pass `scale`. The plugin
  already compensates by default on 5.5+; check the reported size afterwards rather than
  assuming.

**When an import is followed by the connection dropping, do not resend the same file.** A crash
loop is the failure mode here: the editor dies mid-import, reconnects, and an identical retry
kills it again. Report what happened and inspect the file first.

## Just-imported `.uasset` files and the asset registry

Importing from the box's library (`project_manage` → `import_assets`) **copies packages
straight into `Content/`** without going through the engine's import API, so a running editor
does not necessarily know they exist yet. The box now asks the engine to rescan the destination
folders right after copying and says so in the reply (`🔄 已让引擎重扫…`).

When that line is missing — or says it could not rescan, which is what an older plugin build in
the user's project gives you — the registry is lagging, and the symptom is nasty because three
readings disagree: `ue_content_search` reports `找到 0 个匹配的资产（这就是全部）`,
`ue_content_describe` on a mesh happily lists a material dependency that "does not exist", and
the scene renders fine. Do not conclude the assets failed to import.

What to do instead: use the assets **by path** (describe, spawn, apply) — that works regardless
of the registry — and verify what landed on disk with `find_local_files` over the project's
`Content/` folder. Tell the user the search index is catching up, not that assets are missing.

## Path formats — the one thing that bites

`ue_content_search` returns a **package path**: `/Game/Art/Textures/wood_normal`.
Several operations want the **object path**: `/Game/Art/Textures/wood_normal.wood_normal`.

Both forms now work for `ue_content_describe` and `ue_content_move`, but `ue_content_delete`
wants the object path. When passing a search result into delete, append `.` plus the asset name.

## Inspecting

`ue_content_describe` answers the questions users actually ask:

```
assetClass: "Texture2D"
packageSizeBytes: 26388
details: { width: 2048, height: 2048, pixel_format: "BC5",
           compression: "TC_Normalmap", srgb: false }
referencersCount: 1  →  MI_wood_Mat
```

For static meshes `details` carries `triangles_lod0`, `vertices_lod0`, `lod_count`,
`material_slots`, `nanite_enabled` and `bounds` (`size` plus the `min` / `max` corners in
asset space). Those corners are the **pivot convention** — whether the origin is centred,
sitting on the base, or at a corner — which is what you need before placing modular pieces.
`mesh_describe` says the same thing in words and adds LODs, collision and sockets.

**Read `referencers` before moving or deleting anything.** A non-zero count means other assets
point at it — say which ones, and let the user decide, rather than breaking references silently.

## Renaming and moving — always as a batch, always dry-run first

`ue_content_move` takes a **list**. One pair is fine, but never loop it: the engine loads every
referencer of the whole batch once, so ten materials used by the same thirty levels cost one load
as a batch and ten loads one by one.

```
moves: [{ source: "/Game/Props/rock", destination: "/Game/Props/SM_Rock" }]   # rename
moves: [{ source: "/Game/Props/SM_Rock", destination: "/Game/Env/Rocks/" }]  # move, keep name
folder_moves: [{ source_folder: "/Game/Temp/Kit", destination_folder: "/Game/Env/Kit" }]
```

Run with `dry_run: true` first. The plan lists, per asset, where it goes, how many assets
reference it, and whether the destination is taken. Show that to the user, then run for real.
`on_conflict` defaults to `fail` — any taken destination means nothing moves; switch to `skip`
or `auto_rename` only when the user says so.

After a real run the rewritten referencers are saved (`saved_count`), but the redirectors it
left behind are **not** cleaned up — `redirectors_fixed` is always 0; see *Redirectors* below.
If `dirty_after` is non-zero, those are packages modified outside the command and were
deliberately left alone. Every item is read back: `failed` means the asset is not at the
destination, whatever the engine claimed.

Levels (`World`) are refused — renaming a level must relocate its external actors too, which is
a level-tool job. Redirectors are refused as sources — fix them up first.

## Naming convention: audit, then batch-rename

`ue_content_naming_audit` checks a folder against Epic's recommended prefixes (`T_`, `SM_`,
`SK_`, `M_`, `MI_`, `BP_`, `AC_`, `BI_`, `WBP_`, `ABP_`, `AS_`, `AM_`, `BS_`, `DT_`, `E_`,
`F_`, `FXS_`/`FXE_`, `LS_` …) without loading anything. Each violation comes with a
`suggested_path`; `conflict: true` means that name is already taken.

For files you are about to import, do not audit-then-move: pass the names straight to
`ue_content_import`'s `asset_names`. The audit is for assets that are already in the project.

Epic's table is always the baseline. On top of it the box adds the user's own **rename rules**
(match at start / end / anywhere, replace with plain text) for things a prefix table cannot
express (`_FINAL` suffixes, `Temp_` → `WIP_`), and their **asset-home directories** when you ask
for a location check. Pass `use_project_rules: false` to audit against Epic alone.

**Their customized prefixes are reported, not applied.** If the user changed a prefix, the
summary names the change and says explicitly that the audit did not use it. Forwarding a prefix
makes the plugin forget the old one, so every *correctly* named asset of that type comes back as
"missing prefix" with a doubled suggestion (`T_Rock` → `TX_T_Rock`) and no warning marker. To
audit by their convention, confirm with the user first, then pass it yourself via `rules` keyed
by engine class name (`Texture2D`, not `Texture`) — and warn them that their already-correct
assets will all be listed for renaming.

Rules of engagement:

- **`unknown_classes` were not checked.** Audio, fonts and other types have no agreed prefix,
  so the tool refuses to guess. These are the safe case for `rules`: the plugin has no rule to
  displace, so passing `rules: { SoundWave: "S_" }` only adds coverage. Ask the user what they
  use, then audit again.
- **There is a second list.** After the violations, the summary lists assets whose prefix is
  *correct* but which still break the user's own rules. They are not counted in the violation
  total; feed them to the move the same way.
- **`conflict_unknown` means we could not check.** When a suggested name was rewritten by the
  user's rules, the plugin never queried the registry for that name, so "is it taken" has no
  answer here. Those entries make `dry_run` mandatory, not optional.
- **Rules that did not run are named in the summary, not swallowed.** A rule is refused when it
  has no name, or when its "text to find" is empty. Directory entries that are not package paths
  (no leading `/Game`) are refused the same way. Relay these to the user — the fix is theirs to
  make in Preferences, and until they do, that part of the audit did not happen.
- **Read the summary's own caveats; they are the whole story.** Everything you need is in the
  text — the structured fields go to the host UI, not to you. In particular the summary will
  tell you, in words, when: the compliant sweep was cut short by `limit` (so the "prefix is fine
  but still needs renaming" group and the location check covered only part of the project — do
  not report it clean); the location check ran but compared nothing; or a rule was refused.
- **`limit` gates both lists, and raising it is cheap.** The same `limit` caps the violation list
  and the compliant sweep, so it also decides how much of the project the user's own rules and the
  location check covered. The summary prints at most 200 rows no matter what, so raising `limit`
  (max 2000) costs engine time only — it cannot blow up your context. When a list is cut short,
  raise `limit` first; narrow `path` and audit per directory once 2000 is not enough.
- **`check_directory` only works if the user edited the directory table.** Only entries they
  changed from the factory value count, and only for classes actually scanned — so on an untouched
  settings page, or when their entries cover types this scan did not touch, **not one asset's
  location gets checked**. The summary says so in words when that happens. Never read silence or
  "nothing to compare" as "the layout is fine": say the check could not run and offer to have them
  fill the table in.
- **Feed the audit straight into the move.** Take `{ source: path, destination: suggested_path }`
  for every non-conflicting violation and hand the list to `ue_content_move` with `dry_run`
  first. Conflicting ones need a different name — list them and ask.

## Dependencies: what breaks, what must come along

`ue_content_dependencies` walks the asset registry (no loading) from an asset **or a folder**:

- `direction: "referencers"` on a folder → `referencers.external` is everything *outside* the
  folder that points in. Deleting the folder breaks those; moving it is safe (redirectors).
- `direction: "dependencies"` on a folder → `dependencies.external` is what the folder pulls in
  from outside. That is the extra baggage a migration must carry.
- `dependencies.missing` are references to packages that no longer exist — the "Failed to load"
  errors on open. Report them; the fix is a human decision.
- `direction: "unreferenced"` lists assets nothing points at. Registry-only: assets loaded by
  path from code or config are invisible to it, so it is a *candidate* list, never a delete list.

For "why is this so big" keep using `ue_asset_size_map`; for one asset's direct neighbours
`ue_content_describe` is still the quickest.

## Migrating to another project

`ue_content_migrate` is the right-click *Migrate*: copies the assets plus their dependency
closure into another project's `Content`, preserving folder structure. `destination` takes the
`.uproject`, the project folder, or its `Content` folder. Levels bring their One-File-Per-Actor
packages automatically.

`dry_run: true` first — it reports file count, total size, what already exists at the
destination (`skipped` under the default `on_conflict: "skip"`) and plugin content the target
project cannot receive (`external_skipped`, because it lacks that plugin). Unsaved sources are
saved before copying unless `save_first: false`. Every file is verified by size after the copy.

It does not open or modify the destination project. Tell the user to open it and let the
registry scan; leftover redirectors can be cleaned there.

## Redirectors

`ue_content_move` leaves its redirectors in place on purpose; nothing cleans them unless you run
`ue_fixup_redirectors`. Redirectors are harmless (the engine follows them), so cleanup is optional
and can wait. Start with `dry_run: true`: `details` shows each redirector's `target` and flags
`broken` ones whose target is gone. Those cannot be fixed up; `delete_broken: true` removes them,
which turns "follows a dead link" into "object not found" for anything still referencing them —
confirm nobody does first (`ue_content_dependencies` with `direction: "referencers"`). Pass
`paths` to limit the run to a few redirectors or one folder instead of scanning all of `/Game`.

A real run (`dry_run: false`) **opens a modal "Redirector Update Report" window in the editor on
UE 5.4+ that a human must click** — its default button is *Keep Redirectors*, so tell the user
to pick *Delete Unreferenced Redirectors*. Warn the user before running; if nobody clicks within
10 minutes the call times out with the outcome unknown (the engine finishes when they click), so
re-run `dry_run: true` to read back instead of repeating the fixup. When the editor cannot show
the window (script mode, no renderer) the plugin refuses with 409 and `details.how_to`. The engine
also saves every rewritten referencer as-is: `dirty_referencers` in the dry run names referencers
with unsaved user edits that would be committed — have the user save or revert those first.

## Auditing

`ue_content_audit_optimization` with `check_type: "TextureSize"` scans `/Game` by default and
returns the offenders, not just a count:

```
large_textures: [ { path, name, width, height } … ]
```

Pass `path` to narrow it to a folder — useful right after an import. "Large" means at least
4096×2048 pixels in total area, so a 4096×1 divider strip is correctly not flagged.

Report the actual paths so the user can act. A bare count ("you have 4 large textures") is not
something anyone can do anything with.

That tool loads every asset it inspects, so it is slow on a large project. If the question is
just "which assets are big", `ue_project_asset_ranking` answers it from asset-registry tags
without loading anything. A full audit of the project belongs to `ue-project-audit`, not here.

## Deleting

`ue_content_delete` takes asset object paths **and folder package paths** in the same `paths`
array. To remove a whole pack, pass the folder with a trailing slash (`/Game/ThirdParty/AnimeGirl/`):
the tool expands it (subfolders included) and submits **one batch**, so the engine runs one
garbage-collection pass. Call it with `dry_run: true` first and show the user the list — the
approval card only shows the folder name. Redirectors inside the folder are left alone (use
`ue_fixup_redirectors` for those). Over 500 assets in one folder it refuses and tells you to
split by subfolder.

Never loop `EditorAssetLibrary.delete_asset` in Python and never feed search results in one at
a time — each call runs a full GC; on a large level that is seconds per asset and a few hundred
assets freeze the editor for a quarter of an hour, after which every engine command times out
together. If a delete call itself times out, the engine is most likely still deleting: do not
resend, check `ue_session_health`, then read the folder back with `ue_content_search`.

Deletion is irreversible and will ask the user for approval; name what goes, and check
`referencers` first.
