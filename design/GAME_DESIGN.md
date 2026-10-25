# GAME_DESIGN — Angels95, Sefirah 1 opening (Sektor_01–03)

**Scope.** The campaign's first act: `Sektor_01_SilentPines`, `Sektor_02_MegacityGrid`,
`Sektor_03_AegisBaseAlpha`, and the campaign layer that carries the player between them.
Everything below is a contract for *this* slice. Sefirah 2–5, the hub/social layer and
multiplayer mission sync are out of scope.

**Provenance.** No `SOURCE_BIBLE.md`, `CONCEPT.md` or `PRODUCT_BRIEF.md` exists in the repo,
so this contract is derived from shipped content (`GameData/Game/Angels95/**`,
`GameData/Worlds/Sektor_0*/**`), the runtime that consumes it, and the authored intent in
each `World.ozone` header. Where the source and the runtime disagree, the source is the
intent and the runtime is the current behaviour; §7 lists which side each decision changed.
Language: English (no brief to lock it otherwise).

---

## 1. Experience and loop contract

**Player identity.** A soul newly admitted to the Sefirah chain, arriving at Terran Prime
with a sidearm and no standing. Not a chosen one: a visitor who has to earn the right to be
somewhere.

**Player promise.** *Every district tells you what it wants before it charges you for it,
and every mission ends where its landmark said it would.*

This is the one sentence the slice must not break. It is already the design of the geometry
(the gorge descends toward the shrine; the tower is visible over the north gate; the empty
cradle is the first thing the hangar shows) and it is exactly what the campaign layer
currently breaks by teleporting the player out after five kills.

**Target feelings.** Read the room → choose a line → commit → be paid in information or a
weapon → walk out through the thing you were shown.

**`experienceProfile`.** First-person, single-player-led, co-op supported. Short sessions
(8–15 min per district). Low system load: three verbs (move, shoot, choose a route).
No dialogue trees, no vendors, no crafting.

**Core loop (per mission).**

| Step | Player action | World response | Read-out |
|---|---|---|---|
| 1 | Arrive at the authored gate | Landmark in view, ambient line, no enemy in reach | `say` on zone enter + mission start line |
| 2 | Move along the readable route | Named patrol type is the objective; other enemies are obstacles | Objective HUD row (`<type> n/N`) |
| 3 | Take or skip the optional branch | Branch pays a weapon / an angle | Pickup, sightline |
| 4 | Walk through the landmark exit | Mission completes, next district loads at its own gate | Completion line, world change |

**Precedent, and what this game changes.** The loop is UT99/NyLeve-shaped (linear
landmark-led level, one optional branch, exit at the landmark) with Quake-era weapon
pickups. What it changes: the objective never asks for a full clear, so the branch and the
route stay a choice; and completion is *spatial* (the exit), never a counter hitting zero.

---

## 2. World rules and progression

**Visible to the player.** Objective row + progress; mission name; the district's name on
zone entry; the route (geometry, light, landmark); weapon/ammo state.

**Hidden.** NPC patrol graph contents; the exact aggro radii; the objective's target type
until the row names it.

**Action order.** No turn structure. Ordering rules that matter:

- Mission start fires once per active mission, at the moment the mission is bound, not on
  world load (a reload of the same mission does not re-fire it).
- Mission completion fires once per mission, at the first exit through the mission's `next`
  world, and it fires *before* the active mission advances.
- An objective can only advance while it is the active mission's objective.

**Randomness.** None authored in this slice. Enemy placement, patrol phases and projectile
spread are the only stochastic inputs; none of them gates progression.

**Resources and persistence.**

| Resource | Read by | Persists |
|---|---|---|
| Health / mana / stamina | combat, respawn | session only |
| XP, level | server kill awards, HUD | world save, server-owned |
| Hotbar/backpack | combat, HUD | `POS.sav` via `SaveGame` |
| Active mission + objectives | HUD, campaign save, continuation | `Campaign.sav` (mission name only) / `ObjectiveSystem` (session) |
| Mission completion per world | join placement, next transition | server `WorldState` (session) |

**End-state marker.** The player is in `Sektor_03`'s rig hall, has seen the machine, and the
active mission has advanced to `KryoArchipel_Severny`. There is no score; "act complete" is
the next district loading.

---

## 3. Decision and level beats

The slice promises **route choice**, not build choice: two lines through each of maps 2 and
3, paying time against risk, plus one optional room worth a weapon.

