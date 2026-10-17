# Editor Property Panel Consolidation

**Status: P0–P4 IMPLEMENTED, awaiting manual verification.** All five targets
build and all 13 test suites pass, but the interactive checks listed at the bottom
have not been run — AngelEd is Windows-only and not in the test link, and the
`SurfaceMaterial` change alters pixels, so "it compiles" is not "it works". This
file is the source of truth for the work. Update the progress log as items land;
do not work from memory of a plan held in context.

The larger `Core/Subsystems/UI/Resources` split plus an `EditorEventBus` was
**deliberately deferred**: the defects that motivated this work are fixed without
it, and it remains the right next step for *maintainability*, not for correctness.

## Goal

1. Remove the duplicate **View → Zone Properties** and **View → Light Properties**
   windows. Both duplicate the working **Entity Properties** panel, and neither
   applies the settings you edit in it.
2. Replace the level-level state they uniquely owned with a **Map** row at the top
   of the WorldGraph list, which opens the same Entity Properties panel.
3. Give per-zone fog / ambient / reverb a real editor surface — it currently has
   **none**, even though it is what the game actually reads.
4. Re-sort the properties panel visually and fix the dead controls found in it.

## Why the two windows misbehave (diagnosed, not guessed)

### Light Properties — the target is always `-1`

`ShowLightProps` (`AngelEd/Source/Win32Dialogs.cpp:3764`) opens the Entity
Properties panel and returns early **when a light is selected**. That path is fine.

When **no** light is selected it instead shows the legacy window — but
`g_editorPanels.lightPropTarget` is assigned in exactly one place in the whole
tree (`Win32Dialogs.cpp:3771`), immediately before that early return. So the only
case the legacy window is reachable is also the case where its target is `-1`:

- `PawnSystem::GetLight(-1)` matches nothing → returns `nullptr`
- `WM_USER + 50` populate handler (`:3843`) skips its whole `if (ln)` block, so the
  panel shows `WM_CREATE` hardcoded defaults
- `Main.cpp:4661`'s `if (LightNode* ln = …)` is false, so the **entire** write
  block is skipped — including `HistoryPush()` and the `EditorLog` confirmation

It also has no `Name` and no `Position`/`Target` rows, so it could not round-trip
through the OZONE export even if it worked.

### Zone Properties — a level-global shader poke wearing a per-zone name

Its Apply handlers only set boolean flags on a file-static `g_zoneProps`
(`Win32Dialogs.cpp:2676`). The consumer (`Main.cpp:2609-2665`) then:

| Field | Fate |
|---|---|
| `fogR/G/B`, `fogDensity`, `fogIntensity` | reach `LitFogShader` uniforms |
| `fogStart`, `fogEnd` | **never consumed anywhere in the repo** — dead controls |
| `OTEditor.FogColor`, `OTEditor.FogDensity` | written at `:2621-2622`, read by nothing |
| `OTEditor.AmbientColor`, `OTEditor.AmbientIntensity` | written at `:2625-2626`, read by nothing |
| `gameType`, `maxPlayers`, `respawnTime`, `timeLimit*`, `scoreLimit`, `friendlyFire` | → `LevelMetadata` → `levelinfo` — **works** |
| `skyboxTexturePath` | → `LevelMetadata` → `levelinfo`, read live by the viewport — **works** |
| `particle*` | → `LevelMetadata` → `particles` — **works** |

Meanwhile `ZoneVolumeNode::envOverrides` — the per-zone fog/ambient/reverb the
**game** consumes (`Source/Core.hpp:1662-1699`, `SoundManager.cpp:359-360`) — is
written **nowhere** in the editor. Its only AngelEd occurrence is the export read
at `Main.cpp:1532`. So per-zone fog in a shipped level is hand-edit-only.

Also: the zone apply block never calls `HistoryPush()`, so its edits are invisible
to Ctrl+Z.

### What the WorldGraph panel actually is

**There is no WorldGraph tree.** Two separate panels, frequently conflated:

