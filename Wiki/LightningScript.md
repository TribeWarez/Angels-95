# LightningScript Reference

LightningScript is the OmegaTech Engine's scripting language for defining entity behaviors, world interactions, and dynamic events. It replaces the legacy ParasiteScript system.

Files use the `.ozls` extension.

## Entity Definition Format (`.ozls`)

```
entity "<name>" : <type> {
    mesh = "<path>"
    texture = "<path>"
    icon = "<path>"

    stats {
        <name> = <value>
        vec3 <name> = (<x>, <y>, <z>)
    }

    variants {
        "<variantName>" { mesh_override = "<path>"  texture_override = "<path>" }
    }

    actions {
        on_<action> {
            <script lines>
        }
    }
}
```

### Types

| Type | Purpose |
|---|---|
| `weapon` | Ranged/melee weapon (hotbar, projectile dispatch) |
| `armor` / `upgrade` | Equipment items |
| `consumable` | Consumables (use via hotbar `on_use`) |
| `pickup` | World pickup (item_id/respawn_time stats) |
| `skyzone` | Zone volume with env/fog/skybox/zone actions |
| `pawn` | NPC hook-carrier (FSM actions: on_patrol/on_chase/on_return/on_death) |
| `projectile` | Reserved (no runtime consumer yet) |

### Stats Block

Defines runtime numeric properties:

```
stats {
    damage = 25.0
    magazine = 12
    vec3 color = (1.0, 0.2, 0.1)
}
```

#### Weapon-Specific Stats

| Stat | Type | Ranged Default | Melee Default | Description |
|---|---|---|---|---|
| `damage` | float | 10 | 10 | Damage per projectile or melee hit |
| `fire_rate` | float | 0.25 | — | Seconds between shots (ranged) |
| `swing_speed` | float | — | 0.25 | Seconds between swings (melee) |
| `magazine` | float | none | — | Ammo capacity per magazine (ranged only) |
| `reload_time` | float | 2.0 | — | Seconds to complete a reload |
| `projectile_speed` | float | 20 | — | Projectile travel speed in units/sec |
| `projectile_lifetime` | float | 2.0 | — | Projectile lifespan in seconds (legacy key `lifetime` also accepted) |
| `projectile_count` | float | 1 | — | Number of projectiles per shot |
| `spread` | float | 0 | — | Random spread angle in degrees |
| `reach` | float | — | 3.0 | Melee attack range in units |

#### Sound Stats

Sound is authored per-entity in the same `stats` block. Values are asset paths;
they resolve def-relative first, then via the package loader, so a `.wav`
inside a `.ozsnd` works exactly like a loose file.

| Stat | Type | Applies To | Default | Description |
|---|---|---|---|---|
| `fire_sound` | string | ranged weapon | `Audio/Sounds/Guns/machgf3b.wav` | One-shot per projectile fired |
| `swing_sound` | string | melee weapon | *(silent)* | One-shot per melee swing |
| `hit_sound` | string | melee weapon | *(silent)* | Melee connect that is **not** blocked by geometry |
| `reload_sound` | string | any weapon | *(silent)* | Once per reload (manual **and** auto) |
| `equip_sound` | string | any weapon | `Audio/Sounds/Entities/weapload.wav` | Selecting the weapon in the hotbar |
| `fire_volume` / `swing_volume` / `hit_volume` / `reload_volume` / `equip_volume` | float | matching key | 1.0 | Linear gain |
| `fire_pitch` / `swing_pitch` / `hit_pitch` | float | matching key | 1.0 | Playback rate (`0.85` = deeper) |

Player sound lives on `Player.ozls` (`GameData/Tex/Global/Models/`), read off the
auto-spawned Player entity: `jump_sound`, `land_sound`, `walk_sound`,
`run_sound`, `hurt_sound`, `death_sound`, each with a `_volume`.

```
stats {
    fire_sound = GameData/Audio/Sounds/Guns/machgf3b.wav
    fire_volume = 0.9
    fire_pitch = 0.85
}
```

**Authoring rules — these fail silently if broken:**

- **No quotes, no spaces.** A `stats` string value is stored *verbatim*; the
  parser does not strip quotes, and it stops at the first space. Write
  `fire_sound = a/b.wav`, never `fire_sound = "my gun.wav"`. A space truncates
  the path and it will simply never resolve.
- **Deliberate asymmetry:** `fire_sound` and `equip_sound` fall back to a
  global default, so an unauthored weapon still shoots. `swing_sound`,
  `hit_sound`, `reload_sound` and `land_sound` have **no** fallback — an
  unauthored key is simply silent, because there is no honest generic clip for
  "a blade connected" and borrowing one would make every swing in every level
  sound identical.
- `hit_sound` fires only on a connect that cleared geometry. A swing that stops
  at a wall runs no `on_hit` and makes no connect sound.
- `run_sound` is chosen off the sprint key while the player is moving;
  `walk_sound` otherwise. Both are loops.

All of these are editable from the AngelEd Property Window — see
[Editor-Usage.md](Editor-Usage.md).

### Action Blocks

Scripts attached to specific events:

