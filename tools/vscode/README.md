# Angels95 editor extensions

Two **independent** VS Code extensions for the Angels95 / OmegaTech data formats:

| Folder | Language | Purpose |
|---|---|---|
| `angels95-ozone` | `ozone` (`.ozone`) | OZONE world format: TextMate grammar, Outline, argument/flag hover, authoring diagnostics |
| `angels95-ozls` | `ozls` (`.ozls`) and `ozpawn` (`.cfg`) | LightningScript: grammar, Outline/Symbols tree, structure TreeView, diagnostics, workspace def index; pawn-def `.cfg` grammar and symbols |

They do not depend on each other or on npm — plain JavaScript extension hosts, no
build step. Both are also loadable as `.vsix` packages (see *Packaging*).

---

## Install

### Option A — junction (recommended for this repo)

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/vscode/install.ps1
```

This junction-links `angels95-ozone` and `angels95-ozls` into
`%USERPROFILE%\.vscode\extensions`. Edits to the extension sources take effect
after **Developer: Reload Window**. No copy is made, so the repo stays the only
source of truth.

```powershell
# one extension only
powershell -File tools/vscode/install.ps1 -Only ozone
# remove both junctions (the repo is never touched)
powershell -File tools/vscode/uninstall.ps1
```

### Option B — build and install a `.vsix`

```powershell
# writes tools/vscode/dist/*.vsix
powershell -NoProfile -ExecutionPolicy Bypass -File tools/vscode/package-vsix.ps1

# build + install through the VS Code CLI
powershell -File tools/vscode/package-vsix.ps1 -Install

# non-destructive test into a throwaway extensions dir
powershell -File tools/vscode/package-vsix.ps1 -Install -ExtensionsDir C:\Temp\vscode-exts
```

Then **Developer: Reload Window**.

Extension IDs: `tribewarez.angels95-ozone` and `tribewarez.angels95-ozls`
(`code --uninstall-extension <id>` removes a `.vsix` install).

### Workspace settings

`.vscode/` is gitignored, so this is a per-checkout change. This checkout's
`.vscode/settings.json` now maps:

```jsonc
"*.ozone": "ozone",
"*.ozls": "ozls",
"**/Pawn/PawnDefs/*.cfg": "ozpawn",
"**/Pawn/Player/characters.cfg": "ozpawn"
```

The old `*.ozone` / `*.ozls` → `plaintext` pins had to be removed or no
extension would ever activate. `.cfg` is left unassociated globally, so other
`.cfg` files do not silently become pawn defs.

A previous minimal extension lives in `.vscode/angels-ozls/`; it is superseded by
`tools/vscode/angels95-ozls` and is intentionally left untouched.

---

## `angels95-ozone`

### Syntax highlighting

Scope `source.ozone`. The grammar follows
`Source/World/OzoneParser.cpp` rather than the prose docs, so it highlights:

- `add` / `sub` / `intersect` / `deresc` and the five primitive brushes
  (`box`, `cyl`, `sph`, `pyr`, `pln`)
- `skybox`, `heightmap`
- every entity spelling, exactly and case-sensitively:
  `playerstart`, `pickup`, `zone`, `npc`, `light`, `portal`, `levelinfo`,
  `particles`, `emitter`, `door`, `trigger`, `Mesh.Static`, `Mesh.Skeletal`,
  `ParticleEmitter`, `PathNode`, `WindZone`
- `zone` subtypes and `light` subtypes
- brush-wide and `face<name>_<field>=` kwargs, zone physics, light, mesh,
  emitter, PathNode and door kwargs
- `flags=`, path strings, booleans (`bidir`, `once`, `loop`, `wind`)
- numbers and `gametype=` keys

Three deliberate, faithful choices that look like bugs in a generic grammar:

1. **`#` is a comment only in column 0.** `//` is not a comment at all. An
   indented `#` or any `//` line is scoped `invalid.illegal` because the engine
   parses it as an unknown primitive and AngelEd deletes it on the next save.
