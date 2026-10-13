# Editor Usage (AngelEd)

AngelEd is a Windows-only level editor combining Win32 native panels with a raylib 3D viewport. It requires `_WIN32` — use w64devkit or MSYS2 to build.

## Getting Started

Launch `System\AngelEd.exe`. The editor opens with:

- **3D viewport** — raylib render window (center)
- **Menu bar** — File/View/Camera/Settings/Help menus
- **Win32 panels** — dockable side windows (Model Browser, Texture Manager, etc.)

## File Formats

Supports the OZONE (`.ozone`) world format. Open/Save dialogs accept `.ozone`. OZONE export includes CSG brush geometry and entity definitions in a combined plain-text format.

## Menu Bar Reference

### File
| Item | Shortcut | Action |
|---|---|---|
| New | N | Create a new world |
| Open... | O | Load a world from file |
| Save | S | Save the current world |
| Save As... | | Save to a new path |
| Play Test | P | Save + compile the world, then launch Angels95.exe with it (b60: playtest saves and compiles before launch so the client never runs a stale export) |
| Exit | Q | Close editor |

### View
| Item | Shortcut | Action |
|---|---|---|
| Model Browser | F5 | Toggle model browser |
| Sound Manager | F6 | Toggle sound manager |
| Texture Manager | F7 | Toggle texture manager |
| Pawn Manager | F8 | Toggle pawn manager |
| Script Manager | F9 | Toggle script manager |
| Zone Properties | F12 | Toggle zone/env panel |
| Node Panel | | Toggle node placement |
| Pickups | F10 | Toggle pickup panel |
| Light Properties | | Toggle light properties |
| Heightmap Editor | H | Toggle heightmap editor |
| World Graph Explorer | | Toggle world graph |

### Camera
| Item | Shortcut | Action |
|---|---|---|
| Reset Camera | Home | Reset to default position |
| Top | Numpad 7 | Orthographic top-down view |
| Bottom | Numpad 1 | Orthographic bottom-up view |
| Right | Numpad 3 | Orthographic right view |
| Left | Numpad 9 | Orthographic left view |
| Perspective | Numpad 5 | Restore perspective view |

## Entity Selection (Click + Right-Click)

Click on any entity in the 3D viewport to select it (highlighted red):

- **Brush** — click on CSG collision geometry / OZONE primitives
- **Model** — click on a placed 3D model
- **NPC** — click on a pawn's billboard
- **Pickup** — click on a pickup node
- **Light** — click on a light node
- **Zone** — click on a zone volume boundary
- **Spawn** — click on a player start node

**Right-click** a selected entity to open the native context menu:

