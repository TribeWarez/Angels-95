# AngelEd Architecture Refactor — Core / Subsystems / UI / Resources + EditorEventBus

**Status: R0–R6 DONE.** This file is the source of truth. Update the
progress log as items land; do not work from memory of a plan held in context.

**Action-field result in one line:** 58 `action*` fields → **3** (R2 took it to 4,
R6 converted the last convertible one). All 3 survivors are documented exceptions with
a reason. Nine of the 58 turned out to have no writer at all. Six **live** bugs were
found and fixed along the way — see the R2 and R6 sections.

This is the *deferred* follow-up to `Wiki/Editor-PropertyPanel-Refactor.md`. That
work fixed the editor's behaviour and deliberately did **not** restructure it,
because restructuring a 10,900-line pair of god-files while also hunting behaviour
bugs is how you ship two bugs at once. The defects are fixed; this document is
about maintainability.

## Why now, and why not before

The property-panel refactor was worth doing on its own merits and was kept
separate for one reason: **it could be verified**. Every change there was either
observable in a panel or pinned by a test. A layer split changes no behaviour at
all — its only output is "the next change is easier and safer" — so bundling it
with bug fixes would have made both unverifiable.

## The actual problem: 58 hand-rolled message channels

`EditorPanelState` carries **58 `action*` fields**. Each one is a hand-rolled
message: a panel's `WM_COMMAND` writes a field, `Main.cpp` polls all of them once
per frame, consumes, and resets. There is no envelope, no payload type, and no
record of who asked for what.

Three concrete failure modes, all of which are latent today:

1. **A stale index redirects the operation.** `actionWorldGraphDelete` is an
   index. If the selection moves between the click and the poll — which it can,
   because raylib mouse picking and the Win32 message pump both run inside the
   frame — the delete lands on a *different* entity. This exact hazard had to be
   defended by hand for portal delete in b88:
   > "Deleting the *selected* portal now clears the selection — `RemovePortal`
   > shifts every later portal down, so a retained target would have silently
   > edited a different portal."
   That is a bug class, not a missing null check, and an index in a loose field
   cannot express the fix.
2. **An unconsumed field fires later.** Nothing tracks whether a handler ran. A
   field set while the handler is disabled silently executes on a subsequent
   frame against whatever is selected then.
3. **There is no way to test any of it.** The channels only exist inside a Win32
   message handler and a raylib frame loop, so none of it is reachable from the
   headless suites. Every one of the 58 is verified only by clicking.

## Design

### Layers

```
AngelEd/Source/
├── Core/            frame loop, editor state, the bus, the dispatcher
│   ├── EditorState.hpp        selection, placement mode, gizmo, viewport flags
│   ├── EditorEventBus.hpp/.cpp  ← R1, raylib-free and Win32-free
│   └── EditorDispatcher.cpp   routes bus events to subsystems
├── Subsystems/      domain logic, no Win32, no raw raylib drawing
│   ├── Selection.{hpp,cpp}    pick / snap / gizmo targeting
│   ├── History.{hpp,cpp}      undo-redo snapshots
│   ├── Placement.{hpp,cpp}    spawn pawn / mesh / pickup / zone / pathnode / ...
│   ├── SurfaceOps.{hpp,cpp}   SetRenderableFace / ResetSurface / AutoConvex append
│   ├── LevelState.{hpp,cpp}   LevelMetadata + gametype + particles + skybox
│   └── AnimEditing.{hpp,cpp}  clip CRUD, keyframes, tool-scoped vertex undo
├── Resources/       asset discovery and packaging (shared by UI + Subsystems)
│   ├── AssetScope.{hpp,cpp}   the two-root (GameData)/(Packages) tree
│   ├── AssetScan.{hpp,cpp}    file/package enumeration per asset kind
│   └── PackageIO.{hpp,cpp}    PackIntoPackage / HotLoadPackage
└── UI/              Win32 panels. Widget code and nothing else.
    ├── UiCommon.hpp/.cpp      CreateLabel/Button/Ctrl, CHOOSECOLORW, file pickers
    ├── UiShell.cpp            class registration + create/destroy (unity TU)
    └── <Panel>.cpp            one per window, unity-included by UiShell.cpp
```

Dependency rule: **`UI` may call `Subsystems` and `Resources`; `Subsystems` may
call `Resources`; nothing calls back up.** `Core` calls down and owns the loop.
A panel that needs a domain operation posts an event; it does not reach into
`PawnSystem` itself.

### EditorEventBus

Raylib-free and Win32-free **on purpose** — that is what makes the central
invariant testable headlessly, the same discipline as `ZoneTypes.hpp`,
`GameType.hpp` and `SurfaceFlags.hpp`.

```cpp
namespace ed {
enum class Ev { SpawnPawn, SpawnMesh, SpawnPickup, ..., DeleteEntity, ... };

struct Event {
    Ev kind = Ev::None;
    std::variant<std::monostate, int, float, bool, std::string,
                 SpawnDesc, Transform, SelRef> payload;
};
}
```

Three properties that the current design cannot offer:

- **The payload is typed.** `std::variant` means the post site is checked by the
  compiler against the event kind, so a `SpawnZone` cannot be posted carrying a
  filename. The 58 loose fields have no such constraint — `actionSpawnZone` is an
  `int` and `actionSpawnPawn` is a `std::string`, and nothing stops them being
  confused.
- **A selection-sensitive event carries its target, by value.** `SelRef` is
  `{ SelType type; int index; }` captured **at post time**. The stale-index bug
  above becomes impossible by construction rather than by remembering a guard.
- **Post/drain is observable.** `posted()`, `dropped()` and a bounded queue make
  "the handler did not run" a testable condition instead of a mystery.

Draining stays deferred to one point per frame (inline dispatch from inside
`WM_COMMAND` would mutate the world while a panel is mid-layout). The safety comes
from the payload being complete, not from the timing.

### Build wiring — use a unity TU, deliberately

Splitting one 6,100-line `.cpp` into 14 would mean **14** new compile rules in
`AngelEd/Makefile` **and** 14 lines plus 14 link entries in
`.github/workflows/ci.yml`, which builds the editor by hand. That is the
"added in one place, CI breaks silently" trap from `AGENTS.md`, magnified 14x,
and the Makefile and CI list can drift silently.

So `UI/UiShell.cpp` is a single translation unit that `#include`s the per-panel
`.cpp` fragments — the same pattern already used for `raygui.c` in both build
files. **One** new object, **one** Makefile rule, **one** CI line. The trade is
that a fragment can rely on an include it does not state; that is a real cost and
it is accepted here because the alternative is a CI that breaks invisibly.

`Core/` and `Subsystems/` get real `.cpp` files, because those are the layers
whose *interfaces* matter and they are far fewer per layer.