| Mission | Beat | What it asks | What it pays | If skipped |
|---|---|---|---|---|
| 01 SilentPines | arrival plateau, 18u above the gorge | read the descent; no threat in reach | the whole valley in one view | — |
| | 19-step descent | commit to the stair, not the cliff edge | safe passage | a deliberate leap off the rim (this build has no fall damage; the rim is 1.4 high, so a walk-off is blocked and only a jump costs you the stair) |
| | bridge + shrine | fight on the bridge or wade the river | sword at the altar, ammo | sword is missed |
| | shrine → portal | walk out through the shrine | completion + Megacity Grid | stay in the gorge |
| 02 MegacityGrid | south avenue | see deck + tower over the gate | the route map | arrive under the deck instead |
| | crossroads | fight or flank behind the newsstand | rifle on the deck | rifle is missed |
| | **branch** relay hall (door) / substation (dead end) | which interior to clear | hall: cover + sightline; substation: coin | one of them is missed |
| | north gate → plaza | read arcade / vault / tower as three options | choice, not a corridor | — |
| | ring deck → tower top | climb under the stairs' own fire | overview + exit | plaza stays unclear |
| 03 AegisBaseAlpha | south gate → yard | read the hangar doors | cover layout | — |
| | hangar | the empty cradle (the machine is not here) | the beat, and where to go next | — |
| | **branch** barracks (pistol) / armoury (rifle) / watchtower | spend time for a weapon / an angle | loadout | weaker at the annex |
| | motor pool | open apron, bunker roofs | high ground | — |
| | annex gallery vs hall floor | the cells hold the loot | safer read + sword | floor fight is direct |
| | rig → exit | walk into the machine | completion + KryoArchipel | — |

**Causality / settlement.** The player owns *which line* and *when to leave*. The world owns
*what is on each line*. No mission is failed by skipping content; the only failure is death,
and death respawns at the district's first `playerstart` with the def's health.

**Anti-choice to avoid.** A single sweep that dominates (kill everything, then leave) is
acceptable but must cost time; if the objective ever becomes "clear the map", the branch
stops being a branch — that is the rejected design.

---

## 4. Feedback, failure, restart