| Action | Trigger |
|---|---|
| `on_use` | Player selects the entity from hotbar and presses E/Enter |
| `on_equip` | Entity is equipped |
| `on_unequip` | Entity is unequipped |
| `on_fire` | Ranged weapon fired (before projectile spawn) |
| `on_swing` | Melee weapon swung (before range check) |
| `on_hit` | Melee swing connects with a target |
| `on_reload` | Weapon reload triggered (auto or manual via R key) |
| `on_collect` | Entity is picked up |
| `on_enter` | Player enters a zone/skyzone volume |
| `on_exit` | Player exits a zone/skyzone volume |
| `on_tick` | Called every frame while active |
| `on_patrol` / `on_chase` / `on_return` / `on_death` | Pawn FSM state transitions (requires a pawn-named `.ozls`, e.g. `Walker.ozls`) |

## Opcodes

### Variable Operations

| Opcode | Syntax | Description |
|---|---|---|
| `var` | `var x = 5` | Declare int (no decimal) or float (has decimal) |
| `$` | `$x = 42` | Assign to runtime variable (creates if not exists) |
| `$ +=` | `$x += 5` | Add and assign |
| `$ -=` | `$x -= 3` | Subtract and assign |
| `$ *=` | `$x *= 2` | Multiply and assign |
| `$ /=` | `$x /= 2` | Divide and assign |
| RHS arithmetic | `$x = $x - 1` | Assignments support one binary op (`+ - * /`) with variables/numbers |

Variables are referenced in conditions with `$` prefix (e.g., `$health > 0`).
`$flag<idx>` (e.g. `$flag0`) reads the instance's toggle flags set by `wtflag`/`toggle_flag`.

### Control Flow

| Opcode | Syntax | Description |
|---|---|---|
| `if` / `else` / `endif` | `if ($x == 0) ... endif` | Conditional execution (brace style `if (...) { ... }` also supported) |
| `goto` / `jump` | `goto label_name` | Jump to a label |
| `stop` / `end` | `stop` | Halt script execution |
| `say` | `say "hello"` | Print to log |
| `set_cooldown` | `set_cooldown 1.5` | Set cooldown timer in seconds |

### Flags

| Opcode | Syntax | Description |
|---|---|---|
| `wtflag` | `wtflag idx val` | Write toggle flag (0-63) |
| `rtflag` | `rtflag idx` | Read flag value into `$result` |
| `toggle_flag` | `toggle_flag idx` | Flip a toggle flag (0-63) |

Hotbar/equipment instance flags are persisted in save games (`flags=` section). Zone-instance flags are session-only.

### World Interaction

| Opcode | Syntax | Description |
|---|---|---|
| `set_fog` | `set_fog r g b density` | Set fog color and density |
| `restore_fog` | `restore_fog` | Restore fog to world default |
| `set_ambient` | `set_ambient r g b` | Set ambient light color |
| `restore_ambient` | `restore_ambient` | Restore default ambient |
| `set_skybox` | `set_skybox "name"` | Set skybox by name |
| `restore_skybox` | `restore_skybox` | Restore default skybox |
| `play_sound` | `play_sound "path"` | Queue sound playback (package-aware: resolves `.ozsnd` via `SoundManager::PlayScriptSound`) |
| `msg` | `msg "text"` | On-screen text message |
| `heal` / `damage` | `heal 25` | Immediate player health delta |
| `playerstat` | `playerstat health -= 5` | Deferred write to a player stat |
| `consume` | `consume` | Mark consumable as used (removes from hotbar) |
| `spawn_pawn` | `spawn_pawn "Walker" x y z` | Spawn an NPC |
| `spawn_pickup` | `spawn_pickup "Coin" x y z [respawn]` | Spawn a pickup |

### Gameplay / event opcodes (Phase 1)

These queue a deferred `ScriptCommand` the host applies after the action body
runs. Engine-generic ones are applied by `LightningEntityManager`; the ones that
need the camera or world loader are drained by `Core.hpp`.

| Opcode | Syntax | Effect |
|---|---|---|
| `emit_event` | `emit_event "zone.enter" [int]` | Publish an `EventBus` event (topic, optional int). The bridge runs `on_<topic>` on the def named by the event's `s0`. |
| `objective` | `objective <id> += 1` | Advance an `ObjectiveSystem` objective (`=`, `+=`, `-=`); emits `objective.progress`/`objective.complete`. |
| `set_worldflag` | `set_worldflag <name> <value>` | Write a cross-instance world flag (survives across entities, serialised with the world). |
| `get_worldflag` | `get_worldflag <name>` | Read a world flag into `$result`. |
| `give_xp` | `give_xp 50` | Add player XP. |
| `grant_item` | `grant_item "Medkit" [qty]` | Spawn an entity def and place it in the hotbar. |
| `teleport` | `teleport x y z` | Move the player (engine Y-up space). |
| `change_world` | `change_world "Dust_Ravine"` | Request a world load. |
| `apply_area_damage` | `apply_area_damage <radius> <damage>` | Damage pawns around the player. |
| `spawn_projectile` | `spawn_projectile <speed> <damage> <life> [count] [spread]` | Fire a projectile from the player. |
| `play_clip` | `play_clip "Attack"` | Reserved: animation clip (logged no-op until Phase 2). |