## Phases

| Phase | Item | Status |
|---|---|---|
| R0 | this document | **done** |
| R1 | `EditorEventBus` + headless suite + `make test` in CI | **done** |
| R2 | convert the action fields, batch by batch - **done** | 58 |
| R3 | split `Win32Dialogs.cpp` into `UI/` (unity TU) | **done** |

| R4 | split `Main.cpp` into `Core/` + `Subsystems/` | **done** |
| R5 | `Subsystems/Placement` (close Invariant 1) + `AGENTS.md` | **done** |
| R6 | `EditorState` + `EditorDispatcher` + `SurfaceOps` + `LevelState` + `Resources/` + `actionApplyProperties` | **done** |
| F | promote `Subsystems/` to real translation units (`Core/` **excluded**) | pending |
| G | Linux build for AngelEd — **deferred by request** | pending |

R2 is split into batches so each is independently reviewable and each deletes its
fields as it goes:

| Batch | Events | Fields |
|---|---|---|
| B1 | placement / spawn — **done, and it was deletion not conversion** | 10 |
| B2 | world graph — **done** (level list deferred to B5, it is a world *name*, not a selection) | 8 |
| B3 | surface / CSG / portal / reload / convert — **done** | 8 |
| B4 | animation - **done** (11 events; Save/Refresh stay fields) | 12 |
| B5 | assets + level list - **done** | 20 |

## Invariants — do not regress

- **A panel posts an event; it never mutates the world directly.** If a `UI/`
  file calls `PawnSystem::Add*`, the layer rule is already broken.
- **One payload per event kind**, checked by `std::visit` at the handler. No
  `if (e.kind == Ev::X) reinterpret_cast<...>`.
- **A selection-sensitive event captures its target at post time.** Never read
  `g_sel` inside a handler for an event that names an index.
- **Every converted field is deleted in the same commit that stops writing it.**
  A field that is written but never read is worse than one that does not exist —
  that is how `ID_PP_PORTALBROWSE` and `ID_BTN_GT_PREVIEW` survived.
- **CI must build the editor from the same object list the Makefile uses.** R3
  is not finished until `ci.yml` and `AngelEd/Makefile` agree; the check is a
  diff of the two lists, not a build.

## Ruled out — do not re-investigate

- **Converting the whole thing at once.** 58 fields, 14 panels and 4,774 lines of
  `Main.cpp` in one change cannot be reviewed and cannot be bisected.
- **A generic `EventBus` with string topics.** The engine has no reflection and
  no build-time key checking, so a string bus would trade a compile error for a
  runtime typo — strictly worse than the `action*` fields.
- **Immediate (inline) dispatch from `WM_COMMAND`.** A panel is mid-layout when it
  posts; mutating the world underneath it is how you get a crash that only
  reproduces when you click fast.
- **Moving `LevelMetadata` out of `UI/`.** It is level *state* and belongs to
  `Subsystems/LevelState`; it only appears to live in `UI/` because the deleted
  Zone window used to mirror it into a global.

## Progress log

- **R0** — plan written after the b88 tag. Survey at that point: `Win32Dialogs.cpp`
  6,109 lines / 14 window procs, `Main.cpp` 4,774 lines, 58 `action*` fields, 59
  tracked files under `AngelEd/`.

### R1 notes

- **`EventBus` is a value type with a public constructor, plus `instance()`.** It
  started as a private-ctor singleton and the test could not compile. A bus you
  cannot instantiate is a bus you cannot test, and testability is the entire
  reason this type is raylib-free — so the constructor is public and `instance()`
  is only the production convenience.
- **`Ev::Count` is a sentinel, not an event**, and the suite asserts
  `Ev::Count == named + 1`. That is the anti-drift mechanism: add an `Ev` without
  adding it to `kEvents` in the test and the suite fails. My first attempt
  hardcoded "39 values" and was wrong (39 events + None = 40), which is exactly
  the kind of stale constant the sentinel removes.
- **A wrong-shape payload read logs and returns an inert value** rather than
  aborting. Crashing an editor frame is worse than a logged mismatch, and the
  alternative — `reinterpret_cast` on the variant — is the bug this design exists
  to prevent.
- **CI now runs `make test`.** It previously ran only `worldcheck`, which is why
  `test_ozls_writer` and `test_surface` sat unrun. Two details that matter:
  - `make test` invokes each suite as `-./suite`, so it continues past failures and
    its exit code is useless. The new step tees the log and greps
    `^[[:space:]]*FAIL` — one pattern covers every suite's format.
  - A **compile** failure is still fatal, because only the `./suite` invocation is
    `-`-prefixed; the compile rules are not. So the grep covers execution and make
    covers build, with no gap.
- **CI runs the suites in the Linux job only.** The Windows job builds the editor,
  which is where AngelEd compilation is actually verified; the bus itself is
  platform-independent.

### R2 B1 — all ten fields were unreachable, so the batch is a deletion

Auditing the writers of every `action*` field before converting any of them found
that **the entire placement batch was dead**. For all nine `actionSpawn*` fields
the only assignments anywhere in `AngelEd/Source/` were the handlers' own resets to
their defaults:

```
$ grep -n 'actionSpawn\w*\s*=[^=]' AngelEd/Source/*.cpp AngelEd/Source/*.hpp
Main.cpp:4661:    g_editorPanels.actionSpawnParticleEmitter = false;
Main.cpp:4672:    g_editorPanels.actionSpawnPathNode = false;
...
```

So ~110 lines of handlers in `Main.cpp` could never run. Placement in fact happened
**synchronously** in `SpawnSelectedPawnTreeItem`, which covered all nine types - pawn,
pickup, player start, emitter, zone, mesh, particle emitter, path node, wind zone -
using `spawnPos`.

> **CORRECTION (made in R5).** This section originally added: *"The dead handlers
> would also have placed at `MainCamera.target` instead of the ghost position, so they
> were not merely redundant but **wrong**."* **That was false.**
> `g_editorPanels.spawnPos` is assigned from `OTEditor.MainCamera.target` every frame,
> so there is no separate ghost position - the camera aim point **is** the placement
> point. The dead handlers would have been behaviourally **identical**, not wrong.
>
> It mattered, because that sentence was the strongest stated reason to keep placement
> synchronous. With it gone, synchronous placement had no position-fidelity argument
> left - which is what made routing placement through the bus defensible at all. R5
> therefore deferred placement by one frame deliberately, having established there was
> no second position to lose.

**This is why the batch is an audit first and a conversion second.** The plan
assumed 58 live channels; it is closer to 49. `Ev::Spawn*` still earns its place —
`SpawnSelectedPawnTreeItem` reaching into `PawnSystem` directly is exactly the
UI→Subsystems dependency R3/R4 removes — but its first real users arrive there,
not here.

