---
name: ue-ai-render-from-blockout
description_zh: 把白盒布局变成 AI 上色的概念图：先取景，截取视口画面，再把截图作为参考交给 generate_image。产出是一张新图，不是场景的真实渲染。当用户要概念图、氛围图、给刚搭好的白盒出效果图，说「白盒出个概念图」「用 AI 把这个布局画成成品」时使用；Movie Render Queue 出片、引擎内真实渲染、普通截图不适用于它。措辞含糊时（如「把场景渲染成成品」）先问清要哪种。
description: Turns a greybox blockout into an AI-painted concept image by framing the shot, capturing the viewport and feeding that capture to generate_image as a reference. What comes back is a new picture, not the actual scene rendered. Use when the user asks for concept art, key art, a mood board, a look-dev image or a client-facing 效果图 of what was just blocked out, or says things like "白盒出个概念图"、"用 AI 把这个布局画成成品"、"照着这个场景出张效果图". Do not use for Movie Render Queue, sequence or movie output, in-engine rendering, plain screenshots, or any request where the user expects the real scene rendered. When the wording is ambiguous, such as 把这个场景渲染成成品, ask which one is wanted before generating anything.
---

# Painting a blockout with AI

## First: is this an AI image or an engine render?

「渲染」 means two completely different things to the person asking, and the two are not
substitutes:

| They want | Signals | This skill? |
|---|---|---|
| An AI-painted picture | 概念图、效果图、氛围图、给客户看的图、参考图、看看做出来什么感觉 | Yes |
| The engine rendering the real scene | 出片、出序列、Movie Render Queue、MRQ、出视频、渲染队列、带 Alpha、EXR、交付给合成 | No |

**When it is ambiguous, ask.** One short question costs nothing; an AI image handed to someone
who wanted a shot costs them the time they spend discovering it is unusable.

Say what the difference is when you ask, because most people do not think about it until it is
pointed out: an AI image is **newly painted**. It matches the composition and roughly the
layout, and nothing in it corresponds to an asset in the project. It cannot be composited,
cannot be re-rendered at another resolution, cannot have a pass pulled out of it, and the next
image will not match this one exactly.

### If they want a real render

There is no Movie Render Queue tool. Say that plainly rather than substituting an AI image or
improvising a render script with `ue_run_python_script` — a half-working MRQ script that reports
success is worse than not having one.

What is available instead: framing the shot, laying out the camera cuts and keys
(`sequence_camera_cuts`, `sequence_camera_keys`), checking the sequence over (`sequence_audit`),
and `ue_screenshot` for a quick look. Offer those, and leave the render itself to the user
unless they explicitly ask for a scripted attempt and accept that it is untested.

## The loop

Frame → capture → generate → look. Four tools, in that order, every time.

```
ue_focus_viewport  names: ["SM_Blockout_Hall"]
ue_screenshot                     → note the returned path
generate_image     prompt: "...", reference_images: ["<that path>"]
```

The capture is the composition. The prompt is everything the capture cannot show:
materials, lighting, time of day, weather, lens, mood, style.

## Frame the shot first

The viewport shows whatever the editor camera happens to be pointed at, so an unframed
capture usually renders something nobody asked for. `ue_focus_viewport` takes `names` — the
labels from the World Outliner — and frames those actors.

Before spending a capture, read what focus returned: `aim.on_target: false` means the camera
is not actually looking at the target, `transition_settled: false` means it is still flying,
and `occluded_by` means something is in the way. Fix the framing, then capture.

For one part of a large object, pass `region: "bottom" | "middle" | "top"` instead of trying
to compute camera coordinates.

## Write the prompt for the result, not the blockout

Describe what the picture should **become**, and explicitly preserve the camera, layout and
silhouettes the user wants kept. Do not describe "grey untextured boxes" as the target appearance.
Use the `image-generation` skill for general prompt shaping, reference roles and output checks.

- ✓ `weathered limestone hall, warm afternoon sun through high windows, volumetric dust, wide lens, architectural photography`
- ✗ `a grey box room with some grey box pillars, make it look nice`

Chinese or English is fine. Preserve the user's exact requested text, counts and constraints;
do not add creative requirements to an already specific request.

## State the lighting in the prompt

The capture's brightness is only a baseline when the scene's exposure is locked — the
`exposure` line in the screenshot result says whether it is. Either way, **state the lighting
you want in the prompt** rather than letting the reference decide it: a greybox is usually lit
by whatever the template shipped with. Composition, layout, material colour and shadow
direction in the capture are trustworthy regardless.

## One image first

Every call spends the user's own API credits, and `count` multiplies the bill. Generate one,
look at it, then decide. Ask before generating a batch.

When an iteration is authorized, pass the image the user liked as a reference, describe only
the targeted change, and repeat the camera/layout/identity invariants. Label each reference's
role if also supplying the blockout. A seed does not guarantee consistency; the current OpenAI/GPT
and Gemini adapters do not send it. Ask via `ask_user` before spending on an unapproved retry;
an explicit request to redo already authorizes that retry.

## Look at what came back, then say what you see

The image is returned to you directly. Read it and describe it in one or two concrete
sentences: what is in the frame, whether it matches the blockout, what is off. `success` in a
tool result means the call went through, not that the picture is right — never report the
render as done on the strength of that field alone.

If the render lost the blockout's layout, the reference did not carry: check that
`reference_images` really got the capture path, and that the capture actually shows the
subject.

## Several angles

Repeat frame → capture → generate per requested angle, preserving the same material, lighting
and style description. Where the selected integration accepts multiple references, include the
approved image for appearance and the new capture for camera/layout, labeling both roles.
Check consistency visually; do not promise that a shared seed locks the design.

## Delivering the images

Every generated image is saved into the asset library under AIGC/图片 and its absolute path
comes back in the result. To put one into the Unreal project — as a reference board, a texture,
or a decal source — hand that path to `ue_content_import`. Do not go looking for the file with
shell commands; the path is already in the result.

## Failure handling

- **No image model configured** — the error names the exact place to fix it (preferences → AI →
  model source). Relay it; do not try another model id and do not retry.
- **Model rejected the reference** — some image models are text-to-image only. Say so and offer
  either a text-only generation or a different model; do not silently drop the reference.
- **Reference path rejected** — the tool needs an absolute path. Use the `path` from
  `ue_screenshot` verbatim rather than rebuilding it.
- **Empty or black capture** — the camera is not pointed at the subject, or the actors are
  hidden in the editor. That is a framing problem, not a generation problem. Re-frame and
  capture again before spending another generation.

## When there is no scene

`generate_image` works on its own for plain text-to-image — concept art, textures, mood boards,
UI sketches. No engine connection, no capture, no reference. The rest of this skill does not
apply.