| Panel | Control | Holds | Right-click? |
|---|---|---|---|
| World Graph Explorer (`ShowWorldGraph`) | `WC_LISTVIEW`, 6 report columns (`Win32Dialogs.cpp:4118`) | world **instances** | **Yes** — the editor's only `NM_RCLICK`/`TrackPopupMenu` (`:4162`) |
| Pawn Manager → Actor Hierarchy (`BuildPawnTree`, `:1555`) | real `WC_TREEVIEW` (`:1896`) | **defs** to place, not instances | No — `WM_NOTIFY` handles `NM_DBLCLK` only |

That is why the Map row goes in the **list**: it inherits the working
right-click → Properties path for free, with no panel rewrite.

## Phases

### P0 — Foundations

- New header `AngelEd/Source/SelType.hpp`. `SelType` is file-local to
  `Main.cpp:99` while `Win32Dialogs.cpp` compares raw literals (`selType == 5`),
  so adding `MAP` as another literal is how the numbers drift. Header-only, so
  no `Makefile` / CI inline-g++ change (headers are not listed there).
- Add `MAP = 13`. Safe: `-1` is already the "def only, no instance" sentinel set
  by `ShowDefPropertiesFor` (`:5796`), and `0` is deliberately used by the
  non-selectable Sound/Music emitter rows.

### P1 — Map row + level state

Synthetic first entry in `BuildWorldGraphEntries` (`:3929`), before the Brush
block: `typeLabel = "Map"`, `selIndex = 0`, `posX/Y/Z = 0`, bold via a per-row
flag on `WorldGraphEntry` (`:3918`).

**Six guards — a missing branch is *not* harmless for an unhandled `SelType`:**

| Hazard | Site |
|---|---|
| Pos X/Y/Z + Rot emitted before any type branch | `Win32Dialogs.cpp:4896-4916` |
| Delete/Duplicate added unconditionally; both dispatch chains are `else if` with **no `else`**, so Delete silently deselects and pushes a stray `HistoryPush()` | `Main.cpp:2471-2473`, `:646`, `:688` |
| `Delete` / `Ctrl+D` gate is `!= NONE` | `Main.cpp:4815-4823` |
| `DrawModel = true` + `SnapGizmoToSelection` on every pick → draggable gizmo at `{0,0,0}` | `Main.cpp:3620-3621` |
| `HistoryPush()` runs *before* the dispatch → no-op Apply dirties undo | `Main.cpp:3689` |
| `RefreshWorldGraph()` has **zero callers** → list never repaints after Apply | `Win32Dialogs.cpp:4217` |

All 15 `LevelMetadata` fields get rows, seeded from `GetLevelMetadata()` in a new
`propsTargetType == MAP` branch of `ShowPropertiesPanel` (`:5579`):

- **Game Mode** — combo over `oz::gametype::AllGameTypes()`; combo index ==
  `GameType` value, so `CB_SETCURSEL((int)meta.gameType)` is an identity mapping.
- Max Players, Respawn, Time Limit Enabled + Minutes, Score Limit, Friendly Fire.
- **Resolved Ruleset** summary — refresh it (there is no `SetWindowTextW` today,
  so the "refreshed whenever the combo changes" comment at `:3172` is aspirational)
  and derive it from `ResolveGameTypeInfo`, not `GameTypeInfoFor`, so it reflects
  the level's own `levelinfo` numbers and any `.ozls : gametype` override.
- **Skybox** — path + Browse + Use Active Tex, seeded from state (the old dialog
  used hardcoded literals `L"8"`/`L"5"`/`L"10"`/`L"50"`).
- **Weather** — type combo + density/speed/RGB + **Wind X/Z**, which are declared
  at `:2786-2787` and **never created**, so wind is hand-edit-only today.

Four export / round-trip bugs fixed along the way:

| Bug | Site |
|---|---|
| `timeLimitMinutes` missing from the non-default test → a lone change is dropped | `Main.cpp:1640-1643` |
| `particles` gated on `particleType != NONE` → wind lost when the type is None | `Main.cpp:1657` |
| `gametype=` tail ignored on load (`arg(0)` only, never `pr.gametypeKey`) → editor and client can disagree on a hand-edited file | `Main.cpp:1154` |
| `skyboxSidePath` (`pr.entitySubType`) never read → dropped on every save | `Main.cpp:1149-1163` |