### B1 also caught a regression I introduced in b88

`actionApplyLight` was the **only** writer of the legacy light apply handler in
`Main.cpp`, and `LightPropsProc` was that writer. Deleting the Light Properties
window in b88 therefore orphaned ~27 lines. It was not caught at the time because
"a handler nothing can reach" produces no build error and no test failure.

It was also already a strict **subset** of the panel's `tgtType == SelType::LIGHT`
branch (`Main.cpp:3940`), which additionally handles position, name and target and
clamps `type`/`effect` defensively. Removed, along with its 12 `light*` backing
fields — which nothing else referenced.

> **Lesson worth keeping:** removing a producer must include a check that its
> consumer went with it. `grep` for the *handler*, not just the field.

Action fields: **58 → 49** (9 `actionSpawn*` + `actionApplyLight` removed).

### R2 B2 — the first real bus use, and it fixed a live bug

Nine fields became four events (`SelectEntity`, `ApplyProperties`, `DeleteEntity`,
`DuplicateEntity`), and the conversion surfaced a defect that was live in the
editor right now:

```cpp
g_editorPanels.actionWorldGraphDelete = e.selIndex;          // writer
...
if (g_editorPanels.actionWorldGraphDelete >= 0) {
    if (g_sel.type != SelType::NONE) DeleteSelectedEntity();  // reads g_sel, NOT the index
```

The index was written and never read. Delete and Duplicate acted on whatever
`g_sel` was, which was correct **only** because the select field happened to be
written in the same `WM_NOTIFY` — an ordering coincidence, not a guarantee. The
same batch found:

- **Two index spaces in three near-identically named fields.**
  `actionWorldGraphProperties` held a ListView **row** index while
  `actionWorldGraphDelete`/`Dup` held an **entity** index.
- **`actionWorldGraphProperties` was never read at all.** Its handler just called
  `OpenPropertiesForSelection()`, i.e. acted on `g_sel` too.
- **One message spread over five fields.** `actionSelectFromGraph` plus its
  `Type`/`Name`/`Pos[3]` satellites were written together and read together with
  nothing coupling them, so a handler could read a new index against the previous
  frame's type and position without noticing. Now one `ed::Selection`.

`AdoptSelection()` takes the target from the event rather than from `g_sel`, which
makes the ordering irrelevant instead of merely currently-fine.

#### Decisions worth recording

- **`ToBusKind(SelType)` is a switch, not a `static_cast`.** The two enums have
  identical values so a cast would work — and would keep compiling after someone
  adds a 15th `SelType`, silently mis-dispatching every event for it. A switch with
  no `else` turns that omission into a compile error. It lives in `SelType.hpp`,
  not in the bus, because that is the only place both enums are visible.
- **The drain happens BEFORE dispatch**, and into a file-static vector. A handler
  that opens a panel can post further events; draining while iterating the live
  queue would dispatch those in the same frame.
- **`e.selType < 0` is mapped to `SelKind::None`.** `sel::DEF_ONLY` (-1) is a
  Script-Manager sentinel, not a `SelType`, and it means "no live target".
  `AdoptSelection` then rejects it, so Properties/Delete on such a row does nothing
  rather than acting on the previous selection.
- **Unhandled events are counted** (`g_editorUnhandledEvents`), not ignored, so a
  batch that posts an event nobody handles shows up as a counter rather than a
  click that silently does nothing. Should be 0 once R2 completes.

`actionLevelListOpen` / `actionLevelListLink` were **not** converted here: they
carry a world *name*, not a selection, and belong with the other asset/world
operations in B5.

Action fields: **49 → 40**. `test_editorbus` 46/46.

### R2 B3 — surface, CSG, portal, reload, convert (8 fields)

Converted to `ApplySurface`, `ResetSurface` (both a new `ed::SurfaceEdit`),
`CsgPlace`/`CsgCommit` (a new `ed::CsgIntent`), `DeletePortal`, `ReloadMesh`,
`ConvertToAnimated`.

Two more live defects fixed on the way:

- **`Reload` could reset an unrelated object's cache.** The handler read
  `g_sel.index` and only checked `g_sel.type == SelType::MESH` *after* indexing, so
  a Reload pressed while something else was selected called `GetMeshObject()` with
  a foreign index. It now carries a `SelRef` and guards on the kind as well.
- **`Convert to Animated` converted whatever was selected at drain time** rather
  than the mesh whose button was pressed. Same shape of bug as the WorldGraph one.

#### Decisions worth recording

- **`actionApplyProperties` was left as a field here, and converted in R6.** At this
  point there was no target to capture: the properties panel owned `propsTargetType` /
  `propsTargetIndex` itself and was both the only writer and the only reader, so an
  event would add a queue hop without adding a payload that could go stale. It was
  scheduled to convert "in **R4**, when the panel stops owning the target directly" —
  R4 did not do it, and R6 did. See the R6 section: the values stay live-read on
  purpose, and only the target travels.
- **`SurfaceEdit` carries the mask, and `ResetSurface` posts a mask too.** The
  viewport's `IDM_SURFACE_RESET` route previously set a bare bool, so it reset only
  the last-clicked face and ignored the Shift multi-selection the dialog title
  advertises ("(N Selected)"). It now rebuilds the mask from
  `g_selectedSurfaces`, which is what both routes should have used.
- **Surface events are drained in their own pass** (`g_editorSurfaceEvents`), not
  the shared `g_editorFrameEvents`. Those handlers are the ones moving to
  `Subsystems/SurfaceOps` in R4, and a separate pass means that move does not have
  to reorder the selection dispatch.
- **`CsgPlace` and `CsgCommit` stay separate kinds.** "Arm a primitive" resets the
  ghost to a default box at the camera; "commit the ghost" reads the ghost as it is.
  One merged event would either lose the reset or commit a brush nobody positioned.

#### The `Ev::Count` sentinel earned its keep

Adding `DeletePortal` made `test_editorbus` fail immediately:

```
FAIL  Ev::Count == named events + None (every Ev has a row above)
```

That is the anti-drift mechanism from R1 doing exactly its job on the first real
use — a new event kind with no test row is a **failing suite**, not an untested
path. It is the direct successor to `ID_PP_PORTALBROWSE` and `ID_BTN_GT_PREVIEW`,
which sat declared, created and never handled for a long time.

Action fields: **40 → 32**. `test_editorbus` 55/55.

#### Process note: a brace-balance check is worth having

Deleting the CSG handlers by line range left one `}` behind (the block's close was
at 4769, the cut ran to 4768), which closed the render loop early. The symptom was
a cascade of ~15 unrelated "expected constructor" errors 1,700 lines later. A
five-line brace-count loop after every range deletion found it immediately, and is
now part of the routine for the remaining batches.

