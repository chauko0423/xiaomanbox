---
name: ue-system-diagnostics
description_zh: 在编辑器内运行控制台命令和 Python、读取性能数据、检查崩溃报告、检查本地 ZenServer 缓存、启用或禁用插件。当任务需要专用工具表达不了的操作，或当用户问性能、崩溃、着色器重编译、缓存磁盘占用、插件开关时使用；普通资产、Actor、蓝图工作各有专属工具，不适用于。
description: Run console commands and Python inside the editor, read performance numbers, check crash reports, inspect the local ZenServer cache, and enable or disable plugins. Use when the task needs something the dedicated tools cannot express, or when the user asks about performance, crashes, shader recompiles, cache disk usage or plugins. Do not use for ordinary asset, actor or Blueprint work — those have their own tools.
---

# System, Python and diagnostics

Verified against UE 5.5.4 with UnrealAgentLink. These are the highest-privilege tools in the set:
six tools, of which three — `ue_run_console_command`, `ue_run_python_script` and
`ue_manage_plugin` — are marked irreversible and will ask the user for approval.

## Python is the escape hatch — and it returns output now

`ue_run_python_script` runs inside the editor with the full `unreal` module. Anything the
dedicated tools cannot do, this can — reading and writing alike.

It returns whatever the script prints:

```python
import unreal
ar = unreal.AssetRegistryHelpers.get_asset_registry()
print("count:", len(ar.get_assets_by_path('/Game', recursive=True)))
```
→ `stdout: count: 42`

Use `print()` for anything you want to read back. For structured data you also need to act on,
assign `output_data`:

```python
output_data = {"total": 42, "sample": ["/Game/A", "/Game/B"]}
```

Errors come back with the real Python message (`name 'foo' is not defined`), so read it and fix
the script rather than re-running it unchanged. Output is truncated to the last 8000 characters —
aggregate inside the script instead of printing a row per asset.

It prompts for approval every time, because a Python script can do anything. Say what the
script will do before calling — "count the static meshes over 100k triangles" is reviewable,
"run a diagnostic script" is not.

### "Can do anything" is about capability, not permission

Where another skill says a thing must not be done, that ruling wins here too — Python does not
become the way to do it anyway. `ue-sequencer` is the explicit case: it forbids editing a user's
sequence through `ue_run_python_script`, and an approval prompt does not overturn that. The
user approving a script means they accept it runs, not that they re-authorised what a domain
skill already ruled out.

Approval is the last gate, not the first. If a dedicated tool exists, use it; if a skill forbids
the operation, say so instead of scripting around it.

## Console commands return their output

`ue_run_console_command` now reports what the command printed:

```
r.ScreenPercentage  →  r.ScreenPercentage = "100"   LastSetBy: Scalability
```

Commands that only toggle an on-screen overlay (`stat unit`, `stat fps`) produce no text, and the
result says so — that is normal, not a failure. Some commands only write to the engine log; use
`ue_message_log` for those.

## Performance numbers need their context stated

`ue_get_performance_stats` samples the editor **as it is right now**. An editor window in the
background throttles itself hard — real measurement: 4.8 FPS with render thread and GPU both at
zero, which is not a performance problem, it is an editor that is not drawing anything.

The tool flags this case. When it does, do not report a frame-rate problem: ask the user to bring
the editor forward and enter PIE, then sample again. Even a clean sample is the editor, not a
packaged build — say so when you report numbers.

## Crashes: check the count, not the presence of a report

The basic release does not expose `ue_get_crash_logs`: that RPC is not implemented
by the bundled plugin. Use `ue_get_project_info` to identify the user's project,
then `glob` and `read_file` to inspect that project's `Saved/Crashes` or `Saved/Logs`
when the user asks for diagnosis. A normal run log is context, not proof of a crash;
confirm an actual crash report or fatal error before saying the project crashed.

## Local cache: ask ZenServer, not the file system

`ue_zen_server` reads the local ZenServer (the engine's derived-data cache service, default
`127.0.0.1:8558`) over its own HTTP interface: hit ratio, cache and CAS disk usage, and which
projects it holds data for. Read-only; it never cleans anything.

Reach for it when the user says the editor keeps compiling shaders, loading is slow, or the
system drive is filling up. Three things to say with the numbers:

- Hits and misses count **since ZenServer last started**, not all time. When the result says
  `sample_too_small`, do not draw a conclusion from the ratio.
- A low ratio right after an engine upgrade, a cache wipe or the first open of a project is
  expected, not a fault.
- `project_file_missing: true` means the `.uproject` is gone; that data is probably unused. Offer
  that as a fact, not as a cleanup you will do — clearing cache means recompiling shaders next
  time, and that is the user's call.

ZenServer starts and stops with the editor. If the tool cannot reach it, the editor is closed,
the project does not use Zen for its local cache, or the port was changed — do not go scanning
`Zen/Data` on disk instead; the CAS folder holds so many small files that a walk takes minutes.

## Plugins

`ue_manage_plugin` queries, enables and disables by name. It reports `requires_restart` — when
that is true, the change does not take effect until the user restarts the editor, and you must
tell them. Enabling a plugin the user did not ask for is not a fix; propose it first.

Enable and Disable rewrite the project's `.uproject`, and the tool reads the file back before
reporting success. So `success: false` here means the plugin is genuinely still in its old
state — never tell the user to restart on a failed call. It happened once: the tool said
"Plugin enabled. Restart required.", the entry was never written, and the user restarted twice
for nothing.
