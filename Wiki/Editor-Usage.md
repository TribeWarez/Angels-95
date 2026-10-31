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
| New | Ctrl+N | Create a new world |
| Open... | Ctrl+O | Load a world from file |
| Save | Ctrl+S | Save the current world |
| Save As... | Ctrl+Shift+S | Save to a new path |
| Play Test | Ctrl+P | Save + compile the world, then launch Angels95.exe with it (playtest saves and compiles before launch so the client never runs a stale export) |
| Exit | Ctrl+Q | Close editor |

Every hint in the menu bar comes from the same remappable table the shortcuts read
(`AngelEd/Source/EditorKeys.hpp`), so a rebind cannot leave a menu advertising a
key that no longer does anything. Edit it from **Help → Keyboard Shortcuts**.

### View
| Item | Shortcut | Action |
|---|---|---|
| Model Browser | F5 | Toggle model browser |
| Sound Manager | F6 | Toggle sound manager |
| Texture Manager | F7 | Toggle texture manager |
| Pawn Manager | F8 | Toggle pawn manager |
| Script Manager | F9 | Toggle script manager |
| Material Graph Explorer | | Material graph / bake panel |
| Node Panel | | Toggle node placement |
| Pickups | F10 | Toggle pickup panel |
| Heightmap Editor | H | Toggle heightmap editor |
| World Hierarchy | F3 | Toggle the World Graph |
| Level List / Campaign | | Level + campaign rows |
| Show Ambient | | Toggle the lit-viewport ambient contribution on/off (preview only) |
| Show Fog | | Toggle the lit-viewport fog (world + sky horizon) on/off (preview only) |
| Save Pane Layout | | Persist every panel's geometry to `System/AngelEd.ini` now |
| Reset Pane Layout | | Restore the built-in panel geometry |

There are **no Zone Properties / Light Properties windows**. A zone's rows live in
Entity Properties (right-click a zone) and the level-wide state lives in the
WorldGraph's *Map (level)* row; both old windows duplicated that state into a
global an Apply could silently drop.

### Camera
| Item | Shortcut | Action |
|---|---|---|
| Reset Camera | Home | Reset to a default 3/4 view |
| Top | Numpad 7 | Orthographic top-down |
| Bottom | Numpad 1 | Orthographic bottom-up |
| Right | Numpad 3 | Orthographic right |
| Left | Numpad 9 | Orthographic left |
| Front | Numpad 2 | Orthographic front |
| Perspective | Numpad 5 | Restore this pane's perspective view |

Camera items and the toolbar's **Top / Side / Down / Persp** chips act on the
**active pane only** — each viewport keeps its own camera *and* its own preset
state, so a 2x2 layout can be Top + Front + Side + Persp like UnrealEd. The chip
that is lit is derived from the camera, never stored, so an orbit cannot leave the
chip claiming something the view is not.

## Entity Selection (Click + Right-Click)

Selection happens on the **mouse press**, exactly like UnrealEd
(`UnEdClick.cpp` resolves the hit the moment the button goes down, then lets the
drag move whatever ended up selected). A press that also hits a gizmo handle is a
transform, not a pick.

