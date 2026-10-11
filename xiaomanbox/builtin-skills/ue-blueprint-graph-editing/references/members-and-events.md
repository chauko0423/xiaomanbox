# Blueprint members — variables, events, functions, parent class

Everything here is about the Blueprint's **structure**, not the logic inside a graph.
The main `SKILL.md` covers writing and reading graphs; this file covers the members
those graphs refer to.

Read this when the task involves any of:

- exposing a variable so a designer can tune it per instance
- "when the player walks in / clicks / hits it, do X"
- one blueprint reacting to something that happened in another
- changing a function's parameters, or deleting a function
- changing what a blueprint inherits from
- multiplayer replication on a variable
- a Run on Server / Multicast event (RPC)

## Contents

- Triggers, clicks, hits — bind the component event
- "A happens, B reacts" — use an event dispatcher, never a poll
- Cleaning up
- Functions can be changed after they are created
- Reparenting
- Networked variables
- Networked events (RPCs)

---

## Triggers, clicks, hits — bind the component event

"Open when the player walks in", "pick it up when clicked", "take damage on hit" — all of
these hang off a delegate on a **component**, not off Tick.

```json
{ "blueprint_path": "/Game/BP_Door", "component_name": "TriggerBox", "action": "list" }
```

List first. The names are easy to get wrong (`OnComponentBeginOverlap`, not
`OnBeginOverlap`) and they differ by component type. Then bind:

```json
{ "blueprint_path": "/Game/BP_Door", "component_name": "TriggerBox",
  "action": "add", "event_name": "OnComponentBeginOverlap" }
```

The response carries the new node's `node_id` **and all of its pins**. Feed that `node_id`
straight into `blueprint_apply_graph` connections — no need to re-read the graph to find
out what `then` or `OtherActor` are called.

Binding the same event twice returns the existing node (`reused: true`). That is
deliberate: two bound events compile fine and then both run.

**Never replace this with a distance check on Tick.** That is wrong on performance and on
correctness, and it is the shape this task collapses into when the binding step is missed.

## "A happens, B reacts" — use an event dispatcher, never a poll

When one blueprint needs to react to something that happens in another, the correct
Unreal answer is an event dispatcher: the sender broadcasts and does not care who
listens.

```json
{ "blueprint_path": "/Game/BP_Door", "action": "add", "name": "OnDoorOpened",
  "params": [{ "name": "Opener", "type": "object", "class": "Actor" }] }
```

Then wire it like any other node:

- broadcast — `class: "CallDispatcher"`, `member_name: "OnDoorOpened"`
- subscribe — `class: "BindEvent"`, `member_name: "OnDoorOpened"`
- unsubscribe — `class: "UnbindEvent"` / `class: "UnbindAllEvents"`

`member_name` is the dispatcher's own name. The editor titles these nodes
"Call On Door Opened" / "Bind Event to On Door Opened", but that is a title built from
the property name, not a function — there is no such entry in any function table. Those
title strings are accepted under `class: "Function"` for compatibility, but the four
classes above are the shorter path and give better errors.

A dispatcher on **another** class goes as `member_name: "BP_Chest.OnChestOpened"`; wire
that actor into the node's `Target` pin. Component events (`OnComponentBeginOverlap` and
friends) do not go here — use `blueprint_component_event`.

`action: "list"` returns the dispatchers a blueprint already has, with their parameter
signatures, so you can bind to one without guessing its arguments.

### The bind node needs a matching custom event

`BindEvent` has a red pin named **`Delegate`**. What goes into it is the custom event's
`OutputDelegate` pin — a custom event whose
signature matches the dispatcher — that is the callback that actually runs. Build it in
the same `blueprint_apply_graph` call:

```json
{ "id": "cb", "class": "CustomEvent", "member_name": "HandleDoorOpened",
  "params": [{ "name": "Opener", "type": "object", "object_class": "Actor" }] }
```

Copy the types and their order from the dispatcher's `params` (names may differ). A
no-parameter dispatcher takes a no-parameter event — omit `params` entirely.