2. **`surfAlpha=`, `surfCutoff=` and `surfGlowScale=` are marked deprecated.**
   `OzoneParser::ParseSurfaceField` does not read those spellings (the exporter
   writes them anyway). Use `alpha=`, `cutoff=`, `glowScale=`.
3. **Unknown brush kwargs are not marked.** The parser ignores them by design;
   that is what makes the surface vocabulary extensible.

Ctrl+/ is intentionally not bound for `ozone`: a `#` comment only counts in
column 0 and there is no block comment, so an editor-generated indented `#`
would be invalid.

Scopes for theme authors: `source.ozone`, with
`support.function.primitive.ozone`, `keyword.other.entity.ozone`,
`support.class.zone-type.ozone` / `support.class.light-type.ozone`,
`entity.other.attribute-name.<group>.ozone`, `constant.numeric.ozone`,
`invalid.deprecated.surface-kwarg.ozone` and `invalid.illegal.*.ozone`.

### Outline / Symbols

`GameData/Worlds/*/World.ozone` opens an Outline listing **every brush and entity
as its own item** (e.g. `add box (-68, -60, 4.75)`, `zone sky — zone_sky_0`,
`portal → TargetWorld`, `PathNode r1_a`). Nothing is hidden behind a collapsed
group.

If you prefer the collapsible kind groups (`Brushes: box 96`, `light (11)`, …),
enable:

```json
"angels95Ozone.outline.grouped": true
```

### Hover

- Hover a primitive/entity keyword: full positional signature + the engine
  convention that is easy to get wrong (`cyl`'s authored `z` is the TOP, `box`
  is centre-based, `spread` is degrees, `WindZone` strength is units, …).
- Hover a positional number: which argument it is and what it means.
- Hover `flags=NNN`: decodes every set bit by name
  (`16 → SURF_COLLISION_PROXY`, `8 → SURF_FAKEBACKDROP`, …), plus unknown bits.
- Hover a kwarg: what the engine does with it, including "inert unless" notes
  (`cutoff=0` makes `SURF_MASKED` a no-op, etc.).
- Hover `surfAlpha=`, `surfCutoff=` or `surfGlowScale=`: says it is **not read by
  the parser** and names the working spelling (`alpha=`, `cutoff=`, `glowScale=`).

### Diagnostics

Mirrors the failure modes that are silent at runtime:

| Check | Why |
|---|---|
| indented `#` / `//` line | not a comment; the line is deleted on save |
| unknown first token | `OzoneParser` warns and skips |
| `portal` with < 6 box numbers | loader drops it, no runtime effect |
| `bidir` without the spawn triple | read from arg 9, so it does nothing |
| `trigger` with < 6 bounds | parsed but never reaches the runtime |
| `light point` < 8 numbers | loader drops the line |
| `light point` with only 7 numbers | radius becomes 0 → invisible light |
| `flags=` not decimal | `std::stoi`; `0x8` becomes 0 |
| `pickup … respawn=` | respawn is a bare positional number |
| short `npc`/`zone`/`WindZone`/`PathNode`/`ParticleEmitter`/`door` lines | dropped by the loader |

`GameData/Worlds/TestMap/World.ozone` intentionally produces **zero** diagnostics.
The three dead surface kwarg spellings (`surfAlpha=`, `surfCutoff=`,
`surfGlowScale=`) are highlighted by the grammar and explained on hover, but are
**not** Problems entries: they occur 100+ times in shipped worlds.

---

## `angels95-ozls`

### Syntax highlighting

Scope `source.ozls`. Highlights:

- the real header form `entity "Name" : type {` (the name is a quoted string
  **before** the colon)
- every accepted entity type (including `Mesh.Static`, `Mesh.Skeletal`,
  `ParticleEmitter`, `WindZone`, `GameUI`, `skill`, `spell`, `light` and the
  global game hierarchy types); an unknown type is scoped invalid