`AppendOzoneEntities` (`:1197`) is a **second**, colon-delimited LevelInfo/Particles
writer with no callers. Confirm dead, then delete rather than leave two writers.

### P2 — Per-zone env authoring + live preview

`SelType::ZONE` section (`:5072-5097`) gains an **Environment** group — Fog
R/G/B/Density/Start/End, Ambient R/G/B/Intensity, Reverb Mix/Decay — writing
`zone.envOverrides` and its `applyFog`/`applyAmbient` flags, which the exporter
already honours (`Main.cpp:1533`).

Preview needs two genuine engine bugs fixed:

1. `SurfaceMaterial.cpp:71` hardcodes `amb = {0.1, 0.1, 0.1, 1.0}` while its
   comment claims the value lives on `OzoneLoader` — there is no accessor and it
   never reads it. Change `UpdateFrame` to take ambient as a parameter (2 callers:
   `Source/Core.hpp:431`, `AngelEd/Source/Main.cpp:2676`) rather than adding a
   getter, matching the existing "the owner declares it" discipline.
2. `SurfaceMaterial::CacheLocations()` caches **no fog locations**, yet
   `GameData/Shaders/Surface.fs:64-68` declares and uses `fogStart`/`fogEnd`/
   `fogDensity`/`fogColor`/`fogIntensity` at `:223-228`. Surface-flagged brushes
   therefore always fog at the GLSL defaults. Add the five locations + `SetFog`.

Also republish ambient every editor frame from the selected zone's
`envOverrides`. AngelEd has **zero** `SetWorldAmbient` callers today yet calls
`DrawZoneGeometry` (`:2793-2799`), which restores the stale `0.1` and leaves it
clobbered for the rest of the frame and after.

And wire the two dead uniforms: `OTEditor.FogStartLoc`/`FogEndLoc`
(`Editor.hpp:37-38`) are resolved at `:229-230` and **never read again**, while
`LitFog.fs:47-51` declares both. That is exactly why the old dialog's two fog
range fields did nothing.

> **Approved consequence:** fixing these in the client as well as the editor means
> surface-flagged brushes render differently in the shipped game — brighter, and
> actually fogged. That matches the documented intent, but expect visual re-tuning.

### P3 — Remove the two windows

Delete `IDM_ZONE_PROPS` / `IDM_LIGHT_PROPS` from the menu (`:1862`, `:1865`), the
enum (`:55`, `:58`) and the dispatch (`:1800`, `:1803`).

**Split `ToggleEnvPanel`** (`:1050`) — it currently toggles the window *and* sets
`g_placeMode = PlaceMode::ENV`, two unrelated things. F4 (`:4777`) duplicates
both; F12 (`:4809`) calls the same helper. After removal F4 and the toolbar "Zone"
button (`:3531` → `:3443`) keep placement mode, and F12 is unbound.

**Check portal deletion first.** The old Portal tab had a list + Refresh + Delete
(`ID_ZONE_DEL_PORTAL` → `Main.cpp:4716-4723`). The Entity Properties `PORTAL`
section (`:5098-5110`) has Target World / Spawn / Bidirectional but **no delete**.
Add it there or move the list — do not silently lose the ability to remove a
portal.

Then delete `ZonePropertiesProc` (`:3050-3492`), `LightPropsProc` (`:3784-3909`),
their registrations (`:6633`, `:6637`), creations (`:6673`, `:6677`), the
`ZoneProperties` struct, `g_zoneProps`, `GetZoneProperties`, `ClearZoneApplyFlags`,
`ShowEnvPanel`, the related `EditorPanelState` fields, and `SetLevelMetadata`'s
17-line mirror into `g_zoneProps` (`:2700-2716`).

**Keep** `LevelMetadata`, `g_levelMeta` and the accessors — the Map panel owns them.

### P4 — Re-layout + dead controls

- One shared label width. The current `lw = 74` (instance rows) vs `defLabelW = 96`
  (everything from the def block on) produces a visible step in the value column at
  the first section header.