| Event | Feedback | Source of truth |
|---|---|---|
| Mission bound | OBJECTIVES row appears (mission's own objective + target + count); the district's authored zone line names the place | local `ObjectiveSystem` (single-player) / `OBJECTIVE_STATE` (co-op); `skyzone on_enter` |
| Objective progress | row updates, `<target> n/N` | local `entity.death` / `pickup.collect` listeners (single-player) / server store (co-op) |
| Objective complete | row turns green; the objective def's `on_complete` line lands in the message log | `ObjectiveSystem` → `objective.complete` → the def's action |
| Mission complete | the exit portal (shimmer) + the next district's authored line. The mission def's `on_complete` is emitted as `mission.complete` and lands in the message log; TextSystem holds one line, so the district line owns the big on-screen slot | portal path / server exit + `mission.complete` |
| Damage taken | hurt sound, damage arc, low-health pulse | `PlayerController::PlayHurt` |
| Death | death sound + overlay, respawn at `playerstart #1` | server / local respawn |
| Refused transition | explicit line, never silent | out-of-range/mismatched world requests are logged and answered |

**Failure diagnosis.** Every failure the player can cause is legible: death (health reached
0), a missed branch (the weapon is visible but behind a door), a wrong turn (the landmark
recedes). There is no fail state that requires guessing.

**Restart.** Mission restart re-binds the mission (objectives reset to 0). A new session
clears mission, objectives, world flags, hotbar and backpack; a *Continue* restores mission
and loadout over that cleared state. World resets do not resurrect dead NPCs for the session
unless the server restarts.

---

## 5. Narrative attachment

The slice has **no dialogue system and no NPC speech** — there is nothing in the runtime for
an NPC to say. Narrative is carried by three things and no more:

1. **Zone-entry lines** (`skyzone on_enter`): one per district, naming the place and its
   mood. Already authored; keep them.
2. **Mission lines** (`mission on_start` / `on_complete`): one sentence each, from the
   campaign's point of view, not a character's.
3. **The geometry's own argument**: the descent, the tower over the wall, the empty cradle.

**Scene spine.** SilentPines (a world that predates the machine) → Megacity Grid (a city
wired into it) → Aegis Base Alpha (a base that moved it). Each mission's *object* is the
place, not a person.

**Knowledge boundary.** The player learns only what the geometry and the three lines state.
No narrator explains the Sefirot, the machine's purpose, or who moved it. If a later act
wants that, it must add a discoverable object — not a line in this slice.

**Branch / consequence.** The slice has no narrative branching. Weapon and route choices
have no downstream narrative effect in Sefirah 1; they change only the next fight. Declared
explicitly so no later claim of "choices matter" is made against it.

**Design invention** (not from an upstream bible, because there is none): the mission lines
in §7 are written here. Treat them as provisional until a source bible exists.

---

## 6. Language, culture, handoff

- **UI language:** English, sentence case, no invented jargon in the HUD. District names are
  proper nouns and are not translated.
- **Tone:** short declaratives. No exclamation marks in mission lines; the world is not
  enthusiastic.
- **Terms that must stay stable** (they are ids): `Sektor_01_SilentPines`,
  `Sektor_02_MegacityGrid`, `Sektor_03_AegisBaseAlpha`, `Objective_*`, `mission.next`.
- **Content rating:** violence is stylised PS1 low-poly; no gore requirement (the `Gore`
  setting exists and defaults off in the shipped config).
- **Handoff to art/build:** the level geometry is already the spec. This slice asks for no
  new art. It asks for one HUD row, two authored lines per mission, and the exit as the
  transition. Implementation boundaries are in §7.

---

## 7. Executable slice contract

**Biggest design risk.** That the campaign layer and the level layer are two different games:
the level says "walk to the landmark", the campaign says "kill five and be teleported". If
the two disagree, nothing else about this act matters. The slice exists to force them to
agree.

**Prototype form.** Shipped-geometry vertical slice, no new systems: objectives reused
(kill/collect), one new stat reader (`target`), one transition rule (exit), two event hooks
(`mission.start` / `mission.complete`), one HUD row.

**Minimum state.**

| State | Initial | Written by | Read by |
|---|---|---|---|
| active mission | none | menu launch, campaign continue, exit transition | HUD, save, campaign graph |
| local objective progress | 0 | local kill/collect listeners (single-player) | HUD |
| server objective progress | 0 | `damage_npc` / `collect_pickup`, target-filtered | `OBJECTIVE_STATE`, HUD |
| world `mission_transitioned` | false | exit request to the mission's `next` world | join placement, frontier |
| hotbar | starting weapon from `Player.ozls` | LEM init, pickups, save load | combat, HUD |

**Actions: precondition → effect → read-out.**

| Action | Precondition | Effect | Read-out |
|---|---|---|---|
| bind mission | mission def resolves | objectives defined; `mission.start` emitted | mission line |
| kill NPC | npc type matches objective target (or target empty) | objective +1 | HUD row |
| collect pickup | pickup name matches objective target (or target empty) | objective +1 | HUD row |
| cross the exit | destination is the mission's `next` world | mission completed once; every player in the origin world moves | completion line, new district at its gate |
| die | health ≤ 0 | respawn at `playerstart #1`, mission state unchanged | death overlay |

**Seed/observers.** No seed needed. The only observer-sensitive facts are objective progress
(one writer per mode: server in co-op, local listener in single-player) and mission
completion (server owns it in co-op, the portal owns it single-player). Neither is read
across modes.

**Adaptation decisions made in this pass (design ← implementation).**

| Decision | Chosen | Because |
|---|---|---|
| completion trigger | exit portal, not objective count | the authored arcs already end at the landmark; a counter pre-empts them |
| objectives | target-aware (`kill Walker`, `collect etheral_waver`), *advisory* | they point at the route; making them a lock would need a sealed-door system and would bounce clients |
| single-player objectives | local listeners advance the same `ObjectiveSystem` | the server's store is unreachable offline; the HUD must not be co-op-only |
| starting weapon | authored `starting_weapon` on `Player.ozls` | mission 1 ships no sidearm; an unarmed first minute is not a design |
| portal arrivals | the destination's authored `playerstart` (Sefirah 1's internal transitions) | the maps' first minutes are authored around those gates |
| mission lines | `mission.start` / `mission.complete` → def `on_start` / `on_complete`; the OBJECTIVES row is the durable read-out because TextSystem holds one line and the district's `say` owns it | the bus bridge already resolves `on_<last segment>`; no new hook |

---

## 8. Verification of the slice

