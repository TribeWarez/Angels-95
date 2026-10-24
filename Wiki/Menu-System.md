# Menu & Title System

The title screen and pause overlay share one UI shell. There are no floating
panes: exactly one page is active, so a page behind another cannot receive a
click through it.

## Files

| File | Role |
|---|---|
| `Source/Menu/TitleMenu.hpp` | Shell, global input, page dispatch, action results |
| `Source/Menu/MenuMetrics.hpp` | Responsive geometry (screen/DPI/UI scale); raylib-free |
| `Source/Menu/MenuStyle.hpp` | Palette and shared widgets (title + in-game settings) |
| `Source/Menu/CharacterRoster.hpp` | `characters.cfg` parser; raylib-free |
| `Source/Menu/CharacterPreview.hpp` | Real model portraits + live 3D preview |
| `Source/Menu/ServerBrowserModel.hpp` | LAN/internet merge, sort/filter/favourites; raylib-free |
| `Source/Menu/InternetBrowser.hpp` | Async master list + parallel /status probes |
| `Source/Menu/SettingsMenu.hpp` | In-game settings panel (scrolling body) |
| `Source/Menu/PauseMenu.hpp` | Pause overlay (shared metrics/style) |

## Layout

`MenuMetrics::Compute(w, h, dpi, uiScale)` returns the frame, title bar, nav bar,
content and footer rectangles plus every text/row size. Pages must use those
values; a hardcoded rect is a bug. `UiScale` is persisted in
`System/Angels95.ini` as `[AngelTech.HUD] ui_scale`. The menu draws directly to
the screen, not into the game's `Target`, so resizing cannot leave it stretched.

Breakpoints: `compact` below 1100px, `wide` from 1150px. The character page uses
two columns whenever the body is at least 700px wide. Long lists scroll with the
wheel; the selected row stays visible.

## Pages and actions

Nav: Home, Play, Multiplayer, Character, Settings. `F1` opens About; `Esc` goes
back one layer. Pages return a `MenuAction`; `PlayHomeScreen` is the only code
that loads a world, loads a save, joins/hosts a server, or quits. The old
`m_loadGame`/`LoadFlag` path (which never set a world) is gone.

- **Home** — Continue (only when a save resolves to a real world), New Campaign,
  Multiplayer, Co-op, Character, Settings, Quit. The wide layout shows the active
  character card with a real portrait.
- **Play** — Campaign (Sefirot sector/mission list) and Practice (all worlds).
  Single click selects; Launch/Load World is an explicit second action.
- **Multiplayer** — Join (LAN/Internet/Favourites, search, sort, hide full/password,
  details panel, favourites/recents), Host (world list, port, server name), and
  Co-op (mission-pinned host / join).
- **Character** — profile slots, real character cards, live 3D preview, name,
  cosmetic colour, save/new/delete.
- **Settings** — Video, Audio, Controls (key rebinding), Gameplay, Window. The
  in-game panel is the same player settings plus developer toggles.

## Character roster

`GameData/Pawn/Player/characters.cfg` is packed by the existing `player.ozpak`
rule. One section per character, with `[Roster] Order=` controlling display order:

```ini
[Roster]
Order=arcanist,killer

[arcanist]
name=Plague Arcanist
mesh=GameData/Pawn/Player/Plague_Arcanist.glb
mesh_type=static
bio=...
accent=6
```

`PlayerProfile.modelPath` stores the section id. A legacy value that is an asset
path is mapped through `FindByMesh()`. Unknown ids resolve to the `Player` def /
default model; the renderer never opens a path received from the network.

`CharacterPreview` renders portraits from the actual `MeshCache` assets at
512x640 and a live preview while the Character page is open. Authored
`portrait=` overrides are supported. Render textures are unloaded in
`TitleMenu`'s destructor, before `CloseWindow()`.

## Multiplayer character visibility

`MessageType::PLAYER_PROFILE = 32` (server -> client) carries a strict character
id and cosmetic colour byte. The server sanitises the id with
`net::SanitizeCharacterId` (`[A-Za-z0-9_-]`, max 31, no path separators), stores
it on `ServerPlayer`, broadcasts the joining player's profile and sends every
existing player's profile to the newcomer. Clients resolve only roster ids;
unknown ids use the default. A new `MessageType` is forward/backward compatible,
so `PROTOCOL_VERSION` stays at 2.

## Server browser

`ServerBrowserModel` merges LAN and internet rows by `ip:port`, fills missing
fields, and applies source (All/LAN/Internet/Favourites), search, full/password
filters, and sort (Players/Ping/Name/Map). Favourites/recents persist in
`System/ServerBrowser.ini` as ordered `|`-separated `ip:port` values.

LAN discovery's `OZRESPONSE` appends an optional
`;http=<port>;map=<name>;pw=<0|1>` tail. Old clients ignore it; old servers omit
it. `AngelMaster`/internet scanning publishes the master list first, then probes
`/status` in a pool of up to 8 threads (700ms timeout) instead of sequentially.

`AngelServ --world <name>` pins the plain-co-op spawn world; the Host tab passes
it through `LaunchHostedServer`. An unknown name refuses startup with a clear
message. `--campaign` ignores `--world` (the mission frontier owns placement).

## Capture/debug

`--menu-shot <home|play|multiplayer|character|settings> [--menu-shot-out path]
[--menu-shot-frames N] [--shot-res WxH]` runs the real menu and writes a PNG,
then exits. It is a developer aid, not a shipping feature.

Pure rules are covered by `tests/Menu.test.cpp`; LAN/profile wire rules by
`tests/Network.test.cpp`, and `--world` placement by `tests/GameState.test.cpp`.