### R2 B4 — animation (11 of 13 fields)

Eleven commands became `ed::Ev::Anim*` carrying a new `ed::AnimIntent` (mesh id,
clip name, playhead, fps/loop). Another live staleness bug fixed:

> **Delete Clip deleted a different clip than the one clicked.** The handler read
> `animClipName` from live panel state at drain time. A clip list re-selects on
> every click, so this was easy to trigger — click Delete, and the clip that goes
> is whatever the list holds when the frame drains. The same applied to Add/Delete
> Key (`animTime`) and ApplyClipMeta (`animFps`/`animLoop`).

Two fields **deliberately stayed**, and this is the batch's main judgement call:

| Field | Why it is not an event |
|---|---|
| `actionAnimSave` | An **intra-frame chaining signal**. NewClip, DeleteClip, AddKey, DeleteKey and ApplyClipMeta all set it from inside their own handlers, and the file write consumes it later in the *same* frame. Queueing it would delay every save by a frame for no benefit — and the write is part of the command completing, not an independent user action. |
| `actionAnimRefresh` | A **dirty flag**. Set by 7 sites including handlers; consumed by `RefreshAnimPanel()`. It means "the panel needs re-reading", not "the user did something". |

Treating these as events would be the "convert everything" reflex applied to two
signals that are not messages. `test_editorbus` asserts their **absence** from the
enum, so a future "fix" for that asymmetry has to be deliberate.

#### The mesh-id guard is a stopgap, and says so

`AnimIntent` carries `meshId`, but `AnimTarget()`, `AnimSnapshotPush()`, `AnimUndo`
and the snapshot stack all resolve the **current** target internally, so the id
cannot yet be threaded through them. Rather than leave that silent,
`ApplyAnimIntents` detects the mismatch and skips + logs the command:

```
EditorLog("Anim: command skipped, target moved (event=%d was for %d, now %d)", ...)
```

`Subsystems/AnimEditing` in R4 threads the id properly; until then a moved target
is a visible no-op instead of an edit to the wrong mesh.

- **Ctrl+Z / Ctrl+Y now posts the same events the toolbar does**, so keyboard undo
  is not a second code path with its own staleness behaviour.
- **`PostAnimIntent()` reads fps/loop off the controls**, not the mirrored panel
  fields, so an intent cannot be built from values the panel has already moved past.
- **All eleven handlers moved out of `main()`** into `ApplyAnimIntents()` — the
  first real step toward `Subsystems/AnimEditing`, and the reason animation drains
  into its own `g_editorAnimEvents` pass.

Action fields: **32 → 23**. `test_editorbus` 63/63.

### R2 B5 — assets, level list (18 fields) — and R2 is complete

New payloads: `PlacementRequest`, `TextureApply`, `HeightmapDesc`. Events:
`BeginPlacement`, `ApplyTextureToModel`, `PlaySound`, `StopSoundPreview`,
`GenerateHeightmap`, `RefreshModelBrowser`, `OpenWorld`, `LinkWorld`.

One more live staleness bug:

> **The Pickups panel could place the wrong pickup.** It sent a **list index** and
> the main loop resolved it with `LegacyPickupType(idx)` at drain time. A panel
> rebuild between the click and the frame placed a different def. The panel now
> resolves the name itself — the button IDs are minted from the same
> `FindByType(EntityType::PICKUP)` order the panel builds its list from, so the
> lookup is exact — and the **name** travels in the event.

The six `actionHm*`/`actionHeightmap*` fields collapse into one `HeightmapDesc`;
four of them were read at drain time, so a panel edit between click and frame
generated the heightmap from values the panel had already moved past.

#### Three fields renamed off the `action*` prefix

The prefix was *lying* about these — they are live state the loop re-reads every
frame, not messages:

| Was | Now | Why |
|---|---|---|
| `actionSoundVolume` | `previewSoundVolume` | Volume slider; re-applied every frame while the preview plays |
| `actionSoundLoop` | `previewSoundLoop` | Same |
| `actionSoundCategory` | `previewSoundCategory` | Same |
| `actionRefreshBrowser` | `refreshModelBrowser` | Dirty flag, exactly like `actionAnimRefresh` |

Only the **path** became an event. That split matters: a category switch between
the click and the frame would otherwise preview from the wrong list, which is
precisely why the path travels and the sliders do not.

## R2 result: 58 fields → 4 (now 3 — R6 converted the fourth)

| Remaining field | Why it is not an event |
|---|---|
| ~~`actionApplyProperties`~~ | **Converted in R6** → `ed::Ev::ApplyProperties` carrying a `SelRef`. The row below was correct *for R2* and was out of date by one phase: the precondition it named was met in R6. |
| ~~`actionApplyTextureToSel`~~ | **Retired.** It was the last "modal `TrackPopupMenu`" exception, and the argument was sound only while there was one writer and one drain. The drain was a SECOND implementation of texture apply that never learned about `Mesh.*`, so "Apply Texture" silently did nothing on a placed prop. The context menu now posts `ed::Ev::ApplyTextureToSelection` (carrying a `faceMask`) exactly like the Texture Manager, and `EditorDispatcher` is the one implementation. 58 → 4 → 3 → **2**. |
| `actionAnimSave` | **Intra-frame chaining signal.** The anim handlers set it themselves; the file write consumes it in the *same* frame. Queueing delays every save a frame. |
| `actionAnimRefresh` | **Dirty flag** consumed by `RefreshAnimPanel()`. Means "re-read the panel", not "the user did something". |

`58 → 4`. Nine of the original 58 turned out to have **no writer at all** (B1), so
the number of channels that actually needed converting was ~45, not 58. Nine fields
that were never messages were renamed to stop pretending otherwise.

**Live bugs found across R2: five.** WorldGraph Delete/Duplicate read `g_sel`
instead of their own index; Reload indexed `GetMeshObject()` with a foreign index;
Convert-to-Animated converted the wrong mesh; the Pickups panel resolved an index
at drain time; and the b88 Light-window deletion orphaned its apply handler. All
fixed.

## R4 outcome: Main.cpp

Split into the same unity-TU shape as R3, so `Main.o` stays `Main.o` and **no build
wiring changed at all** — the Makefile and the hand-rolled CI build were untouched.