**Range.** Three missions, single-player and co-op (`--campaign`). Not: hub/social, save
migration, multiplayer mission-name sync (co-op clients learn objectives but not the
mission's narrative line).

**Non-goals.** No new objective types (`reach`, `escort`), no keyed doors, no dialogue, no
difficulty menu, no hub.

**Complete path (must be walkable end to end).**
`New Campaign` → SilentPines armed with a visible weapon and an objective row → shrine exit
→ completion line → Megacity Grid at the south avenue → crossroads → north gate → tower top
→ Aegis Base Alpha at the south gate → hangar → rig exit → KryoArchipel.

**Observations that would change the design.**

- A fresh single-player campaign starts with an empty hand. → starting-weapon path failed.
- The objective row is invisible without opening a menu. → local objective path failed.
- A mission ends while the player is still mid-district, or the next district is entered
  anywhere other than its authored gate. → transition rule failed.
- Two different mission types produce the same fight because the target filter is ignored.
  → objective seeding failed.
- The player reaches the exit and is refused, or is bounced back. → completion must remain
  unconditional at the exit.

**What this verification cannot show.** Whether the route *feels* like a choice, whether the
pacing is right at 8–15 minutes, or whether the numbers are balanced. Those stay open
judgements; see the open items in `Wiki/Game-Hierarchy.md`.

---

## 9. Status of this pass

**Landed with this contract.**

- Sektor 1's three missions now carry their own target-aware objectives
  (`Objective_S01_GorgeWatch`, `_S02_RelayGuard`, `_S03_HangarBreach`) and authored
  start/complete lines.
- Objectives filter by `target` on both ends (server `WorldState::Objective::targetName`,
  client `GameManager::LocalObjective`), so `kill 4 Walker` no longer counts a Brute.
- Completion moved from "every objective ticked" to the mission's authored exit
  (`GameState::complete_mission_at_exit`, called from `REQUEST_WORLD`); the party moves
  together and it fires once.
- Single-player seeds and advances the same objectives locally (`entity.death` /
  `pickup.collect`), and the OBJECTIVES row is visible without a menu.
- `EnsureStartingWeapon()` grants the `Player.ozls` `starting_weapon` once per session,
  after a Continue has had the chance to restore a loadout.
- Sefirah 1's internal portal arrivals land on the destination's authored `playerstart`
  (map 2's south avenue, map 3's south gate), so the level's first-minute read survives.

**Not in this pass** (the ranked P2/P3 register from the investigation):

- Release packaging: `System/Data/Zones/world_*.ozone`, `game_Angels95.ozpak` and the world
  packages are generated by `build-data.ps1` and still hold the pre-pass content; a
  release build must be repacked (dev runs read the loose files first). The new
  per-district triggers, mission/objective edits, NPC speed changes, sector role change and
  `WORLD_CHANGE` mission tail must be included.

**Implemented in this follow-up pass** (closing the register above):

- **NPC chase speed (provisional):** `Walker` 3.8, `Skaarj` 5.2, `Brute` 2.8, `Floater` 4.4
  (45–69% of the player's walk). Sprint still disengages. Values are provisional pending a
  timed playtest.
- **Per-district trigger lines:** `Trigger_S01_Bridge`, `Trigger_S02_Avenue`,
  `Trigger_S02_Gate`, `Trigger_S03_Yard`, `Trigger_S03_MotorPool` replace the shared
  `Trigger_EnterVolume` line on the three maps.
- **`Key` / `Powerup`:** removed from `Sektor_01_SilentPines` (no runtime consumer beyond
  XP, no door/power state exists). Defs remain available for a sector that uses them.
- **Hub claim retired:** `Sefirah_1_TerranPrime` is `role = frontier` (ships combat
  districts, no hub services).
- **Co-op mission beats:** `WORLD_CHANGE` carries an optional trailing `mission[40]`
  (`kWorldChangeSizeWithMission`), and co-op clients emit `mission.start`/`mission.complete`
  without binding the mission into `GameManager` (so no `Campaign.sav` pollution).

**World content pass (this contract applied to the three `World.ozone` files):**

- **The descent is the only walkable way down (map 1).** Added a 1.4-high broken-rock rim
  along the plateau's east edge with the stair mouth left open at `y -3..3`. This build has
  no fall damage, so without the rim the "commit to the stair" beat was optional *by
  accident — you could walk off the edge for a free 18-unit drop. The lip sits below eye
  height (19.4 vs 19.8), so the arrival view is unchanged.
- **Exit thresholds shimmer (all three maps).** `ZonePortal` is pure logic and draws
  nothing, while §4 promises "the exit portal (shimmer)". Each exit now carries two
  co-located `ParticleEmitter` lines (a slow rising column + threshold sparks), tinted per
  district: shrine gold, relay cyan, rig pale-blue. Map 3's empty cradle also keeps a few
  fading embers, so "switched off but still there" is visible before the line says it.
  Budget and lifecycle: `design/vfx-sefirah1-exits.json` (validated: 32 particles, 2 draw
  calls per instance; the ambient layer is gated by the existing Particles toggle).
- **The altar keeps a light (map 1)** so the branch reward — the blade — reads from the
  bridge before the player commits, matching "sword at the altar".
- **Market roof pays the climb (map 2):** the optional roof had grass, a mana vial and a
  brute but no coin; the coin now matches the beat table.
- **Both cells hold loot (map 3):** the west cell already had the blade; the east cell was
  empty, so it gets a medkit on its restraint plinth.