- Bold group headers with **distinct control IDs** — `addSection` (`:4920`) emits
  plain `SS_LEFT` and every header shares control ID `1`.
- Read-only rows visually distinct. They are `ES_READONLY` with **no control ID**,
  so Apply cannot reach them; keep that, just make it look intentional.
- Order: Transform → `<Type>` → Environment → Physics → Definition → Stats →
  Other → Actions → Apply/Close.
- `StatSpec` gains a `group` member; regroup `kWeaponStats` (currently interleaved)
  into Damage / Melee / Projectile / Viewmodel / Audio.
- Show `StatSpec::label` as the row's primary text with the key alongside — the
  label is never displayed today (the panel prints the raw key at `:4979`).
- `DefStatRow::isFloat` is stored and never read → apply `ES_NUMBER` to float rows
  and right-align them.

Dead code to fix:

| Item | Site |
|---|---|
| `ID_PP_STAT_BROWSE_0` (587-631) has **no handler** — the range test at `:5285` covers only `ID_PP_STAT_PREVIEW_0`, so Browse on a sound stat row is unreachable dead code | `:5285` |
| No-op `idx` computation (both arms yield 0) | `:4994-4996` |
| "read-only: def is packaged" branch is unreachable, because `FillDefBlock` only fills `propDefEditable` when `propDefWritable` — so a packaged def drops the whole section instead of showing it read-only | `:4969-4977`, `:5519` |
| `ID_BTN_GT_PREVIEW` — declared and created, no `WM_COMMAND` branch anywhere | removed with P3 |

## Change-impact register

**AngelEd-only (P1, P3, P4)** — nothing under `Source/` changes.

**Shared engine — four sites, one phase (P2):**

| File | Change | Callers |
|---|---|---|
| `Source/Renderer/SurfaceMaterial.hpp` | `UpdateFrame` also takes ambient | 2 |
| `Source/Renderer/SurfaceMaterial.cpp` | drop the hardcoded ambient; add fog locations + `SetFog` | — |
| `Source/Core.hpp:431` | pass the world's ambient | client |
| `AngelEd/Source/Main.cpp:2676` | pass the editor's ambient | editor |

No `Makefile` change (`SurfaceMaterial.cpp` is already in all three build lists),
no CI inline-list change, no protocol, no `.ozls` writer, no save format.

**Out of bounds without asking:** network protocol, `Makefile` object lists,
`.ozls` writer semantics, save-file formats, `Source/Pawn/`, `Source/Script/`,
`Source/Package/`. If a bug forces a fix outside that list, stop and ask.

## Invariants — do not regress

| Invariant | Why | Where |
|---|---|---|
| `addReadOnlyRow` creates EDITs with **no control ID** | Apply must never reach read-only rows | `Win32Dialogs.cpp:4926` |
| Per-row control IDs, not shared Preview/Browse + hit-test | a click cannot land on the wrong weapon's sound | `:5542`, `:5281` |
| `PropsUpdateScroll` runs **after** the resize | `nPage` comes from the client height; reversing it leaves a panel taller than the work area with no scrollbar | `:4776-4780` |
| `propsPanelPos.w = 540` | `PropsFitWindow` re-applies the create-time width; shrinking it steals the value column | `Win32Dialogs.hpp:374` |
| `LevelMetadata` / `g_levelMeta` / accessors survive P3 | the Map panel owns them | `Win32Dialogs.cpp:2694-2717` |
| `envOverrides` export gate `applyFog \|\| applyAmbient \|\| reverbMix > 0` | an explicitly-zeroed reverb is intentionally not emitted | `Main.cpp:1533` |
| Children move to scroll; owned painting scrolls at paint time | two different mechanisms, do not mix | `:4781` vs `:944` |
| A def gets an editable stat row **only** if a runtime consumer exists | see below | `PropsPanel.cpp` `StatSchemaFor` |

## The schema rule — no editable key without a live consumer

An editable row is a promise: the author types a number and the game behaves
differently. A row for a key nothing reads is worse than no row, because it
silently lies about what a level does. The rule, applied throughout the
game-hierarchy schemas:

