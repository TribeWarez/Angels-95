# Angels95 — OmegaTech Engine

![Angels95 Title Splash](../GameData/Tex/Title/splash.png)

Angels95 reimagines the OmegaTech Engine as a **multiplayer game world** — a persistent, server-authoritative realm where players explore partitioned worlds, collect power-ups, level up, and fight NPCs alongside other connected players.

Built on [raylib](https://www.raylib.com/) 5.5 with PS1-inspired retro aesthetics and a custom OZONE world format. Current release: **b101**.

The player is an **AngelPlayer** built from behaviour modules (GameUi bridge, InventoryBehaviour, WeaponBehaviour, PlayerController); the **SlotBar** object-bar HUD renders a data-driven atlas with click-to-select. **ZoneManager** owns zone volumes, portals and environment overrides; **SoundManager** is the single facade for every sound/music call; the menu's Internet browser talks to the official HTTPS master; and `--shot` captures deterministic in-engine screenshots.

## Showcase

In-engine `--shot` renders across the showcase worlds (the campaign maps added in
b101 are at the end):

<img src="../GameData/Screenshots/CitadelRuins_1.png" width="22%" alt="Citadel Ruins 1" />
<img src="../GameData/Screenshots/CitadelRuins_2.png" width="22%" alt="Citadel Ruins 2" />
<img src="../GameData/Screenshots/CitadelRuins_3.png" width="22%" alt="Citadel Ruins 3" />
<img src="../GameData/Screenshots/Dessert_Dreams_1.png" width="22%" alt="Dessert Dreams 1" />
<img src="../GameData/Screenshots/Dessert_Dreams_2.png" width="22%" alt="Dessert Dreams 2" />
<img src="../GameData/Screenshots/Dessert_Dreams_3.png" width="22%" alt="Dessert Dreams 3" />
<img src="../GameData/Screenshots/Dust_Ravine_1.png" width="22%" alt="Dust Ravine 1" />
<img src="../GameData/Screenshots/Dust_Ravine_2.png" width="22%" alt="Dust Ravine 2" />
<img src="../GameData/Screenshots/Dust_Ravine_3.png" width="22%" alt="Dust Ravine 3" />
<img src="../GameData/Screenshots/EngineTest_1.png" width="22%" alt="Engine Test 1" />
<img src="../GameData/Screenshots/EngineTest_2.png" width="22%" alt="Engine Test 2" />
<img src="../GameData/Screenshots/EngineTest_3.png" width="22%" alt="Engine Test 3" />
<img src="../GameData/Screenshots/Sektor_01_SilentPines_1.png" width="22%" alt="SilentPines — arrival ledge over the gorge" />
<img src="../GameData/Screenshots/Sektor_01_SilentPines_2.png" width="22%" alt="SilentPines — waterfall and plunge pool" />
<img src="../GameData/Screenshots/Sektor_01_SilentPines_3.png" width="22%" alt="SilentPines — shrine terrace and pines" />
<img src="../GameData/Screenshots/KryoArchipel_Severny_1.png" width="22%" alt="Dusk Falls — upper terrace" />
<img src="../GameData/Screenshots/KryoArchipel_Severny_2.png" width="22%" alt="Dusk Falls — cascade and aqueduct" />
<img src="../GameData/Screenshots/KryoArchipel_Severny_3.png" width="22%" alt="Dusk Falls — basin and stepping stones" />

## Contents

| Page | Description |
|---|---|
| [Core Game](Core-Game) | Gameplay mechanics, controls, HUD, inventory, save system |
| [Engine Overview](Engine-Overview) | Architecture, source tree, key classes, package system |
| [Editor Usage](Editor-Usage) | AngelEd panels, toolbar, CSG, workflow, known gaps |
| [LightningScript](LightningScript) | Scripting language reference, opcodes, entity definitions |
| [World Format (OZONE)](World-Format-OZONE) | OZONE syntax and world directory layout |
| [Building](Building) | Build instructions, prerequisites, test targets, CI |
| [Engine Roadmap](Engine-Roadmap) | Gap analysis and prioritized roadmap by tier |
| [Master Server](Master-Server) | Master server protocol, HTTPS uplink, client browser |
| [Aethelgard Ruins changes](Aethelgard-Ruins-Changes) | What the mission-world pass changed, line by line |
| [Outdoor maps](Outdoor-Maps-Changes) | SilentPines / Dusk Falls: terrain, water, sky seams, and the UV traps behind them |

## Related repositories

- [**angels95-master-server**](https://github.com/TribeWarez/angels95-master-server) — the hosted master
  server (heartbeat intake and the public server list). The in-repo `AngelMaster` implements the same
  protocol; this is the deployed service.
- [**OmegaTech**](https://github.com/paganswrath/OmegaTech) and
  [**OTEditor**](https://github.com/paganswrath/OTEditor) — the original engine and editor that Angels95
  was built on top of. If you want to know where this codebase came from, start there.