| Option | Action |
|---|---|
| Properties | Opens the Properties panel with entity details |
| Delete | Removes the selected entity from the world |
| Duplicate | Creates a copy offset 2 units on X+Z |
| Append AutoConvex Collision | **Brush or Mesh only.** Voxelises the selection into convex collision boxes and appends them to the world (see [AutoConvex collision](#autoconvex-collision)) |
| Apply Texture to Surface | Applies the currently selected texture (if a texture is active in the Texture Manager and the target is a Brush or Model) |

Right-click drag (without an entity under the cursor) orbits the camera.

## AutoConvex collision

The collision world is **AABB-only** (`CsgProcessor` in `Source/Physics/OzBsp.hpp`
consumes nothing else). That has two consequences you will hit:

- A placed `Mesh.Static` / `Mesh.Skeletal` prop has **no collision at all** —
  the player walks straight through it.
- A brush only ever gets the AABB of its *generated primitive*, so a concave
  brush over-blocks.

**Append AutoConvex Collision** (right-click a Mesh or Brush) closes both gaps.
It slices the selection's geometry into a grid of small boxes, keeps only the
cells a triangle actually passes through, and appends each one as a real
`add box` brush in the `.ozone`.

Notes:

- **Cell size is derived from the selection** (`extent / 16`, floor 0.25), so a
  crate and a castle wall both get a sane budget.
- **Budget: 2048 boxes per run.** Over budget the command refuses and logs the
  reason rather than appending a partial hull — a missing box in a collision
  wall is the exact failure this feature exists to prevent.
- **Skeletal meshes are voxelised in their bind pose.** Vertex poses are
  uploaded per frame at draw time, so no posed geometry exists to sample.
- Proxies are written as ordinary `add box ... flags=16` lines, so they survive
  save/reload. Use the **Collision** sidebar toggle to see them; they are never
  drawn in game.

## Collision visualisation

The **View → Collision** sidebar button (bottom of the stats sidebar) toggles
the post-CSG collision wireframes — i.e. exactly what the player will stand on.
It is the only way to see a brush whose render mesh looks solid but whose
collision volume does not exist.

When a **Sub / Intersect / De-Resc** brush produces *no* collision volume the
sidebar counter reads `Collision: N vols << NO SOLID` and the wireframes turn
red. See [Sub needs a solid](#sub-needs-a-solid) below.

## Lighting Modes

| Button | Action |
|---|---|
| Lit | Models render with LitFogShader (lighting + fog) |
| Unlit | Models render with default unlit shader |
| Wire | Wireframe view mode |
| Sky | Show/hide the viewport skybox backdrop (b64/b67) |

Toggle between modes from the ViewMode toolbar combo.

## Viewport skybox (b64–b68)

The editor renders the world skybox cube as a camera-following backdrop with
cap-texture sides. Resolution order (highest first):

1. `levelinfo` `skyboxTexturePath` (Fog tab) — saved skybox from the `.ozone`
2. world `Models/Skybox.png` — fallback only

The Fog tab's **Apply Skybox** (with **Browse** or **Use Active Tex**) commits
the selection; a failed load is logged.

## CSG Brushes

The CSG Brushes panel provides:

- **Primitive buttons**: Box, Cylinder, Sphere, Pyramid, Plane
- **Operation buttons**: Solid (0), Add (1), Sub (2), Intersect (3)
- **Edit fields**: Position (X/Y/Z), Size (W/H/D), Rotation, Scale
- Clicking an operation button commits the current brush immediately at the
  camera target (a 4×4×4 box by default)

The CSG operation value is stored per brush and fed to the backend `CsgProcessor` for **collision** geometry (`OzoneLoader::RebuildCollisionVolumes`); render meshes stay whole (no render-time CSG carving). The OZONE export preserves each brush's `add`/`sub`/`intersect` op.

### Sub needs a solid

> **Sub / Intersect / De-Resc are _modifiers_, not shapes.** They subtract from,
> or intersect with, solids that **already exist** in the world. A brush's render
> mesh draws regardless of its op, so a `sub` floor looks exactly like a floor
> while contributing **zero** collision volume — and the player falls straight
> through it.

If you commit a modifier op and the world has nothing for it to act on, AngelEd
logs a warning, raises a dialog, and marks the sidebar counter
(`Collision: N vols << NO SOLID`). To fix it: add a **Solid** (or **Add**) brush
for the volume you actually want, then use **Sub** to carve the openings.

> Never author a whole floor as `sub`. Build openings from separate pier/lintel
> brushes around it instead — `sub` carves collision only, never the render mesh.

## Panels

### Asset scope (Model Browser + Texture Manager)
Both panels group assets as the same two-root tree instead of one flat list, so it is
always obvious whether an asset is an editable file or a package record:

- **`(GameData)`** — loose files under `GameData/`, nested by their real folder
- **`(Packages)`** — assets that only exist inside a `.oz*` package

A `.oz*` file is the only thing treated as a package; anything reachable on disk is a
real file even when a package also holds a same-named copy (real files win).

Both panels have a **Search** box that re-filters the already-loaded entries as you
type (no filesystem hit per keystroke). While a search is active, matching folders
auto-expand and folders with no matches are pruned.

### Model Browser
- Scope tree of all `.obj`/`.gltf`/`.glb`/`.iqm`/`.vox`/`.m3d` files from `GameData/` and packages
- Previews selected model in a 256x256 render texture
- Click a leaf to place it in the world; clicking a folder does nothing
- **Import** packs the chosen mesh (and its companion texture, if present) straight into
  `System/Data/imported_models.ozpak` and hot-loads it, so it shows up under
  `(Packages)` immediately. The package is appended to, so repeated imports accumulate.
  No loose file is written to `GameData/`.
- **Export** writes the selected mesh back out to a path of your choosing

### Texture Manager
- Scope tree (left) + **grid view** with 64x64 thumbnail previews in a custom scrollable
  control (right). The tree selects the *scope* the grid shows: pick a folder and the grid
  lists that subtree, pick nothing and it lists everything. The "Source:" label reports how
  many textures are in scope.
- Click a texture to see full-size preview and file info
- Select target model from dropdown (populated from loaded world models)
- Click **Apply** to set texture on model; **Apply to All** checkbox applies to all model instances
- **Add Package** button loads additional `.oztex`/`.ozpak` files at runtime
- **Import Textures** packs selected image file(s) into `System/Data/imported_textures.oztex`
  (appended, then hot-loaded) rather than dropping loose files into a world folder.

> **Imported textures are free-placement assets, not tileset entries.** Use them with
> `tex=<path>` on `Mesh.Static` / `Mesh.Skeletal` entities, or as a model's texture.
> They **cannot** be used as a brush `texSlot` — that argument is a *bare positional
> float* selecting a tileset slot (1-based, ordered by filename in
> `<world>/oztex/tileset/`), and only textures in that folder count. Writing `texSlot=3`
> does nothing and you silently get auto-selection instead (`h<1` -> slot 1, else slot 2).
> To make a texture a tileset slot, add the file to the world's `oztex/tileset/` folder on
> disk and reopen the world.

### Sound Manager
- Lists `.wav`/`.mp3`/`.ogg` files organized by category tabs: **SFX** / **Music** / **Ambience**
- Source path shown for each category
- Volume slider for preview volume
- **Loop** checkbox for continuous playback
- **Play** / **Stop** / **Refresh** buttons

### Pawn Manager
- Hierarchical **tree view** of all actor types:
  - **PlayerPawn** > AngelPlayer (player start)
  - **EnemyPawn** > registered NPC defs (Walker, Skaarj, Brute, Floater, etc.)
  - **InventoryPawn** > Pickups (from LightningScript registry) + Weapons
  - **Volume & Node Markers** > PlayerStartNode, EmitterNodes (Sound/Music), ZoneVolumeNode types (Water/Ladder/Sky/Reverb/GameplaySound)
- Double-click a leaf node to view entity info
- **Spawn Selected** places the chosen actor at camera position
- **Refresh** reloads the tree from current definitions

### Script Manager (b60)
- Lists `.ozls` entity definitions from the LightningScript registry (scanned
  from all of `GameData/` + packages) with name, type and source path
- Double-click to view the def body; **Edit / New / Delete / Reload** open the
  file in the configured external editor and re-scan the registry after edits
- **Properties** opens the Entity Properties panel on the selected def. This is
  the only route to a def with no instance in the open world — `Player.ozls`
  above all, whose `jump_sound` / `hurt_sound` keys need editing somewhere

### Zone Properties / Environment Settings
- **Fog**: color, density, start/end distance
- **Ambient**: color, intensity
- **Game Type**: Singleplayer, Coop, Etheral Match, Angel Team Game, Angel Run, Capture the Orb, Time Shift
- **Max Players**, **Respawn Time**, **Time Limit**, **Score Limit**, **Friendly Fire**
- **Particles**: type (None/Snow/Rain/Void Realm/Psychic Realm), density, speed, color, wind
- **Skybox**: custom skybox texture path; levelinfo path takes priority over
  `Models/Skybox.png` (see *Viewport skybox* above)
- Per-zone overrides for fog/ambient/reverb when editing zone volumes

### Pickup Panel
- Select and place pickup nodes by type (from LightningScript entity registry)
- Configures `actionPickupType` for the main loop

### Node Panel
- Place node markers: Player Start, NPC Spawn, Point Light, Zone Volume
- Configures `actionNodeType` for the main loop

### Heightmap Editor
- Browse for grayscale heightmap image
- Browse for terrain texture overlay
- Configure: position (X/Y/Z), size (Sx/Sy/Sz), scale
- Click **Generate** to build terrain mesh

### Light Properties
- Configure point lights: color (R/G/B), intensity, radius
- Light type: directional, point, spot
- Light effect: none, watery, torch, fire, lamp
- Toggle: flare, corona

### World Graph Explorer
- Lists all placed models in the current world
- Shows each model's position, rotation, scale, and name
- Click to select/jump to that model in the viewport

### Properties Panel
- Context-sensitive panel showing selected entity details
- Edit position (X/Y/Z), scale, rotation
- For brushes/zones: edit size (W/H/D)
- Texture mapping: U/V scale and offset
- **Def-aligned rows (b60)**: entities backed by a `.ozls` def or `PawnDefs/*.cfg`
  show read-only stats + hook rows straight from the def, with
  instance overrides and zone/portal fields editable below them
- **Editable `.ozls` stats**: an *Edit stats* section lists every documented
  stat for the def's entity type, and **Apply** writes the changed rows back to
  the source `.ozls`. See below.

#### Editing `.ozls` stats

The *Edit stats* section is generated from a **per-entity-type schema**, not
from the keys the def already has — a weapon therefore shows every documented
stat including ones it has never set. Rows are generated rather than discovered
because otherwise the panel could only ever edit what already exists.

- Only rows you actually **changed** are written. A blank, unauthored row is not
  an erase, so opening a panel and pressing Apply never strips a def.
- Clearing a field removes that key from the file.
- Sound rows have **Preview** (plays it) and **Browse...**.
- `Browse...` **rejects a filename containing a space**. A `.ozls` stats value
  is stored verbatim and stops at the first space, so such a path could never
  resolve — rename the file instead.
- Sound paths must be written **unquoted**. The parser does no quote handling on
  stats strings, so `"my gun.wav"` arrives at the runtime with its quotes and
  fails to resolve.
- Writes are **surgical line patches**, not a re-serialisation. Comments, key
  order, and any key the editor does not recognise all survive; an edit is a
  one-line diff. New keys are appended at the end of the `stats` block.
- Stats the schema does **not** know about appear in the read-only dump above,
  under *".ozls stats (keys with no edit field)"*, so nothing authored by hand
  is hidden. Keys the schema **does** know about are omitted from that dump —
  they are already shown in the *Edit stats* section below, where you can change
  them, and repeating them there only doubled the panel's height.
- The panel **scrolls**. A weapon def generates ~30 stat rows, so it is capped
  to the monitor's work area with a vertical scrollbar rather than growing off
  the bottom of the screen. The mouse wheel works over the panel.
- **Packaged defs are read-only.** A def resolved from a `.oz*` package has no
  source file to write, so the section says so instead of accepting edits that
  cannot be saved. Edit the `GameData` source and repack.

**Reaching a def with no world instance:** a def with no placed instance cannot
be reached from a selection. Use the Script Manager's **Properties** button
instead — this works for any def by name and skips the per-instance
position/rotation rows.

#### Which selection reaches which def

The Properties panel resolves a def per selection type:

| Selection | Def resolved by | Editable stat schema |
|---|---|---|
| NPC | the pawn's `defName` | `kPawnStats` (+ read-only `PawnDefs/*.cfg` rows) |
| Pickup | the pickup's `typeName` | whatever the def's own type maps to — `: weapon` → `kWeaponStats`; `Player.ozls`-style `: upgrade` → `kPlayerStats`; `: consumable` → read-only |
| Zone | the zone's `name=` (skyzone def) | read-only (`SKYZONE` has no schema) |
| **PlayerStart** | **the fixed name `Player`** | `kPlayerStats` |
| **Light** | **the light's `name=`** | `kLightStats` |
| Brush / Mesh / Portal / Emitter / PathNode / WindZone | *(instance fields only, no def)* | — |

`playerstart x y z yaw` carries no name, so a PlayerStart always resolves the
`Player` def (`GameData/Tex/Global/Models/Player.ozls`) — the player the spawn
points at is the same def whichever spawn you select.

A light's `name=` resolves an `: light` def exactly the way a zone's `name=`
resolves its skyzone def:

```
light point 0 0 4 255 180 90 1.2 12 name=torch flare=1
```

> **A light def is a DEFAULTS layer.** A value the light line authored always
> wins. Only keys the line cannot express take effect: `effect` / `flare` /
> `corona` when the line omitted the matching kwarg, plus `period`,
> `cast_shadow`, `is_static`, `inner_cone`, `outer_cone` which have no line
> syntax at all. `intensity`, `radius` and `color` are positional on every light
> line and are deliberately **not** in the schema — offering them would suggest
> an edit the runtime ignores.

### Animation Tool (b74)
Opens from the toolbar **Anim** button. Authors **vertex-keyframe (morph)**
animation for `Mesh.Skeletal` entities — the text `.ozanim` format
(`Source/Package/Anim/`, raylib-free):

- **Clip list** — New/Delete clips, per-clip FPS + loop toggle
- **Transport** — Play / Pause / Stop + timeline scrub
- **Convert to Animated** (Mesh properties / Entity Properties) — writes a
  default clip under `GameData/Environment/Anims/` and switches the entity to an
  animated mesh
- **Edit Verts** — vertex picking (click / Shift-add / Sel All); keyboard
  deform moves verts with **U/J/H/K/Y/I** (X/Z/Y) and rotates with **O/L**;
  **Ctrl+Z/Y** undo/redo is tool-scoped
- **Add Key / Del Key** — capture or drop sparse vertex offsets at the current
  timeline position; playback is client-cosmetic (not networked)

## Keyboard Shortcuts

| Key | Action |
|---|---|
| U/J | Move placement ghost X |
| H/K | Move placement ghost Z |
| Y/I | Move placement ghost Y |
| O/L | Rotate placement ghost |
| T/G | Scale placement ghost up/down |
| Enter | Commit placement |
| Double-click | Commit placement |
| Middle Mouse | Pan camera |
| Shift+Middle Mouse | Pan camera vertically |
| Alt+Middle Mouse | Orbit camera |
| Scroll Wheel | Dolly camera (zoom) |
| Home | Reset camera position |
| F5 | Model Browser |
| F6 | Sound Manager |
| F7 | Texture Manager |
| F8 | Pawn Manager |
| F9 | Script Manager |
| F10 | Pickup Panel |
| F11 | Toggle Fullscreen |
| F12 | Zone Properties |
| H | Heightmap Editor |
| C | Toggle collision visibility |

## World Saving

Worlds are saved in OZONE text format. OZONE export includes CSG brush primitives, heightmap, and all entity types (player starts, pickups, NPCs, zones, emitters) with per-zone environment overrides.

## History (Undo / Redo)

Full-document undo/redo via **Edit > Undo/Redo** or **Ctrl+Z** / **Ctrl+Y** (Ctrl+Shift+Z also redoes). Each step snapshots the whole world (geometry, entities, level metadata, heightmap) using the OZONE export, so place/delete/duplicate, property edits, lighting/portal/zone changes, texture application, heightmap generation, terrain painting, spawns and gizmo drags are all reversible. History is cleared when a world is opened or a new one is created. The Animation tool's vertex-edit undo (Ctrl+Z/Y while editing verts) is separate and takes precedence there.

## Known Limitations

- Lighting toggle (Lit/Unlit) does not actually unset shader from model materials
- Render meshes are not CSG-carved (CSG booleans process collision volumes only)
- Undo/redo covers world state only; camera and selection are not restored
- No test-play save prompts ("Reload world from playtest changes?")
- Model/Texture preview rendering requires the raylib viewport to be focused
- Lighting effects (watery, torch, fire, lamp) are UI only — not rendered in viewport
