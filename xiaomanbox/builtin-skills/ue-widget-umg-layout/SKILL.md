---
name: ue-widget-umg-layout
description_zh: 制作 UMG 控件蓝图：新建蓝图、按锚点和尺寸摆放控件、设置文本/颜色/数值、把控件暴露为变量供蓝图驱动、并渲染预览检查结果。当用户要做 HUD、菜单或任何游戏内 UI 时使用；编辑蓝图节点图、往关卡里摆 Actor 不适用于。
description: Build a UMG Widget Blueprint — create it, place controls with anchors and sizes, set text/colour/values, expose controls as variables for Blueprint to drive, and render a preview to check the result. Use when the user wants a HUD, menu or any in-game UI. Do not use for editing Blueprint node graphs, or for placing actors in the level.
---

# UMG widget layout

Verified against UE 5.5.4 with UnrealAgentLink by building a health-bar HUD end to end and
looking at the rendered result.

## The chain

```
widget_create          root container: CanvasPanel / VerticalBox / ScrollBox / GridPanel / …
widget_add_child       one call per control, carries its own initial layout
widget_set_slot        move/resize/align a control that is already there
widget_set_property    text, percent, colour, visibility, enabled
widget_make_variable   expose a control so Blueprint can drive it
widget_get_hierarchy   read back the real structure and slot data
widget_preview         render it and LOOK at it
```

## Control types are not a short list

`control_type` and `root_type` take any widget class the engine knows. The named ones are
`Button`, `TextBlock`, `RichTextBlock`, `Image`, `CanvasPanel`, `VerticalBox`, `HorizontalBox`,
`Overlay`, `Border`, `ScrollBox`, `SizeBox`, `Spacer`, `ProgressBar`, `Slider`, `CheckBox`,
`ComboBoxString`, `EditableText`, `EditableTextBox`, `SpinBox`, `GridPanel`, `WrapBox`,
`UniformGridPanel` — and anything else resolves dynamically by class name (drop the `U`), so a
project's own widget classes work too.

A scrolling list is a `ScrollBox`, a text field is an `EditableTextBox`, an inventory grid is a
`UniformGridPanel`. Do not fake them out of the four containers you happen to remember.

Slot arguments on `widget_add_child` only apply to CanvasPanel / VerticalBox / HorizontalBox /
Overlay parents. Adding into a ScrollBox or a grid works, but the child lands with default slot
settings.

## Preview returns the actual image — use it

`widget_preview` renders the widget and hands the image back to you directly. This is the only
tool in the UI set that tells you whether the thing you built looks right.

Call it after any layout change and actually read the picture: overlapping controls, text
running off the edge, a bar that is the wrong colour — none of that shows up in
`widget_get_hierarchy`, which will happily report a perfectly sensible tree for a broken layout.

Then tell the user what you saw, not just that the call succeeded.

## Layout: set it on add, fix it with `widget_set_slot`

Position, size, anchors and alignment are arguments to `widget_add_child`, so decide them when
you add the control. When the preview shows it sitting wrong, `widget_set_slot` changes them on
a control that already exists — **never delete and re-add to nudge a position**, that throws away
the control's properties, event bindings and variable flag.

`widget_set_slot` takes one group of arguments, chosen by the parent:

- in a CanvasPanel: `anchors`, `position`, `size`, `alignment`, `z_order`
- in a VerticalBox: `size_rule`, `padding`, `h_align`, `v_align`

Passing both groups is an error, not a guess. `anchors: "Center"` alone puts the control's
*top-left* at the centre; real centring also needs `alignment: { x: 0.5, y: 0.5 }`.

```
anchors:  "TopLeft" | "Center" | "BottomRight" | "Stretch" | …
position: { x: 40, y: 40 }        ← x / y
size:     { width: 300, height: 24 }   ← width / height, NOT x / y
padding:  { left, top, right, bottom }
h_align / v_align: Fill | Left/Top | Center | Right/Bottom
```

`position` and `size` use different key names. Getting it wrong is a clear validation error, not
a silent failure, so fix and retry — but write it correctly the first time.