```
AngelEd/Source/
  Main.cpp                 the TU: preamble + 11 fragment includes + main()
  Core/EditorShell.hpp     includes, shared editor globals, local types, fwd decls
  Core/EditorShell.cpp     view presets, panel toggle shims, world dir, scene reset
  Core/EditorLog.cpp       AngelEd.log appender
  Core/EditorMenus.cpp     native menu bar
  Subsystems/Selection.cpp          raycast, surface-face pick, gizmo snap, adopt,
                                         delete, duplicate
  Subsystems/WorldIO.cpp            LoadWorldDocument + name helpers
  Subsystems/OzoneExport.cpp        ExportToOzone, Save, File menu, texture apply
  Subsystems/WorldGraphBridge.cpp   UI accessor surface + Win32 menu dispatch
  Subsystems/AnimEditing.cpp        vertex-keyframe editing + anim event handlers
  Subsystems/EntityOps.cpp          AutoConvex proxy append, brush CSG commit
  Subsystems/History.cpp            OZONE-snapshot undo/redo + ApplyMapProperties
  Subsystems/Placement.cpp          Spawn* handlers: the world mutation
  EditorEventBus.{hpp,cpp}          shared event contract
  Editor.hpp, EditorIcons.{hpp,cpp}, SelType.hpp
  UI/                        see R3
```

### Why a unity TU rather than 11 real TUs

Almost every helper in the old file is `static`, and they share file-scope globals
(`g_sel`, `g_placeMode`, `g_histUndo`, `g_selectedSurfaces`, `g_lastPreviewSel`,
`g_editorPanels`, ...). Promoting them to real translation units means externalising
~40 symbols and hoisting that shared state into a header — which changes **linkage
and initialise order**. That is the one class of change in this whole refactor that
`make test` cannot see, because AngelEd has no headless harness. A unity split keeps
the declaration sequence byte-identical and the risk at zero.

**Promotion to real TUs is the obvious R5** and is now unblocked: the cut points and
the shared-state inventory are both known.

### The include order is load-bearing

The fragments carry no forward declarations of their own, so several statics are
declared in the fragment *above* their user: `g_editorLog` lives at the top of
`EntityOps.cpp` and is used in `EditorLog.cpp`; `g_originalWndProc` and
`EditorWndProc` are declared in `WorldGraphBridge.cpp` and used in `EditorMenus.cpp`.
Grouping the includes by layer ("core before subsystems") reads better and does not
compile — it was tried, produced four errors, and `Main.cpp` now says so at the
include list.

### The one deliberate back-edge

`UI/UiCommon.hpp` includes `Core/EditorEventBus.hpp`. That is not an accident and not
a violation to fix by moving the file: UI uses the `ed::` vocabulary throughout
(`Ev`, `Selection`, `SelKind`, `SelRef`, `SurfaceEdit`, `CsgIntent`, `AnimIntent`,
`PlacementRequest`, `TextureApply`, `HeightmapDesc`). The bus is the **contract all
three layers speak** — producers publish, `Main.cpp` drains, UI dispatches — with the
queue in its `.cpp`. The layer rule is therefore stated as: UI may reach
`EditorEventBus.hpp` and nothing else upward.

### Verification

Line-multiset comparison against the pre-split file: 237 lines added (banners, the
include-order comment, the re-pathed includes) and exactly **16** removed, every one
of them an `#include` whose path changed because the preamble moved from `Source/` to
`Source/Core/`. Zero code lines lost or altered.

> The comparison needs `-Encoding UTF8` on both sides. PowerShell 5.1's `Get-Content`
> otherwise defaults to the ANSI codepage and mangles every em-dash in the file's
> comments, which shows up as ~40 phantom "removed" lines and looks like data loss.

## R5 outcome: Subsystems/Placement (Invariant 1 closed)

R3 and R4 were file surgery. R5 was the first change in this document that alters
behaviour, and it exists to close the invariant the document opens with:

> **A panel posts an event; it never mutates the world directly.** If a `UI/` file
> calls `PawnSystem::Add*`, the layer rule is already broken.

It was broken. `SpawnSelectedPawnTreeItem` in `UI/Panels/PawnPanel.cpp` called
`PawnSystem::Add*` eight times and `ZoneManager::AddZone` once, from inside a
`WM_COMMAND` handler.

### What changed

| | Before | After |
|---|---|---|
| Panel does | validate + **mutate** | validate + **post** |
| Position source | `g_editorPanels.spawnPos` read at click | `SpawnDesc::at`, captured at post |
| Mesh path | resolved by index at click | resolved by the panel, path travels |
| Undo | **none** - placement was the one mutation with no `HistoryPush` | `HistoryPush()` first |

`SpawnDesc` gained `Transform at` (position, plus zone/wind-zone half-extents and
mesh/playerstart yaw), `bool skeletal` and `int lightType`; `Ev::SpawnLight` was
added for the three light tags, which had no event at all. `Transform` had to move
**above** `SpawnDesc` to be nested.

### Two things worth recording

- **The `Spawn*` seam already existed and was fully tested - with zero users.** All
  nine kinds and `SpawnDesc` were exercised only by `tests/EditorEventBus.test.cpp`.
  The dead `actionSpawn*` fields had been deleted in B1 and the replacement was left
  to land here, which is exactly what the B1 note said would happen.
- **`ev.spawn()` is the shape check.** The first draft of `ApplyPlacementSpawns` used
  `std::holds_alternative<ed::SpawnDesc>(ev.payload())` - which does not compile, the
  member is `data`. But the better outcome is that it is unnecessary: `spawn()` already
  logs and returns an inert `SpawnDesc` on a shape mismatch, which is the documented
  anti-`reinterpret_cast` mechanism. A second, weaker guard doing the same job would be
  the wrong fix.

### Behaviour changes a reviewer must know about

1. **Placement defers by one frame.** It used to happen inside `WM_COMMAND`; it now
   happens at the next drain. Safe because the panel validates synchronously (its
   failure `MessageBox` needs the return value, and a queued event cannot report back)
   and because nothing in the handler reads live panel or selection state.
2. **Placement is now undoable.** This is a fix, not a regression: previously a placed
   entity could not be removed with Ctrl+Z while delete, duplicate and surface edit all
   could.

### Verified

- `Spawn*` events with no handler increment `g_editorUnplacedEvents` rather than being
  ignored - the failure mode that kept the nine `actionSpawn*` fields invisible.
- **Invariant 1 grep now passes:** the only `ps.Spawn` left in `UI/` is inside a comment
  describing the old code. `UI/` still calls twelve `PawnSystem` accessors, all reads
  (`GetDefs`, `GetMeshObject`, `GetPickups`, ...), which the invariant permits.
- `make test` green including `test_editorbus` 77/77 (was 73/73).
- The `Ev::Count` sentinel failed the suite the moment `SpawnLight` was added, before
  its row existed. Working as designed.

### Not done, and still outstanding