1. **A key is editable** only when a shipped code path reads it at runtime —
   and the schema row cites that consumer in a comment.
2. **A key with no reader is not offered editable.** It still appears in the
   read-only dump (so nothing hand-authored is hidden), and the def's type gets
   a note explaining why there is no editor.
3. **A dead schema entry is a bug.** If the consumer is removed, the row goes
   with it in the same commit.

`StatSchemaFor()` returns `nullptr` for `EntityType::TRIGGER` and
`EntityType::EVENT` on purpose: `TriggerSystem` reads a trigger's **box and def
name from the OZONE line**, not from the def; and event topics are emitted by
the engine (`world.load`, `entity.*`, `pickup.collect`, `zone.enter|exit`, ...)
and consumed by the LightningEntityManager wildcard bridge. Neither def type
owns a runtime stat today. `FillDefBlock` therefore sets `propDefNote` for them
— *"No runtime-consumed stats … read-only template"* — rendered under the
Definition header, and the row dump stands on its own.

Do not "fill in" TRIGGER/EVENT schemas to make the panel look complete. Do it
when a runtime consumer lands: add the reader, the schema row, and the note
removal together.

## Ruled out — do not re-investigate

- **No WorldGraph tree exists.** See the table above.
- **Light Properties already forwards** when a light is selected. Its Apply only
  misbehaves in the no-selection case, because of the `-1` target.
- **Zone Properties' Fog/Ambient tabs are level-global**, not per-zone, despite the name.
- **Unreachable / dead, not undiscovered features:** `ID_BTN_GT_PREVIEW`,
  `ID_SF_PAR_WINDX`/`ID_SF_PAR_WINDZ`, `RefreshWorldGraph()`, `AppendOzoneEntities()`.
- **Sound/Music emitter rows carry `selType = 0` deliberately**, so
  `OpenPropertiesForSelection` bails at `Main.cpp:792`.
- **`g_editorPanels.propsTargetType = -1`** is the Script Manager's def-only
  sentinel, so out-of-range values are already valid.

## Progress log

| Phase | Item | Status |
|---|---|---|
| P0 | `Wiki/Editor-PropertyPanel-Refactor.md` + `AGENTS.md` pointer | **done** |
| P0 | `AngelEd/Source/SelType.hpp` — `SelType` moved out of `Main.cpp:99`, `MAP = 13` added, 24 raw literals in `Win32Dialogs.cpp` replaced with `sel::*` | **done** |
| P1 | Map row + 6 guards + level rows + apply + 4 export fixes | **done** (all 5 targets build) |
| P2 | zone env authoring + preview + 2 `SurfaceMaterial` fixes + dead uniforms | **done** |
| P3 | remove both windows, split `ToggleEnvPanel`, keep portal delete | **done** |
| P4 | re-layout + 4 dead-control fixes | **done** |
| V1–V6 | build, test, round-trips, regression sweep | pending |

### P1 notes (deviations from the plan, recorded)

- **The Map row is NOT bold.** A ListView has no per-item bold state
  (`LVIS_BOLD` does not exist) and `NM_CUSTOMDRAW` cannot supply one either:
  MinGW's `NMLVCUSTOMDRAW` exposes only `iSubItem` (the column), not `iItem`,
  so there is no portable row index to test. It is distinguished by the label
  `"Map (level)"` plus blank position columns instead.
- **`LevelMetadata::skyboxSidePath` added.** It did not exist — the second
  OZONE `levelinfo` path token was parsed into `OzonePrimitive::entitySubType`
  and then dropped by both the loader and the exporter.
- **`ApplyMapProperties()` is a separate function** with its own diff-based undo
  policy, dispatched before the generic `HistoryPush()`.
- **`ChooseColorRGB()` added** (`CHOOSECOLORW`). The legacy dialog used three
  HSCROLL sliders per colour, which cannot express an exact value. The new
  Environment/Weather rows are text fields with a picker.
- Two additions beyond the listed items, both load-bearing:
  `Win32Dialogs.cpp` gained a `sel::MAP` branch in the *WorldGraph popup menu*
  (it had its own Delete/Duplicate, same silent-no-op problem), and the
  world name for the Map row comes from `Editor_GetCurrentWorldDir()`.