- the three body blocks `stats { }`, `actions { }`, `variants { }`
- every top-level key the parser actually knows
- stat keys, vec3 tuples `(r, g, b)`, strings and numbers
- action hooks and every script opcode
- `$vars`, `$flagN`, `$result`, labels, operators and both comment forms
  (`//` and `#`)

### Outline / Symbols tree

`Ctrl+Shift+O` on a `.ozls`:

```
"pistol_01" : weapon
├─ Properties        mesh, texture, icon, mesh_type, anim_idle
├─ stats             damage, fire_rate, magazine, viewmodel_*, fire_sound ...
├─ actions           on_fire, on_reload (each with its script lines)
├─ variants          lvl1, lvl2 → mesh_override / texture_override
└─ Problems
```

For a pawn `.cfg`, sections (e.g. `[arcanist]`) and their keys are listed; a flat
pawn def shows `(root)`.

### Hover and CodeLens

- Hovering a **stat key** shows what consumes it, plus the current value and its
  parsed kind (`float` / `string` / `vec3`).
- Hovering a **top-level key**, **hook**, **opcode**, **variable**, **entity
  type** or **variant override** explains the runtime rule (including the
  scripts' 30/50-step budgets, `$result`, `$flag0..63`, and the event-bus name
  mapping).
- CodeLens above `stats {` / `actions {` / `variants {` shows their counts, and
  above each hook shows its line count. A pawn `.cfg` gets `pawn: <name>`.

### Structure TreeView

Explorer → **LightningScript** (visible while an `ozls`/`ozpawn` file is active):

- the active document's structure, with every item clickable to its line
- a **Problems** node fed by the same diagnostics
- **Workspace Defs** — every `GameData/**/*.ozls` grouped by entity type, with a
  one-click open. Indexed on demand and invalidated by file create/delete/save.
- **Pawn Defs** — every `GameData/Pawn/PawnDefs/*.cfg`, named by its `name=` key,
  with a one-click open.

### Diagnostics

| Check | Why |
|---|---|
| unknown entity type | the def is marked `UNKNOWN` and **dropped** |
| missing/malformed `entity "Name" : type {` | the whole file yields an empty def |
| `vec3 name = (…)` prefix | the parser sees key `vec3`, expects `=`, and aborts the rest of the stats block |
| stat key without `=` | aborts the rest of the block |
| malformed vec3 | that one stat is dropped (deliberately not zeroed) |
| stat value containing a space | the parser stores only the first token; quotes are stored verbatim |
| unknown top-level key | warned and dropped — `model_height = 3.0` outside `stats { }` is the historical case |
| action name containing `.` or `-` | cannot become a script label, so it can never fire |
| unknown opcode | `LightningScriptContext` warns at runtime |
| second `entity` block | only the first is parsed |
| unclosed block | file ends before every `}` |

### Pawn `.cfg`

Language `ozpawn`, scope `source.ozpawn`. Supports both forms the engine loads:

- `GameData/Pawn/PawnDefs/*.cfg` — flat `key=value` with `#`/`;` line comments
- `GameData/Pawn/Player/characters.cfg` — `[Section]` roster entries

Diagnostics encode the sharpest edge: the **client and editor use `std::stof`
without a try/catch**, so a non-numeric `speed=`/`damage=`/`maxHealth=` value
terminates the process (the server logs and keeps the default). A fractional
`maxHealth`/`accent` is also rejected because those are `std::stoi`.

> The `.cfg` language is contributed through `filenamePatterns`
> (`**/PawnDefs/*.cfg` and `**/Pawn/Player/characters.cfg`), not the bare `.cfg`
> extension, so a project that uses `.cfg` for something else is unaffected.
> This repo additionally pins both paths in `.vscode/settings.json`.

---

## Snippets