- **Manual passes: DONE for R3-R8.** The user exercised the editor by hand after each
  round: R3-R5 (placement), then R6-R8, where R6 deferred the properties apply by a
  frame, R7 changed how both asset browsers enumerate, and R8 fixed brush selection.
  **This is the load-bearing check for the whole document.** A layer split changes no
  behaviour and so has *no* headless signal; the line-multiset diff proves the text is
  unchanged, not that the editor still works. Both matter, and the second is the one
  that actually caught anything.
- **What a manual pass caught that nothing else could:** both of R8's reported bugs
  (Delete crashing, brushes reappearing on click) were PRE-EXISTING - the delete branch
  is byte-identical at `b89` - and had been sitting in `AngelEd.log` as a churn of
  `Brush idx=102, 15, 14, 13, 11, 10, 9, 7` for one point on screen. No suite covers
  picking and there is no honest way to add one: the defect was two index spaces behind
  one `SelType`, which is only observable by clicking.
- ~~**`Resources/` layer: never created.**~~ **DONE in R6** — `AssetScope` and
  `PackageIO` are real translation units with headers of their own, and
  `tests/AssetScope.test.cpp` (45 checks) is the first automated coverage the asset
  browser has ever had. **`AssetScan` is the one exception** and is deliberately not
  done: `ScanFilesAndPackages` takes a `ResourceEntry`, which holds an `HBITMAP`, and
  `ScanModelBrowserFiles` only ever fills `g_editorPanels.modelEntries` and then
  `SendMessage`s the panel. Moving either as-is would put a Win32 type or editor-UI
  state inside `Resources/`. The honest version extracts one enumeration primitive
  returning `AssetScopeItem` and leaves the state write and refresh in the panel — a
  refactor with behaviour, so it needs its own commit and its own tests.
- ~~**`Core/EditorDispatcher.cpp` and `Core/EditorState.hpp`: never created.**~~
  **DONE in R6.**
- ~~**`Subsystems/SurfaceOps` and `Subsystems/LevelState` were folded.**~~ **DONE in
  R6.** Note the plan was wrong about *where* they were folded: surface code was never
  in `EntityOps.cpp` — that file has never contained any.
- ~~**`LevelMetadata` is still declared in `UI/UiPanels.hpp`.**~~ **DONE in R6**, along
  with `ParticleType`, which was only in a UI header because `LevelMetadata` was. Its
  *accessor* had been implemented inside `UI/Panels/LevelState.cpp` — subsystem state
  living in a UI panel.
- ~~**`actionApplyProperties` is still a field.**~~ **DONE in R6.** 58 → **3**, and
  **`actionApplyTextureToSel` has since been retired** (it had become a second
  implementation of texture apply that missed `Mesh.*`), so the count is 2.
- **`actionAnimSave` and `actionAnimRefresh` are the two survivors.** Both are
  intra-frame signals rather than user messages, and their reasoning has not changed.
- **Promotion of `Subsystems/` to real translation units** (phase F, `Core/` excluded).
- **Linux build for AngelEd** (phase G) — deferred by request. Scope already sized: 2
  missing UI stubs, ~6 guard blocks, delete `AngelEd/Makefile:38`'s `$(error)`, add
  AngelEd to the Linux CI job. R6 *reduces* this cost — `Resources/` moved shared
  logic out of the Win32-only UI translation unit into portable TUs.

## Verification

- `make MODE=debug OTENGINE AngelServ AngelMaster ozpack` and
  `make -C AngelEd MODE=debug` clean at every phase.
- `make test` green — **including `test_editorbus`, which does not exist until R1.**
- **The two AngelEd object lists were diffed, as required — and they differ by exactly
  one entry, legitimately.** `ci.yml` links `OTCustom_stub.o`; `AngelEd/Makefile`'s
  `EDITOR_OBJS` has no such object because the Makefile builds a real shared
  `Custom.so` and links `-l:Custom.so` instead. Both compile the same stub body. This
  is pre-existing and correct, but it means the invariant "the two lists must be
  equal" is **imprecise as written** — a literal diff will always show this one entry
  and will read as a regression to whoever checks next. Compare with this exception
  named, not by equality.
- **Manual pass: DONE.** Every menu entry, toolbar button, panel context menu and drag
  was exercised in the editor after R3/R4/R5; the user confirmed placement works. This
  was the main risk in the document, because a layer split changes no behaviour and so
  has **no** headless signal at all — the line-multiset diff proves the text is
  unchanged, not that the editor still works. That gap is now closed by hand rather
  than by machine.


## R6 outcome: the rest of the designed layout

Five pieces the Design section specified and R3/R4 did not deliver. `Main.cpp` went
from 5,039 lines (start of R4) to **2,184**.

### The layer tree now matches the design

```
AngelEd/Source/
  Main.cpp                     the TU: preamble + includes + main()
  Core/EditorShell.hpp         the include list, and nothing else
  Core/EditorState.hpp         menu IDs, event buffers, selection / mouse / gizmo state
  Core/EditorDispatcher.cpp    DispatchFrameEvents() - the 18 frame-pass kinds
  Core/EditorLog.cpp  Core/EditorShell.cpp  Core/EditorMenus.cpp
  Core/EditorEventBus.{hpp,cpp}
  Subsystems/Selection.cpp  EntityOps.cpp  SurfaceOps.cpp  WorldIO.cpp
  Subsystems/OzoneExport.cpp  WorldGraphBridge.cpp  AnimEditing.cpp
  Subsystems/History.cpp  LevelState.{cpp,hpp}  PropsApply.cpp  Placement.cpp
  Resources/AssetScope.{hpp,cpp}  PackageIO.{hpp,cpp}
  UI/UiCommon.hpp  UiPanels.hpp  UiShell.cpp  Panels/*.cpp
```

### What actually justified each move

- **`EditorShell.hpp` was three things wearing one filename.** The include preamble, the
  editor state, *and* thirteen `RaycastTest*` picking functions whose only caller is
  `Selection.cpp:38-50`. Picking logic sitting in a state header is the whole reason
  that file was 420 lines and unreadable.
- **The 240-line dispatch switch left the render loop.** `main()` now reads as a frame
  loop: drain, dispatch, four more drains, apply.
- **`SurfaceOps` is the name the plan always specified.** The plan claimed it already
  existed folded into `EntityOps.cpp`. It did not — `EntityOps.cpp` holds only
  `AppendAutoConvexForSelection` and `CommitBrushRenderable`, and has never contained
  any surface code. The plan was wrong about where the code was, not just about its name.
- **`LevelMetadata` was subsystem state implemented inside a UI panel.** Its accessor
  lived in `UI/Panels/LevelState.cpp`; its declaration in `UI/UiPanels.hpp`. The plan's
  "Ruled out" section had this right years before the code did.
