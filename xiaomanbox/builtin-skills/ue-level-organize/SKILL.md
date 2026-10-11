---
name: ue-level-organize
description_zh: 把打开关卡里的 Actor 归入世界大纲（Outliner）文件夹。当用户想整理大纲、按类型或名字分组 Actor，说「把场景整理一下」「把所有灯放到一个文件夹里」时使用；性能优化分析、移动 Actor 位置、创建和删除 Actor 不适用于。
description: Files actors in the open level into World Outliner folders. Use when the user wants the Outliner tidied, asks to group actors by type or by name, or says things like "把场景整理一下"、"把所有灯放到一个文件夹里". Do not use for optimisation analysis or asset size reports, for moving actors in the world, or for creating and deleting actors.
---

# World Outliner tidy-up

Verified against UE 5.5.4 with UnrealAgentLink. One tool.

This skill only changes **organisation**. Nothing moves in the world, nothing is created or
deleted, and the user can undo it by dragging folders back.

Looking for what is *in* the level and what it costs — triangle counts, missing collision,
heavy assets — is `ue-project-audit`, not this.

## `level_organize_actors`

Moves matching actors into a World Outliner folder. Two ways to select:

```
class: "PointLight"                       simple case
filter: { name_pattern: "SM_Rock*" }      same filter shape as ue_get_actor
```

It reports `count` and `total_found`. **Check both.** Matching more than you intended is the
failure mode here, and those two numbers are the only place it is visible — a filter that was
meant to catch 12 rocks and caught 400 actors looks exactly like success otherwise.

## Workflow for "把场景整理一下"

1. `ue_get_actor` to see what is actually in the level, grouped by class.
2. Propose the folder structure to the user, with counts: "灯光 12 个 → Lighting，
   静态网格 240 个 → Props/…".
3. Organise once they agree.

Inventing a folder scheme and applying it silently gives the user an Outliner they did not
design and now have to re-learn. Say which actors you are about to file and where, first.