What is under the cursor is resolved against a **5x5 pixel window** (centre plus
four 2 px samples), not an infinitely thin ray, and the **nearest** hit wins —
that is what makes thin targets (a path node, a light billboard, a fence prop, a
pawn's arm) clickable at all.

| Click | Result |
|---|---|
| **Left** | Select the entity under the cursor (highlighted red) |
| **Left again on it** | Deselect it |
| **Shift+Left** / **Ctrl+Left** | Extend/toggle the multi-selection (Ctrl only while Unreal-style brush movement is **off**) |
| **Left double-click** | Select + open Entity Properties |
| **Left on empty space** | Clear the selection (modifiers keep it, so a missed extend-click cannot wipe the set) |
| **Right** | Select (keeping an existing multi-selection) + open the context menu |

Selectable kinds: **Brush** (OZONE primitive), **Model**, **NPC**, **Pickup**,
**Light**, **Zone**, **Spawn** (player start), **Portal**, **Mesh.\***, **Particle
emitter**, **Path node**, **Wind zone**. Objects that are drawn invisible (an
AutoConvex collision proxy, a hidden face) are never pickable — the picker and
every draw path share one predicate, so what you see is what you can select.

**Right-click** a selected entity to open the native context menu:

| Option | Action |
|---|---|
| Properties | Opens the Properties panel with entity details |
| Delete | Removes **every selected** entity (one undo step) |
| Duplicate | Copies the whole selection, offset 2 units on X+Z, and selects the copies |
| Snap to Ground | Drop each selected entity onto the surface below it (see [Snap to Ground / Side](#snap-to-ground--side)) |
| Snap to Side | Push each selected entity against the nearest brush face along the view's right axis |
| Append AutoConvex Collision | **Brush or Mesh only.** Voxelises the selection into convex collision boxes and appends them to the world (see [AutoConvex collision](#autoconvex-collision)) |
| Apply Texture to Surface / to Selection | Applies the active texture. A **Brush** applies to the picked faces (or brush-wide when no face was picked), a **Model** or a **Mesh.\*** prop takes it as its texture. Applies to **every** selected entity |

Right-click drag (without an entity under the cursor) orbits the camera.

## Snap to Ground / Side

Two toolbar chips (row 2, next to the view presets) and two context-menu items.
Both operate on the whole selection and each entity is resolved on its own, so a
crate on a ledge and a barrel on the floor each land correctly.

- **Ground** drops an entity until its bounding box rests on the highest surface
  under its footprint. The heightmap is the floor where no brush is below it. A
  surface *above* the entity is never used, so standing under a roof does not
  snap you up onto it.
- **Side** pushes along the view's right axis (snapped to the nearest cardinal,
  because collision is AABB-only) until the entity touches the nearest brush face.
  The push is capped at 200 units so it cannot teleport an object across the
  level.

Both are single undo steps. Implemented in
`AngelEd/Source/Subsystems/SnapMath.hpp` (query) and `Subsystems/Selection.cpp`
(apply).

## Transform gizmo (Move / Scale / Rotate)

Pick a tool with the sidebar's **Cam / Move / Scale / Rotate** buttons.

- **Move** — drag an axis handle for a constrained move, a plane handle for a
  two-axis move, the centre sphere for a camera-plane move, or simply drag the
  selected object.
- **Scale** — drag an axis handle, or use the wheel. One handle length of drag is
  roughly 2x.
- **Rotate** — drag the yaw ring, **or anywhere inside it** (the ring's projected
  radius shrinks with distance), or use the wheel.

An axis or plane that is edge-on to the camera has no ray intersection, so the
drag falls back to a screen-space projection of the handle's direction instead of
doing nothing. Grid snapping is anchored at the position the drag *started*, so
grabbing an off-grid object never teleports it onto the grid. Shift = finer step,
Ctrl = snap at the toolbar step.

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

**View → Show Ambient / Show Fog** are independent lit-preview switches. They
mask the shader uniforms (including the sky's horizon fog) without changing the
level, zone or `.ozone` data, and the choice is persisted in `[Views]` in
`System/AngelEd.ini`. Use them to inspect geometry in a dark or heavily fogged
area without editing the world.

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

### Zone + level environment (Entity Properties)

The standalone **Zone Properties** window is gone. A zone volume's rows are in
Entity Properties (right-click the zone): **Fog** (colour, density, start/end),
**Ambient** (colour, intensity), reverb, and the zone's type/intensity. The
*level-wide* state lives in the WorldGraph's **Map (level)** row:

- **Game Type**: Singleplayer, Coop, Etheral Match, Angel Team Game, Angel Run, Capture the Orb, Time Shift
- **Max Players**, **Respawn Time**, **Time Limit**, **Score Limit**, **Friendly Fire**
- **Particles**: type (None/Snow/Rain/Void Realm/Psychic Realm), density, speed, colour, wind
- **Skybox**: custom skybox texture path; the levelinfo path takes priority over
  `Models/Skybox.png` (see *Viewport skybox* above)

Both halves apply **live** (the viewport previews fog/ambient as you edit) and
push a History entry, so a mistyped value is one Ctrl+Z away.

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

### Lights (Entity Properties)

The standalone **Light Properties** window is gone; right-click a light. Rows:
colour (R/G/B), intensity, radius, type (directional/point/spot), effect (none,
watery, torch, fire, lamp), flare/corona toggles, and — for spot lights — the
inner/outer cone angles in degrees (stored as cosines, which is what the shader
compares against). A light's `.ozls` def (matched by its `name=`) supplies the
defaults; a value authored on the OZONE line always wins.

### World Hierarchy (WorldGraph)

- A grouped tree of the live world: Map (level), Brushes, Models, Meshes, NPCs,
  Pickups, Lights, Zones, Portals, Player Starts, Emitters, Particles, Path Nodes,
  Wind Zones
- Filter box, **Reveal** (viewport selection → tree row, without stealing focus)
  and **Frame Selection** (tree → camera)
- Right-click a row for the same context menu the viewport offers (Properties,
  Delete, Duplicate, Snap to Ground/Side, Apply Texture)
- Derived state: it is rebuilt from a dirty flag at the frame's choke points, not
  polled every frame

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
  - The window is sized from the **content** height converted to a window height,
    so the non-client frame/caption no longer eats the bottom margin and the
    **Apply / Close row is always fully visible**.
  - The scroll range is published so the **last page is reachable**; the bottom
    of a long def is not stranded below the visible range.
  - Rows use one right margin that reserves room for the scrollbar, so no value
    field or colour channel hides underneath it when the bar appears.
- **Packaged defs are read-only.** A def resolved from a `.oz*` package has no
  source file to write, so the section says so instead of accepting edits that
  cannot be saved. Edit the `GameData` source and repack.
- **`trigger` / `event` defs are read-only dumps by design.** Neither type has
  a runtime-consumed stat schema (`trigger` volumes carry their box, `once` and
  `team` on the OZONE `trigger` **line**; the def only holds hook actions), so
  their def section is a labelled read-only dump rather than an *Edit stats*
  block. The rule is deliberate: an editable schema row requires a **verified
  runtime consumer** — an offered-but-ignored field is worse than a missing
  one.

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
- **Transport** — Play / Pause / Stop + timeline scrub, with a live
  `t 0.42 / 1.02 s  |  frame 25 / 61` readout and a scrubber that follows
  playback (~10 Hz; it does not rebuild the list, so the selection is stable)
- **GLB preview (read-only)** — select a `Mesh.Skeletal` entity and the panel
  lists the clips embedded in its GLB instead. Play/Pause/Stop and the scrubber
  work; New/Delete/Save/Add Key/Edit Verts are disabled because the clip lives
  inside the asset. The preview never rewrites the entity's authored
  `anim=` — see `MeshObjectNode::previewClip`
- **Convert to Animated** (Mesh properties / Entity Properties) — writes a
  default clip under `GameData/Environment/Anims/` and switches the entity to an
  animated mesh
- **Edit Verts** — vertex picking (click / Shift-add / Sel All); keyboard
  deform moves verts with **U/J/H/K/Y/I** (X/Z/Y) and rotates with **O/L**;
  **Ctrl+Z/Y** undo/redo is tool-scoped
- **Add Key / Del Key** — capture or drop sparse vertex offsets at the current
  timeline position; playback is client-cosmetic (not networked)

## Keyboard Shortcuts

Everything with a `Ctrl` modifier is in the remappable table (**Help → Keyboard
Shortcuts**, persisted in `System/AngelEd.ini` `[Keys]`); the rest is fixed.

| Key | Action |
|---|---|
| Ctrl+Z / Ctrl+Y | Undo / Redo (Ctrl+Shift+Z also redoes) |
| Ctrl+C / Ctrl+V | Copy / paste the whole selection (new ids, pasted set selected) |
| Ctrl+D | Duplicate the whole selection |
| Delete | Delete the whole selection |
| Escape | Cancel an active drag (restores the pre-drag transform); otherwise clear the selection |
| Ctrl+S / Ctrl+Shift+S / Ctrl+O / Ctrl+N / Ctrl+P / Ctrl+Q | Save / Save As / Open / New / Play Test / Exit |
| U/J | Nudge -X / +X |
| H/K | Nudge +Z / -Z |
| Y/I | Nudge -Y / +Y |
| O/L | Rotate +90 deg / -90 deg |
| T (hold) / B (hold) | Scale up / down |
| Enter | Commit placement |
| Middle Mouse | Pan camera |
| Shift+Middle Mouse | Pan vertically |
| Alt+Middle Mouse | Orbit camera |
| Scroll Wheel | Dolly camera |
| PageUp / PageDown | Raise / lower the camera |
| Home | Reset the camera |
| Numpad 7 / 1 / 3 / 9 / 2 / 5 | Top / Bottom / Right / Left / Front / Perspective |
| F3 | World Hierarchy |
| F5 / F6 / F7 / F8 / F9 / F10 | Model / Sound / Texture / Pawn / Script Manager / Pickups |
| F11 | Toggle fullscreen |
| H | Heightmap editor |
| G | Cycle the CSG operation (Model placement mode) |

**Shortcuts follow the keyboard layout, not the physical key.** A binding is
stored as a *letter* and resolved to whichever key produces that letter on your
layout, so Ctrl+Z is Ctrl+Z on a German QWERTZ keyboard (where the Z key sits at
the US-Y position) and on AZERTY. Before this, Ctrl+Z was unreachable on any
non-US layout — and on QWERTZ it ran the "nudge Y down" key instead, which is why
Undo appeared to move the selection.

With **Unreal-Style Brush Movement** on, `1`/`2`/`3` become the movement-speed
keys and the placement modes move to `Alt`+number.

## Viewports (split camera layouts)

The **View:1 / View:2V / View:2H / View:4** chip cycles the layout, and
**Pop:On** pops the active pane into its own window.

- Every pane has its **own camera and its own Top/Side/Down/Persp state**; the
  camera presets, menu items and Numpad keys always act on the **active** pane.
- The active pane is whichever one you last clicked; the others are read-only
  reference views and keep their own camera as you navigate the active one.
- A fresh session seeds the UnrealEd kit: pane 0 is an editable perspective view,
  1 Top, 2 Front, 3 Side.
- Popped-out panes navigate with the same gestures (MMB pan, Shift+MMB vertical,
  Alt+MMB orbit, wheel dolly). Picking inside a pop-out is not wired yet — click
  the main-window pane to select.

## Hot reload (defs, no Refresh click)

The editor watches `GameData/` while it runs and reloads definition files the
moment they are saved, so a `.ozls` or a `PawnDefs/*.cfg` can be edited in
VS Code / Notepad and the change is live without pressing **Reload** in the
Script Manager or **Refresh** in the Actor Hierarchy.

- **Watched:** every `*.ozls` under `GameData/` and every `*.cfg` under
  `GameData/Pawn/PawnDefs/`. Saves are debounced (~180 ms of quiet), so an
  editor's temp-file + rename sequence is read once, after it settles.
- **What refreshes:** the Script Manager list (same row stays selected), the
  Actor Hierarchy and Pickup lists (only when a def name is added or removed,
  and expansion/selection are preserved), the Entity Properties def section when
  it is showing the file that changed, and the live def registry itself.
- **What does NOT happen:** the world is not reloaded, the WorldGraph is not
  rebuilt, no History entry is pushed, and the camera/selection/undo stack are
  untouched. A def is not a world entity, so editing one cannot change what the
  level contains.
- **A broken file keeps the last good def.** A half-typed entity does not make
  the enemy/weapon vanish; the log says it no longer parses. Deleting the file
  (or emptying it) *does* remove the def.
- **Live instances update:** a `.cfg` edit pushes speed/ranges/damage onto
  already-spawned pawns, and a changed model/anim path drops their cached mesh so
  the next frame re-resolves it. Health is instance state and is not reset.
- **Not covered:** content inside `.oz*` packages (repack to change it), edits to
  the `.ozone` world itself (open/save the document), and asset *file* contents
  (a re-exported `.glb`/`.png` keeps its cached GPU resource - use Reload Mesh /
  the Texture Manager's reload for that).

Implementation: `AngelEd/Source/Subsystems/HotReload.cpp` (watcher + `.cfg`
loader), `LightningEntityRegistry::ReloadFile` (in-place `.ozls` reload) and
`PawnSystem::UpdateDef`/`RemoveDef`.

## UnrealEd-style brush editing

The main window is resizable (minimum 1024x640) and remembers its position/size
in `System/AngelEd.ini` `[Layout]` (`main_x/y/w/h`, `main_max`). The toolbar is
two rows so no control is clipped at the minimum width.

**Settings → Unreal-Style Brush Movement** (also the `Unreal:Off/On` toolbar
button) switches the mouse from gizmo handles to UnrealEd's modifier gestures.
It is **off by default** because it takes Ctrl/Shift away from the gizmo's own
snapping modifiers.

| Gesture | Result |
|---|---|
| Ctrl + LMB drag (horizontal) | Move the selection along world **X** |
| Ctrl + RMB drag (horizontal) | Move along world **Z** |
| Ctrl + LMB + RMB drag (vertical) | Move along world **Y** (up/down) |
| Ctrl + RMB drag while the **Rotate** tool is active | Yaw |
| Ctrl + Shift + LMB drag | Move on Ctrl's axis **and take the camera with it** |
| Arrow keys | Nudge on the ground plane (X/Z) by the Snap step (or 1 unit) |
| Ctrl + Up/Down | Nudge vertically (Y) |
| 1 / 2 / 3 | Movement speed Slow / Medium / Fast |
| Alt + 1..5 | Placement modes (Model / Pickup / Node / Zone / Terrain) |

Notes:

- **Axes are translated to this engine**, which is Y-up. Unreal's X/Y/Z map to
  X/Z/Y here, matching what the viewport shows; the table names the *world* axis
  you actually move on.
- **Exactly one axis is live at a time** (UnrealEd's `CalcAxialMoveRot`): LMB
  moves X, RMB moves Z, both move Y. Pressing the second button mid-drag switches
  to the vertical axis without the object jumping, because the total is the sum
  of per-frame deltas.
- **Snapping is anchored to the drag's start**, not to the world grid, so an
  entity sitting at 0.37 does not teleport to 0.5 on the first pixel. This is
  UnrealEd's `GGridBase` behaviour.
- The pixel-to-world factor follows the selection's **depth on screen**, so the
  drag covers the same screen distance at any zoom; the speed keys scale it by
  0.25x / 1x / 4x (UnrealEd's MovementSpeed ratio).
- **Pitch/roll are not mapped.** OZONE stores one rotation (yaw) per
  renderable/mesh; a drag that cannot be written back would not do what it says.
- Ctrl/Shift gestures are ignored while an Animation vertex edit is active, and
  Shift+RMB still does the per-face Surface Properties pick.
- A gesture pushes **one** History entry, on the first movement - a Ctrl+click
  that never moves leaves no undo step. **Escape** during a gesture cancels it and
  restores the pre-drag transform **and the camera**.
- **Ctrl+click does not select while this mode is on** (it is the transform
  modifier); Shift extends the selection instead. With the mode off, Ctrl extends
  as well, matching UnrealEd.
- The movement-speed keys follow the UnrealEd convention and are hard-wired;
  the placement-mode keys stay remappable and move to Alt while the option is on.
  With the option off, 1/2/3 keep their original placement-mode meaning.

## World Saving

Worlds are saved in OZONE text format. OZONE export includes CSG brush primitives, heightmap, and all entity types (player starts, pickups, NPCs, zones, emitters) with per-zone environment overrides.

**`portal` and `trigger` lines round-trip.** Portals are exported from
`ZoneManager` (target world, bounds, spawn, `bidir`); trigger volumes are
re-emitted from the parsed entity set — the editor has no trigger-placement UI,
so author `trigger <def> minX..maxZ [once] [team=N]` lines as text, and they
survive open/save and History snapshots. Both kinds need a full **6-number box**
(min/max in OZONE Z-up): the parser warns about shorter lines and the runtime
drops them. A trigger box that contains `playerstart` #1 fires its `on_enter`
on the first simulation step, so `once` volumes should be placed off the spawn.

## History (Undo / Redo)

Full-document undo/redo via **Edit > Undo/Redo**, **Ctrl+Z** / **Ctrl+Y** (Ctrl+Shift+Z also redoes). Each step snapshots the whole world (geometry, entities, level metadata, heightmap) using the OZONE export, so place/delete/duplicate, property edits, lighting/portal/zone changes, texture application, heightmap generation, terrain painting, spawns, snaps and gizmo drags are all reversible. History is cleared when a world is opened or a new one is created. The Animation tool's vertex-edit undo (Ctrl+Z/Y while editing verts) is separate and takes precedence there.

Two properties worth knowing:

- **One edit is one step**, however many entities it touched. Deleting a 20-brush
  multi-selection, pasting a set, or snapping a group is a single Ctrl+Z.
- Shortcuts follow the **keyboard layout**, so Ctrl+Z is Ctrl+Z on QWERTZ/AZERTY
  as well. Before that fix it was unreachable on any non-US layout (see the
  Keyboard Shortcuts note).

## Known Limitations

- Lighting toggle (Lit/Unlit) does not actually unset shader from model materials
- Render meshes are not CSG-carved (CSG booleans process collision volumes only)
- Undo/redo covers world state only; camera and selection are not restored
- No test-play save prompts ("Reload world from playtest changes?")
- Model/Texture preview rendering requires the raylib viewport to be focused
- Lighting effects (watery, torch, fire, lamp) are UI only — not rendered in viewport
- Picking inside a **popped-out** pane is not wired yet: click the main-window pane to select
- The Properties panel does not re-flow its rows when the window is resized; it scrolls instead
