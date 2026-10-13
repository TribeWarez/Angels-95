# Core Game

## Controls

| Key | Action |
|---|---|
| WASD | Movement (first-person) |
| Mouse | Look |
| Shift (hold) | Sprint — faster movement (stance replicated to the server) |
| Ctrl (hold) | Crouch — lower profile (stance replicated to the server) |
| Left Click (hold) | Fire selected weapon (auto-fire at weapon's fire rate) |
| Right Click (hold) | Aim Down Sights (tighter crosshair, reduced spread) |
| R | Reload selected weapon |
| 1–8 | Select inventory/hotbar slot |
| Mouse Wheel | Cycle to the next/previous **occupied** slot (wraps; throttled to ~0.15 s per step) |
| Arrow Keys | Move the cursor within the inventory overlay's backpack grid |
| E | Collect nearby pickup |
| Tab | Toggle inventory overlay |
| Escape | Pause menu (Resume / Settings / Main Menu / Quit) |
| F11 | Toggle fullscreen |
| Grave (`) | Toggle developer console |

## HUD

- **Health / Mana / Psychic Energy** — three resource pools displayed at top, automatic regeneration (tick-based). Max values default to 100.
- **Level & XP** — exponential XP curve (`XP_BASE_TO_NEXT=100`, growth 1.3x per level). XP gained from pickups, NPC kills, and exploration.
- **SlotBar object bar (b82)** — 8 cells rendered from a data-driven atlas
  (`GameData/Tex/Engine/GameUI.ozls`: `texture` + comma-separated `slot_rects`
  in source-texture pixels). Occupied cells get a fitted entity icon, slot
  numbers and a gold selection/hover outline; number keys, mouse wheel and
  **click-to-select** all work. The same bar renders in-world (bottom centre)
  and inside the inventory overlay (mini-bar). Slot 1 is the default starter
  weapon.
- **Ammo counter** — shows current ammunition / magazine capacity for the selected weapon. Turns red when empty.
- **Crosshair** — 4-line crosshair at screen center. Gap expands with recoil bloom, tightens during ADS.
- **Muzzle flash** — brief yellow sphere at weapon origin on each shot.
- **Recoil** — camera pitches upward on fire, recovers over time. Random horizontal kick. Displayed visually via crosshair bloom.
- **Ping** — displayed when connected to a network server.

## Weapons & Combat

### Weapon Types

Weapons are data-driven via LightningScript entity definitions (`.ozls`). Two types:

**Ranged** — fires projectiles with configurable speed, spread, damage, and projectile count. Examples: `automag` (fire_rate=0.4, magazine=12), `Object1`–`Object5`.

**Melee** — forward range check on swing. Uses `reach` and `swing_speed` stats. No projectiles spawned. Example: `selenite_blade` (reach=3.0, swing_speed=0.8, damage=40).

### Weapon Stats

| Stat | Ranged Default | Melee Default | Description |
|---|---|---|---|
| `damage` | 10 | 10 | Damage per hit/projectile |
| `fire_rate` | 0.25 | — | Seconds between shots (ranged) |
| `swing_speed` | — | 0.25 | Seconds between swings (melee) |
| `magazine` | none | — | Ammo capacity (ranged only) |
| `reload_time` | 2.0 | — | Seconds to reload |
| `projectile_speed` | 20 | — | Projectile travel speed |
| `projectile_lifetime` | 2.0 | — | Seconds before projectile expires |
| `projectile_count` | 1 | — | Projectiles per shot |
| `spread` | 0 | — | Spread angle in degrees |
| `reach` | — | 3.0 | Melee attack range |
| `recoil` | 2.0 | 1.0 | Camera kick intensity |

### Ammo & Reload

- Ranged weapons with a `magazine` stat consume ammo per shot
- When empty, the weapon auto-reloads (triggering `on_reload` script action)
- Press **R** to manually reload (resets ammo to `magazine`, sets cooldown to `reload_time`)
- Ammo is displayed as `Ammo: current/max` in the HUD
- **AMMO pickups** (type 5) grant ammo to the player's ammo pool server-side

### Projectile System

- Client: projectiles are simulated in `PawnSystem::UpdateProjectiles()` with simple gravity
- Local single-player: projectiles collide with `Pawn` NPCs (radius 1.5), applying damage directly
- Multiplayer: `WeaponFireData` sent to server via UDP; server spawns `ServerProjectile` entities
- Server: `GameState::tick_projectiles()` runs per-world, checking collision against NPCs (global + partition) and other players (owner excluded)
- Server projectiles use `HIT_RADIUS = 2.0` for collision detection

### Visual Feedback

- **Crosshair**: 4-line white crosshair at screen center. Gap size increases with recoil bloom.
- **Recoil**: Camera kicks upward on fire; random horizontal component. Recovers at 0.82x per frame.
- **Crosshair bloom**: Expands on fire, decays at 0.90x per frame (0.75x during ADS).
- **Muzzle flash**: Yellow sphere at weapon origin, 0.12s duration, shrinks and fades.
- **ADS**: Right-click to aim. Tighter crosshair (gap 2 vs 5), faster bloom decay, reduced spread.

## Pickups

Pickup types are **data-driven** via LightningScript entity definitions (`.ozls` files parsed by `LightningEntityRegistry`). The legacy hardcoded types are:

| Type | ID | Effect |
|---|---|---|
| Health | 0 | Restores health |
| Mana | 1 | Restores mana |
| Psychic Energy | 2 | Restores psychic energy |
| Armor | 3 | Grants armor HP |
| Weapon | 4 | Unlocks Object1–5 in inventory |
| Ammo | 5 | Refills ammo pool (capped at 999) |
| Key | 6 | Adds key to inventory (grants XP server-side) |
| Coin | 7 | Currency (grants XP server-side) |
| Powerup | 8 | Grants XP server-side |

New pickup types can be added by creating `.ozls` entity definition files.

## Inventory

- **20 backpack slots** — collected items go here
- **8 equipment slots** — armor, jewelry (Armory1/2, Jewelry1/2)
- **5 weapon hotbar slots** — Object1–Object5, directly selectable with number keys
- Press Tab to open/close the inventory overlay

## Skills & Equipment (b78–b79)

- **Ethereal skill tree** — the title-menu **SkillTree** overlay ("Angel's
  Descent") renders every `skill`-typed `.ozls` entity as a tiered node graph.
  Left-click an available node to unlock it for Mana or Psychic Energy; nodes
  may require another skill and grant stat bonuses (`max_health_bonus`,
  `max_mana_bonus`, `max_psychic_energy_bonus`, `move_speed_bonus`). Unlocks
  persist through the toggle-flag save (`TF.sav`); **Respec (refund all)**
  re-allocates the spent resources.
- **Equipment wiring** — the inventory overlay renders an EQUIPMENT panel
  (left side) alongside the backpack; equipment choices and the sprint/crouch
  stance replicate to the server in multiplayer.

## NPC AI

Five-state finite state machine (no ATTACK state — combat is melee-range
checks and projectiles):

```
IDLE -> PATROL -> CHASE -> RETURN -> DEAD
```

- **IDLE**: standing still, waiting for a target to enter aggro range
- **PATROL**: circles around spawn point (follows the world's `PathNode`
  graph server-side when present, else the legacy circular patrol)
- **CHASE**: pursues nearby player within aggro range
- **RETURN**: goes back to spawn if target strays too far
- **DEAD**: despawned state after health reaches zero

NPCs are configured data-driven from `GameData/Global/PawnDefs/*.cfg` (name, speed, aggroRange, attackRange, damage, maxHealth) or via fallback hardcoded defs: Walker, Skaarj, Brute, Floater.

## Networking

- **Game port**: UDP 27015
- **LAN discovery**: UDP 27100 (servers broadcast presence)
- **HTTP API**: port 8080 (`GET /map?list`, `GET /map?name=X`)
- **Internet**: the Multiplayer → Join page's Internet source fetches the live
  server list from the configured masters (default
  `https://angels95.tribewarez.com/master`) and queries each server's
  `/status` for RTT; see `Wiki/Master-Server.md`
- Client connects via `Join Game` in the home screen, or an
  `angels95://join/<ip>:<port>` deep link

## Save System

Three binary save files in `GameData/Saves/` (all `.gitignore`d):

| File | Contents |
|---|---|
| `TF.sav` | Toggle flags + object ownership (100 flags) |
| `POS.sav` | Player position + level |
| `Script.sav` | (obsolete) removed with the legacy WDL system |

## Pause Menu

Press **Escape** during gameplay to open the pause menu overlay:

- **Resume** — return to gameplay
- **Settings** — open developer settings panel
- **Main Menu** — return to title screen (world is unloaded)
- **Quit** — exit the game

The pause menu uses custom button textures (`menu_button.png`, `menu_button_hover.png`, `menu_button_clicked.png`) and heading (`menu_heading.png`) from `GameData/Tex/Title/`.