### Events (subscription)

The engine publishes facts on the `EventBus`; a def subscribes by declaring an
action named `on_<topic>` with `.` replaced by `_`:

| Topic | Action | Emitted when |
|---|---|---|
| `world.load` | `on_world_load` | A world finishes loading |
| `entity.death` | `on_entity_death` | A pawn's FSM enters DEAD (`s0` = pawn def name) |
| `entity.chase` / `entity.patrol` / `entity.return` | matching `on_entity_*` | Pawn FSM transitions |
| `pickup.collect` | `on_pickup_collect` | A pickup is collected (`s0` = pickup def name) |
| `player.hurt` / `player.death` | `on_player_hurt` / `on_player_death` | Player takes damage / dies |
| `trigger.enter` / `trigger.exit` | `on_trigger_enter` / `on_trigger_exit` | Player crosses an OZONE `trigger` volume (`s0` = trigger def name) |

### Triggers (OZONE)

A `trigger` line places a volume whose meaning is a `: trigger` def:

```
trigger Trigger_EnterVolume minX minY minZ maxX maxY maxZ [once] [team=N]
```

On the player's entry/exit the engine emits `trigger.enter` / `trigger.exit`
with the def name; `once` latches after the first entry.


## Conditions

Used in `if` statements within action blocks (both styles work):

```
if ($health <= 0) {
    say "Player defeated"
    end
}
```

```
if ($result == 0)
    say "first time"
endif
```

Supported operators: `==`, `!=`, `>`, `<`, `>=`, `<=`, plus `&&`, `||` and
parentheses (e.g. `if (($a == 1 || $b == 2) && $c > 0)`).

## Example: Ranged Weapon (automag)

```
entity "automag" : weapon {
    mesh = "automag_lvl1.obj"
    texture = "automag_lvl1_texture.png"
    stats {
        damage = 15
        fire_rate = 0.4
        magazine = 12
        reload_time = 2.0
        projectile_lifetime = 2.5
    }
    actions {
        on_fire {
            say "Bang! (15 damage)"
            set_cooldown 0.4
        }
        on_reload {
            say "Reloading..."
        }
    }
    variants {
        "lvl1" { mesh_override = "automag_lvl1.obj" }
        "lvl2" { mesh_override = "automag_lvl2_alt.obj" }
        "lvl3" { mesh_override = "automag_heavy_rifle_lvl3.obj" }
    }
}
```

`variants` are resolved when the instance spawns: the engine parses the `lvlN`
name for its tier and picks the highest tier that does not exceed the player's
level (clamped to the highest declared tier). A level-1 player therefore always
sees `lvl1`. The chosen variant applies to both the world pickup and the
first-person view-model. Names without an `lvlN` prefix are treated as tier 0.
Override `mesh_override` paths are resolved relative to the def's own directory,
so include the file extension.

### Declarative zone environment

A `skyzone` entity can author its atmosphere directly rather than through
`set_fog` / `set_ambient` opcodes. These are applied when the player enters the
zone:

```ozls
entity "skyzone_citadel" : skyzone {
    skybox = "..."
    music = "..."            // asset prefix, handed to SoundManager::PlayWorldMusic
    fog_color = (0.62, 0.52, 0.46)
    fog_density = 0.0022
    ambient_light = (0.50, 0.46, 0.46)

    actions {
        on_enter { msg "..." }
        on_exit  { restore_fog   }
    }
}
```

`fog_color`, `fog_density` and `ambient_light` are applied on `on_enter`; the
`restore_fog` / `restore_ambient` / `restore_skybox` opcodes revert them to the
level defaults. Note the vec3 syntax is `name = (x, y, z)` — a `vec3` prefix
does not parse.

## Example: Zone with Damage Loop (acidpool)

```
entity "acidpool" : skyzone {
    actions {
        on_enter {
            wtflag 0 1
            var timer = 60
            play_sound "GameData/Audio/Sounds/Entities/Hurt.mp3"
            msg "Acid! The pooled run-off burns your boots!"
        }
        on_tick {
            if ($flag0 >= 1) {
                $timer = $timer - 1
                if ($timer <= 0) {
                    playerstat health -= 5
                    var timer = 60
                }
            }
        }
        on_exit {
            wtflag 0 0
            msg "You scramble clear of the acid."
        }
    }
}
```

## Integration

- Entity `.ozls` files are parsed by `LightningScriptParser` into `EntityDef` structs (recursively scanned from all of `GameData/` + packages)
- `LightningEntityRegistry` stores all registered definitions
- `LightningEntityManager` manages runtime instances with individual `LightningScriptContext` per instance
- Instances start idle — action bodies run only when triggered (spawn does not auto-execute)
- Hotbar integration: slot selection triggers `on_equip`/`on_unequip`; use triggers `on_use`
- Zone triggers: `PawnSystem` calls zone actions on entry/exit; pawn FSM transitions dispatch the matching `on_*` action