- **`Resources/` shared code lived inside its consumers.** `BuildAssetScope`,
  `PackIntoPackage` and `HotLoadPackage` were all implemented in
  `UI/Panels/TexturePanel.cpp` while `UI/Panels/ModelPanel.cpp` called them. They were
  reachable only because the UI layer is one translation unit — the moment it became
  real objects, `ModelPanel.cpp` would have stopped compiling. A shared contract with no
  header is not a contract.

### The one piece this phase refused to do honestly

`Resources/AssetScan`. `ScanFilesAndPackages` takes a `ResourceEntry`, which holds an
`HBITMAP thumbnail`; `ScanModelBrowserFiles` returns nothing and only clears, fills and
dedups `g_editorPanels.modelEntries` before `SendMessage`ing the panel. Moving either
wholesale would put a Win32 type, or editor UI state, inside `Resources/`.

The honest version extracts **one** enumeration primitive returning `AssetScopeItem` —
which is also what `BuildAssetScope` consumes, so both browsers share it — and leaves
the state write and the panel refresh in the panel. That is a refactor with behaviour in
it, not a pure move, so it does not get the line-multiset verification; it needs its own
commit and its own tests.

### `actionApplyProperties` → `Ev::ApplyProperties`, and why only the target travels

The handler read **87** distinct `g_editorPanels.prop*` fields, of which about six apply
to any given `SelType`. A capturing payload would snapshot 87 to write 6. So the values
stay live-read and the event carries the `SelRef` only.

That is safe rather than a half-finished conversion, because the staleness this refactor
kills is an *index* that ends up naming a different entity than the one clicked. It
cannot happen to a value the user just typed: the panel is not modal, but between the
Apply click and the drain — the same frame — nothing pumps input.

What it does buy is the part that was genuinely wrong. The handler used to read
`propsTargetType`/`propsTargetIndex` from live panel state, so whichever row the panel
happened to be showing at drain time decided what got written.

`ToSelType(ed::SelKind)` was added as the reverse of `ToBusKind` — also an exhaustive
switch, not a `static_cast`, for the reason already recorded on the forward direction.

`propsTargetHasRotation` stays a live read at three sites. It is a per-*type* capability
flag ("does this kind have a yaw"), not a target; conflating them would reintroduce the
bug where an Apply silently reset `playerstart` yaw to 0.

**Action fields: 58 → 4 → 3 → 2.** Two survivors remain, and their reasoning is
unchanged: `actionAnimSave` (intra-frame chaining) and `actionAnimRefresh` (dirty
flag). `actionApplyTextureToSel` was retired once its "modal `TrackPopupMenu`"
argument stopped paying for itself: the drain was a second copy of the apply and
had already drifted (no `Mesh.*` branch), so the menu now posts the same
`ed::Ev::ApplyTextureToSelection` the Texture Manager does.

### Two build-wiring entries, deliberately

`Resources/` is the one place in this refactor where extra wiring was accepted rather
than avoided — 3 Makefile rules, 3 `ci.yml` entries. Everywhere else a unity fragment
keeps the object count at one. The exception is justified because `Resources/` is a
genuine two-consumer interface, and its whole defect was having no header.

The object lists were diffed as this document requires: Makefile 36, `ci.yml` 37,
agreeing on both new objects. The single difference is `OTCustom_stub.o`, which is
intentional (CI links a stub where the Makefile links `-l:Custom.so`) and is now named
in the Makefile comment so the next person does not read it as a regression.

### Mistakes made in this phase, since the value of recording them is now established

- **A range-delete mangled `main()`**, twice: once from an off-by-one (the frame
  `for` is at 1662 and closes at 1902, not 1663–1903), and once from a miscomputed
  splice range. Both were reverted with `git checkout` and redone from exact
  brace-balanced ranges.
- **The first generated `EditorDispatcher.cpp` had a duplicated `for`** and balance +1,
  which surfaced as a parse error in a file three includes later. Both extractions now
  assert `balance == 0` before writing.
- **`git checkout -- AngelEd/Makefile`, intended to undo a temporary probe rule,
  reverted the uncommitted `Resources/` wiring too.** Re-applied and rebuilt to prove it.
- **`PostApplyProperties` was inserted below its three call sites** — the unity-build
  property again: fragments carry no forward declarations of their own.
- **The first `test_assetscope` run was 43/45**, both failures a bug in my own test
  helper (a synthetic parent node leaked `"(GameData)/"` into every expected path). The
  code was right; the test was wrong.

Every one was caught by verifying after the fact rather than assuming. That is the only
reason any of them cost minutes instead of a bad commit — and the reason a reviewer can
read these entries as evidence the process works, not as an excuse.

## Working directory: every asset path is relative and unanchored

`EnsureProjectRoot()` (`Core/EditorShell.cpp`) is the **first** statement in `main()`,
before `g_config.Load`, before `PackageAssetLoader::Init`, before the world load. Three
rules, in order:

1. cwd already contains both `GameData/` and `System/`;
2. cwd's final component is `system` (case-insensitive) and its parent is a root - this is
   what the run scripts set up, and is the *only* case the old code handled;
3. walk up to four levels from the executable (`GetModuleFileNameA`).

It never fails hard - the editor can still open an explicitly-named world from a
GameData-less layout - but it logs the directory it settled on, and on failure logs the
cwd it tried.

**Why the generalisation was needed.** A real session reported two errors that had no
visible connection:

```
WARN: GameData/Global/PawnDefs not found, using hardcoded defaults
Skybox: could not load 'GameData/Global/sky/sky_noon.dds'
```

Both are downstream of the cwd. Almost every asset path in the editor is a relative
literal - `GameData`, `System/AngelEd.ini`, `System/Data` - so a wrong launch directory
does not fail loudly, it fails *selectively and confusingly*. The old heuristic handled
only the directory the run scripts happen to use, so a shortcut with a different "Start
in", a copy of the tree elsewhere, or a debugger all landed in the broken state.

`PackageAssetLoader::Init`'s missing-`System/Data` branch was a bare `return`. That is
the line that made this undiagnosable: every packaged asset vanished with nothing in the
log. It is now an `OZ_WARN` naming both the path and the cwd.

Verified by launching the built editor from `System/` and from an unrelated temp
directory: both recover, log `Project root: ... (from System/)` / `(from exe)`, load all
four pawn defs, and leave no stray log in the wrong cwd.

**Do not reintroduce a path that exists in only one layout.** The no-argument default
world was `"../GameData/World.ozone"`, which resolves in no layout at all: worlds live
at `GameData/Worlds/<Name>/World.ozone`, and the path also contradicted the
`System/AngelEd.ini` load eight lines below it. `FirstAvailableWorld()` takes the
sorted-first world that actually has one, so the choice is deterministic rather than
dependent on directory-iteration order.