Anchors decide what the control does when the screen resizes. A HUD element pinned to a corner
wants that corner's anchor (`TopLeft` for a health bar, `BottomCenter` for a hotbar); a
background wants `Stretch`. Choosing `TopLeft` for everything gives a layout that breaks on any
other resolution — ask the user about target resolution if it matters to them.

## Properties you can set

`Text`, `Visibility`, `IsEnabled`, `ToolTipText`, `Percent`, `FontSize`, `Justification`, and
the colour trio: `FillColorAndOpacity` (ProgressBar), `ColorAndOpacity` (TextBlock / Image),
`BrushColor` (Border).

Colour values are **0..1 floats**, not 0..255:

```
{ "r": 0.85, "g": 0.12, "b": 0.12, "a": 1 }
```

`FontSize` is a number and works on `TextBlock` / `RichTextBlock`; it changes the size only and
leaves the typeface alone. `Justification` takes `Left` / `Center` / `Right` on any text control.

Still out of reach: border radius, background brush textures, swapping the font asset itself.
Say so rather than reporting a half-styled widget as finished — and do **not** go looking for a
Python route around it.

When a call fails, read the `error` and the `hint` in the return: they say whether the control was
not found, the value type was wrong, or that control type simply has no such property. Retrying
with a different property name is not a fix.

## Updating text at runtime: `TextBlock.SetText`, never `SetTextPropertyByName`

A score that has to change while the game runs is set in the Blueprint graph, not here. Use the
control's own member function — `TextBlock.SetText`, `ProgressBar.SetPercent`,
`Border.SetBrushColor` — written straight into `blueprint_apply_graph` as
`member_name: "TextBlock.SetText"`.

`blueprint_search_nodes` **will not find these**. Its range is function libraries plus the
Blueprint's own parent chain, and a control class is neither. Searching for `SetText` returns
`KismetSystemLibrary.SetTextPropertyByName` instead — and that node is a trap: it writes the
field by reflection without telling Slate, so the value changes and **the screen never repaints**.
The variable reads back correctly while the user stares at the old number.

Not finding a function in the search does not mean it does not exist. Write the class name
yourself.

## Checking it actually shows on screen

`widget_preview` renders the widget asset and is the right check for layout, colour and sizing.

For "is my HUD really up during play", the **default** paths of `ue_screenshot` and `ue_playtest`
are useless: those render the scene, and UMG is a layer drawn on top of it. A clean-looking
screenshot with no HUD in it proves nothing. Three things that do work:

- `ue_screenshot` with `show_ui: true` — grabs the editor window as the user sees it, PIE
  viewport and HUD included.
- `ue_playtest` with `frame_mode: "window"` and `frames: 6` — same pixels, but sampled over the
  whole run and stitched into one timeline image. This is the only way to see a HUD that appears,
  changes or disappears *while the game runs* — a single frame cannot tell "never showed up"
  from "showed up and went away". The editor's panels are in those frames too, so the game area
  is only part of each cell; read numbers off it only if they are large.
- Ask the user to press Play and tell you what the screen says.

Reading the TextBlock's `Text` property back is **not** evidence that the text is on screen —
that is exactly the false positive `SetTextPropertyByName` produces. Say what you verified and
how; do not report a runtime UI as confirmed on the strength of a property read.

## Exposing controls to Blueprint

A control is only reachable from the Widget Blueprint's graph if it is a variable.
`widget_make_variable` does that; `widget_get_hierarchy` reports `is_variable` so you can check.

Any control the game needs to update at runtime — a health bar's `Percent`, a score `TextBlock` —
must be exposed. Values set with `widget_set_property` are just the design-time defaults;
they are what the user sees in the editor, not what the game will show.

## Reading the structure back

`widget_get_hierarchy` returns each control's `slot_data` with the resolved `position`, `size`
and `anchors` — use it to confirm layout actually applied.

`visibility` is the design-time `ESlateVisibility` value (`Visible`,
`SelfHitTestInvisible`, `Hidden`, `Collapsed`). A CanvasPanel root normally reports
`SelfHitTestInvisible` — that is correct and not something to fix.