### P2 notes (deviations from the plan, recorded)

- **`OzoneLoader` now owns the world fog too** (`SetWorldFog`/`GetWorldFog`,
  alongside the existing `SetWorldAmbient`). The plan said to pass ambient as a
  parameter and to add fog locations only to `SurfaceMaterial`, which left the
  question unanswered: *where does the surface program get fog?* `Core.hpp` sets
  the five fog uniforms on `LitFog` from **three** separate sites (startup
  defaults, zone entry, zone exit). Mirroring at each would be three places to
  keep in step; publishing once into `OzoneLoader` makes `UpdateLightSources()`
  the single mirror point. Same reasoning as the `SetWorldAmbient` comment that
  already existed for ambient.
- **The editor preview no longer reads the legacy global.** It reads the
  *selected* zone's `envOverrides` (`Main.cpp`, per-frame block). That is what
  `Core.hpp`/`ZoneManager` apply at runtime and what `ExportToOzone` writes, so
  previewing the legacy global showed the author a result the game never
  produces. As a side effect the block no longer depends on the old window's
  one-shot `apply*` flags, which is what makes the old window deletable rather
  than something that has to be kept in sync. Only the level-state half
  (`GameType` / skybox / particles) still reads those flags, and it dies with P3.
- **`FogStartLoc`/`FogEndLoc` are not "resolved and never read"** — they are
  resolved at `Editor.hpp:229-230`, written **once** at `:242-243`, and then never
  updated. The precise bug is that the legacy dialog's Start/End rows looked live
  and did nothing: colour, density and intensity tracked the zone while the fog
  *distance* stayed pinned at 10/100.
- **`ZoneProperties` field removal deferred to P3.** The struct still carries
  `fogStart`/`fogEnd`/`reverbMix`/`reverbDecay` for the old dialog's benefit;
  they become dead the moment that window goes.

### P3 notes (deviations from the plan, recorded)

- **Portal deletion was NOT left to the panel's Apply.** The plan said "add it
  there or move the list". It is a button (`ID_PP_PORTALDELETE = 470`) that sets
  `actionDeletePortal = propsTargetIndex`, because delete mutates the vector the
  panel is indexing into — routing it through the ordinary Apply path would apply
  the panel's *stale* size fields to the portal that shifts down into that slot.
- **Deleting a portal now clears the selection.** `RemovePortal()` shifts every
  later portal down by one, so a retained `propsTargetIndex` meant the next Apply
  silently edited a different portal. The old list hid this by rebuilding itself.
- **The dead portal-apply plumbing went too, not just the windows.**
  `PortalEditState`, `PortalEditValues`, `GetPortalEditValues`, `SetPortalSelection`,
  `LoadPortalIntoEditor`, `actionApplyPortal` and `actionSelectPortal` were all only
  reachable from the removed window. Portal editing is the panel's
  `tgtType == sel::PORTAL` Apply branch; picking a portal in the viewport now opens
  the panel instead of calling `SetPortalSelection()`.
- **The level-state half of the per-frame block was deleted, not migrated.** It
  existed only to notice the old window's one-shot `applyGameType`/`applySkybox`/
  `applyParticles` flags. With the window gone there is no setter, so keeping the
  reader would have been an unreachable branch.
- **`RefreshPortalList()` is now an intentional no-op**, kept because three
  Main.cpp call sites (portal create/delete) use it as the "portals changed"
  notification. Its old body was already dead behind an `hEnvPanel` guard.
- **`ID_PP_PORTALBROWSE = 470` was already dead** — declared, never created, never
  handled. Its slot was reused for `ID_PP_PORTALDELETE`.
- **`GetPortalCount()` / `GetPortalTargetWorld()` had zero callers** and were
  deleted in the b88 dead-code sweep (their `UiPanels.hpp` declarations, inline
  fallbacks and `UI/Panels/LevelState.cpp` definitions all went together).

## Dead code found and NOT removed (P4 backlog)