## WorldGraph Explorer: derived list, dirty flag

The Explorer list is **derived** from `PawnSystem` / `ZoneManager` / `OzoneLoader`, and it
was built exactly once - in `WM_CREATE`, from `CreateAllEditorWindows`, which runs
**before** the world is loaded and before PawnDefs register. Opening it on a fresh
session showed one `Map (level)` row and stayed that way until `Refresh` was pressed.

Worse, each row's `lParam` is a `selIndex` **into the very vector the row was built from**.
A delete or duplicate shifts every later row, so a second right-click acts on the wrong
entity until the next manual refresh. The single auto-refresh that existed sat inside the
props-apply block: it fired for *every* Apply regardless of what changed, and missed every
other mutation.

The rule now:

- `MarkWorldGraphDirty()` (in `Core/EditorPanelState.hpp`) is set at the choke points -
  each event drain **gated on non-empty**, the Enter-key placement path that bypasses the
  bus, `AppendAutoConvexForSelection`, `HistoryRestore`, and world load.
- It is consumed **once per frame after the last drain** in `Main.cpp`, so a mutation is
  never reflected a frame late.
- `ShowWorldGraph(true)` refreshes unconditionally; a hidden panel costs nothing because
  the consume is gated on `showWorldGraph`.

**Setting a flag is not a mutation.** That is what keeps the layer rule intact: `UI/` still
posts and never mutates, `Subsystems/`/`Core/` only set the flag, and the single
`RefreshWorldGraph()` call lives in `Core`'s frame loop - Core reaching down into UI, the
legal direction. Do not "fix" this by calling `RefreshWorldGraph()` from a `Subsystems/`
handler.

Two calibrations worth keeping:

- **Gating the drains on `!empty()` is load-bearing.** Marking unconditionally rebuilt the
  list on every frame - measured at 19 rebuilds/second on an idle open panel.
- **The gizmo release is gated on `g_gizmoHistPushed`**, the flag meaning a transform was
  actually pushed to history. Ungated, every plain *click* refreshed the list, because a
  pick is also a release.

Verified by driving `IDM_WORLD_GRAPH` and querying the ListView directly: 131 rows
populated on open with no manual refresh, and zero rebuilds while idle.

## Control IDs must be unique per window

`ID_PAWN_SHOW_HIDDEN` and `ID_PAWN_TREE` were both `105`. It happened to work only because
the treeview emits `WM_NOTIFY` and the checkbox `WM_COMMAND` - but neither could be
addressed with `GetDlgItem`, and the checkbox's handle was discarded at creation, so its
state could not be seeded or reset.

The checkbox also had a hard-coded rect and was never passed to `LayoutPawnMgr`, so on
`WM_SIZE` it stayed at `y=228` while the tree and the three buttons re-flowed to
`y = 41 + treeH` - a visible 9px desync at the default size. It now has a distinct ID, a
kept handle seeded from `g_editorPanels.showHidden`, and a place in the layout; the default
`pawnMgrPos` is 480 wide so it no longer ends 6px from the window edge.

**When adding a control to a panel that has a layout function, add it to that function.**
A hard-coded rect in `WM_CREATE` is a bug the first time the window is resized.

## Texture Manager: delegate the scan, build thumbnails lazily

`ScanTextureBrowserFiles()` now delegates to `Resources/AssetScan`'s `ScanAssets` rather
than carrying a third copy of the GameData + package walk. It also stops re-deriving the
dedup policy from `!IsPathFile()`: `ResourceEntry` carries `fromPackage`, stated by the
scanner that knows which list the entry came from, exactly as `AssetScopeItem` already did.

Two resource bugs fixed alongside:

- **Thumbnails were built eagerly, one per texture in GameData, on every rescan** -
  thousands of GDI bitmaps and a visible memory spike at panel-open time, for cells that
  were off-screen or never scrolled to. `BuildTextureThumbnail()` is now called from the
  paint loop on first use. Off-screen cells are already `continue`d, so scrolling pays only
  for what it shows.
- **`ScanTextureBrowserFiles` cleared a vector owning an `HBITMAP` per entry without
  deleting them**, leaking one bitmap per texture per rescan - and a rescan runs on every
  window activation. The free loop now runs *before* the `clear()`.

The eager loop's `tex.path.substr(tex.path.rfind('.'))` was also unguarded: `substr(npos)`
throws when a path has no extension, reachable for a hand-built package key.

## Phase F status: parked at 5 of 11

Promoted to real translation units (`AngelEd/Makefile` rule + `ci.yml` entry each):

`SurfaceOps`, `Placement`, `PropsApply`, `LevelState`, `History`

Still unity fragments in `Main.cpp`, with the seam that blocks each:

| Fragment | Blocker |
|---|---|
| `Selection` | reads/writes `g_editorPanels` throughout; entangled with the Core dispatcher |
| `EntityOps` | `CommitBrushRenderable` + AutoConvex reach into `OzoneLoader` and UI statics |
| `WorldIO` | uses `ClearScene`/`Clear*` statics declared across Core fragments |
| `OzoneExport` | calls UI file dialogs (`ChooseOpenWorldFile`, `ChooseSaveWorldFile`) and Core statics (`g_documentPath`, `SetWorldDirectory`, `g_pendingNew`) - deliberately **not** promoted, since moving it either way is a violation |
| `WorldGraphBridge` | the `EditorWndProc` menu dispatcher, which owns Core statics |
| `AnimEditing` | cross-state access to the animation target and undo stacks |

Object lists verified in step: Makefile 44 / `ci.yml` 43, agreeing on all 43 editor
objects, the sole difference being `OTCustom_stub.o`.

## If you find `Win32Dialogs.cpp` referenced in these docs

It was **deleted in R3**. It was the single 8k-line file that held every Win32 native
panel; it is now `UI/Panels/*.cpp`, with `UI/UiShell.cpp` as the one unity TU that
`#include`s them, and the shared two-consumer code moved to `Resources/`.

Mentions of `Win32Dialogs.cpp` **inside the progress logs above are left as written** -
they record what was true when the work was done, and rewriting them would falsify the
record. The live pointers were fixed in `AGENTS.md` and `Wiki/Engine-Overview.md`:

| Symbol | Now lives in |
|---|---|
| `BuildAssetScope`, `AssetScopeItem`, `AssetScopeNode` | `Resources/AssetScope.{hpp,cpp}` |
| `PackIntoPackage`, `HotLoadPackage` | `Resources/PackageIO.{hpp,cpp}` |
| `FillDefBlock`, `kWeaponStats`/`kPlayerStats`/`kPawnStats`/`kLightStats`, the `Props*Scroll` layout | `UI/Panels/PropsPanel.cpp` |