- `ozone`: `box`, `cyl`, `sph`, `pln`, `sub`, `playerstart`, `pickup`, `npc`,
  `light point|spot|directional`, `zone sky`, `portal`, `trigger`, `Mesh.Static`,
  `Mesh.Skeletal`, `ParticleEmitter`, `PathNode`, `WindZone`, `levelinfo`,
  `particles`, `emitter`, `skybox`, `heightmap`, `surface`
- `ozls`: `entity`, `ozls-weapon`, `ozls-skyzone`, `ozls-pawn`, `ozls-pickup`,
  `ozls-spell`, `ozls-light`, `ozls-mission`, `ozls-objective`, `hook`, `stats`,
  `if`, `emit`, `obj`, `area-damage`, `spawn-pawn`
- `ozpawn`: `pawn-def`, `hover`, `character`

---

## Testing

```powershell
node tools/vscode/test/smoke.js
```

The smoke test stubs the `vscode` module and:

- parses `GameData/Worlds/TestMap/World.ozone` (expects 230 boxes, zero
  diagnostics) and checks outline + `flags=16` hover
- exercises every OZONE diagnostic against a deliberately broken file
- parses `Pistol_01.ozls`, checks stats/hooks/symbols, and verifies the stat/hook
  hover text and the `N stats` / `N actions` / `N lines` CodeLens titles
- parses `Walker.cfg` and checks the non-numeric / fractional-int errors
- compiles every TextMate regex in all three grammars
- walks the TreeView provider (active document + workspace index)

`package-vsix.ps1` was verified by installing both `.vsix` files into a throwaway
`--extensions-dir`:

```
$ code --extensions-dir C:\Temp\vscode-exts --install-extension tools/vscode/dist/angels95-ozls-1.0.0.vsix
Extension 'angels95-ozls-1.0.0.vsix' was successfully installed.
```

---

## Layout

```
tools/vscode/
├── install.ps1                  junction install
├── uninstall.ps1
├── package-vsix.ps1             zip-based .vsix (no npm/vsce)
├── test/smoke.js                headless verification
├── angels95-ozone/
│   ├── package.json
│   ├── language-configuration.json
│   ├── syntaxes/ozone.tmLanguage.json
│   ├── snippets/ozone.json
│   └── src/{extension,parser,data}.js
└── angels95-ozls/
    ├── package.json
    ├── language-configuration.json
    ├── pawn-language-configuration.json
    ├── syntaxes/{ozls,ozpawn}.tmLanguage.json
    ├── snippets/{ozls,ozpawn}.json
    └── src/{extension,parser,data,cache,symbols,diagnostics,tree,assist}.js
```

---

## Extending

| Change | Where |
|---|---|
| new OZONE keyword / entity / argument | `angels95-ozone/syntaxes/ozone.tmLanguage.json` + `src/data.js` |
| new surface flag | `angels95-ozone/src/data.js` (`FLAG_BITS`), mirroring `Source/World/SurfaceFlags.hpp` |
| new LightningScript entity type | `angels95-ozls/src/data.js` (`ENTITY_TYPES`) + the grammar's type alternation |
| new opcode / hook | `angels95-ozls/src/data.js` (`OPCODES` / `KNOWN_HOOKS`) + the grammar |
| new `.ozls` top-level key | `angels95-ozls/src/data.js` (`TOP_KEYS`) + the grammar |
| new pawn `.cfg` key | `angels95-ozls/src/data.js` (`CFG_KEYS`, plus `NUMERIC_CFG_KEYS` / `INT_CFG_KEYS`) |

Keep the grammar and `data.js` in step: the grammar colours what the parser
accepts, and the diagnostics explain why a construct is ignored.

The authoritative sources are `Source/World/OzoneParser.cpp`,
`Source/World/OzOzoneLoader.cpp`, `Source/World/SurfaceFlags.hpp`,
`Source/Script/LightningScriptParser.cpp`,
`Source/Script/LightningScriptContext.cpp` and `Source/Core.hpp`'s `.cfg` loader.