| Item | Why it is dead |
|---|---|
| `GetPortalCount()`, `GetPortalTargetWorld()` | **deleted** — zero callers, predated this work |
| `ID_PP_PORTALBROWSE` | removed in P3 (slot reused) |
| `ID_BTN_GT_PREVIEW` | declared and created in the removed window, so now gone |
| `Editor.hpp` `ShowEnvPanel` flag | nothing reads it; the window is gone |

## Verification

### P4 notes (deviations from the plan, recorded)

- **`StatSpec::label` is now shown, with the key alongside it** — `Label  (key)` in
  a 190px column. The def block (read-only dump, PawnDefs rows and schema rows)
  shares that width, so the value column steps **once**, at the `Definition`
  header, rather than mid-section. A tooltip was considered and rejected: it needs
  a tracking tooltip control and buys nothing over showing both strings.
- **Float rows get `ES_RIGHT` at creation, not `EM_SETALIGN`.** `EM_SETALIGN` is
  not declared by MinGW's headers, and the style bit is what the message sets.
  Right-alignment is a hint — `ES_NUMBER` stays off because it rejects both a
  leading `-` and a `vec3` like `(0.1, 0.2, 0.3)`.
- **`StatSpec::group` populated for all four schemas** (82 rows) and `kWeaponStats`
  reordered so each group's rows are contiguous — `swing_speed` and `reach` used to
  sit between the Handling rows. Groups: weapon Damage/Handling/Melee/Projectile/
  Viewmodel/Audio, player Vitals/Progression/Audio, pawn Combat/Appearance/
  Animation/Audio, light Effect/Shadow/Spot cone.
- **`StatSpec::group` has a default-ish empty value**, meaning "continue the
  previous group". `DefStatRow::label` falls back to the key when a schema omits
  one, so a stat with no label still renders.
- **Section headers are bold via `WM_SETFONT`** with a single shared
  `GetBoldUiFont()` HFONT. A STATIC control has no bold style bit, so there was no
  other way; the font is deliberately leaked because live controls must not outlive
  their HFONT. Header IDs are now `ID_PP_SECTION_0 + n` (parked at **20000**) with
  a counter reset per rebuild — they used to all share control ID `1`.

### Automated result (verified)

| Check | Result |
|---|---|
| `make MODE=debug OTENGINE AngelServ AngelMaster ozpack` | clean |
| `make -C AngelEd MODE=debug` | clean |
| `make test` (13 suites) | all green — 7/7, 39/39, 22/22, 26/26, 15/15, 11/11, 20/20, 26/26, 4/4, 114/114, 54/54, 21/21, 0 failed |
| `make worldcheck` (6 shipped worlds) | 0 errors, 38 warnings (pre-existing) |

### Manual checks still outstanding

Everything below is interactive and **cannot** be covered by the headless suites:
AngelEd is Windows-only and not in the test link, and the `SurfaceMaterial` change
alters pixels.

- **Map round-trip:** load `TestMap`, right-click the Map row, edit Game Mode /
  Skybox / Weather, Apply, save, reopen → values intact and the `levelinfo` /
  `particles` lines are correct.
- **Zone env:** select a zone, set fog/ambient/reverb → the viewport preview
  changes, surface-flagged brushes follow (not stuck at 0.1), and the exported
  zone line carries the values.
- **Menu:** View has neither entry; F4 and the toolbar Zone button still enter zone
  placement; a portal can still be deleted; deleting the *selected* portal closes
  the panel rather than leaving it aimed at a shifted index.
- **Stats:** change a float stat (right-aligned); Preview **and** Browse both work
  on a sound row — Browse was dead until P4.
- **Packaged def:** select a weapon that resolves from a `.oz*` package → the stats
  section now *appears* (read-only, no control IDs) instead of vanishing, and
  Apply does not strip it.
- **Regression:** undo/redo across a MAP Apply, a zone env Apply and a stat
  patch; plus a **client visual check** of the `SurfaceMaterial` change in both a
  lit room and a fog volume — not just that it compiles.

> Note: `GameData/Worlds/TestMap/World.ozone` was already dirty in the working
> tree before this work began (an earlier AngelEd re-export that dropped
> `flags=16`). Do not revert it; only change it as far as these edits go.