`params` works on `CustomEvent` only. Function graphs get their parameters from
`blueprint_create_function` or `blueprint_function_signature`, and the engine's own
events (`class: "Event"`) have a fixed signature you cannot change; passing `params`
anywhere else is an error, not a silent no-op.

**Do not solve this with Tick.** Checking every frame whether the other actor changed
is wrong on both correctness and performance, and it is the shape this task collapses
into when the dispatcher step is skipped.

### Reading what a broadcast actually carried

Nothing outside the graph can subscribe to a dispatcher. Python's
`DelegateBase.bind_callable` fails on blueprint-declared dispatchers with *"Delegate
wrapper proxy class is null"* — the engine only generates Python proxies for delegate
signatures declared in C++, and a dispatcher you made in a blueprint has none. Do not
spend calls on that route.

To see a broadcast parameter at runtime, print it from inside the graph:

1. Add a `KismetSystemLibrary.PrintString` fed by the value, wired **after** the node
   that produces it — wired before, it prints the value from the previous run and you
   re-do the whole playtest for nothing. Put it in the same `blueprint_apply_graph`
   call that writes the logic; it costs no extra round trip.
2. Run `ue_playtest`. Its `print_strings` is exactly this output.
3. Remove the debug nodes by re-sending the graph without them and
   `clear_existing: true`. **Do not clean up with `ue_undo`** — undo replays whole
   transactions, not the two nodes you picked, so it also rolls back everything else
   that call did.

If the blueprint belongs to the user, say in your report that you added print nodes to
observe the value and took them out again.

## Cleaning up

`blueprint_remove_variable` deletes a member variable. It is irreversible and the
`Get`/`Set` nodes that referenced it become orphans, so run `blueprint_compile`
afterwards to see which graphs broke. Use it when you created the wrong variable —
leaving it behind means it shows up in every later `blueprint_describe` and reads
like intentional state.

## Functions can be changed after they are created

`blueprint_create_function` makes a function with no parameters. `blueprint_function_signature`
adds and removes them, and deletes the function outright:

```json
{ "blueprint_path": "/Game/BP_Door", "graph_name": "OpenDoor",
  "action": "add_param", "param": { "name": "Speed", "type": "float" } }
```

`direction: "out"` adds a return value instead, which requires the function to already have a
result node.

Changing a signature leaves **existing call sites wired to the old pins** — compile after,
and re-wire what broke. Same for `remove_function`: its call sites become orphans.

## Reparenting

`blueprint_set_parent_class` changes what a blueprint inherits from — "this should have been
a Character, not an Actor". Previously the only route was rebuilding the blueprint and
copying the graphs across, which is its own source of mistakes.

It **compiles automatically afterwards and returns the errors**, because reparenting usually
breaks nodes that referenced members of the old parent. Treat `compile_errors > 0` as a
failed operation even though the call succeeded: read `messages` and fix before continuing.

## Networked variables

`blueprint_set_variable_meta` takes `replication`:

- `Replicated` — the server's value is pushed to clients
- `RepNotify` — same, plus a callback when it changes. The callback name is fixed as
  `OnRep_<VariableName>`; you have to create that function yourself or compilation fails.

Leave it alone in single-player projects — replication on a variable nothing reads is
wasted bandwidth and a source of confusing bugs.

## Networked events (RPCs)

A Blueprint RPC is a `CustomEvent` with `replication` set — do it in `blueprint_apply_graph`,
don't ask the user to click the Details panel:

```json
{ "id": "req", "class": "CustomEvent", "member_name": "Server_RequestFire",
  "replication": "Server", "reliable": true }
```

- `Server` — Run on Server: a client asks the server to do something
- `Client` — Run on owning Client: the server tells one player's machine
- `Multicast` — the server tells everyone
- `reliable` only applies when `replication` is not `None`

A client's `Server` call is only delivered on an actor that client **owns** — its pawn,
PlayerController, or something they own. Put the request there, not on a level actor.

`blueprint_get_graph` returns `replication` / `reliable` on replicated events. To change an
existing event, delete the node and recreate it with the new `replication`, wiring back to
its neighbours by `node_id`.
