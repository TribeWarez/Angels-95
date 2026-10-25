# Angels95 — AngelTech Engine

![Angels95 Title Splash](../GameData/Tex/Title/splash.png)

Angels95 is a **multiplayer game world** built on **AngelTech**, the engine that began life as the OmegaTech Engine — a persistent, server-authoritative realm where players explore partitioned worlds, collect power-ups, level up, and fight NPCs alongside other connected players.

Built on [raylib](https://www.raylib.com/) 5.5 with PS1-inspired retro aesthetics and a custom OZONE world format. Current release: **b111**.

The player is an **AngelPlayer** built from behaviour modules (GameUi bridge, InventoryBehaviour, WeaponBehaviour, PlayerController); the **SlotBar** object-bar HUD renders a data-driven atlas with click-to-select. **ZoneManager** owns zone volumes, portals and environment overrides; **SoundManager** is the single facade for every sound/music call; the menu's Internet browser talks to the official HTTPS master; and `--shot` captures deterministic in-engine screenshots.

## Showcase

In-engine `--shot` renders: the six showcase worlds, then the campaign arc
(eleven of its worlds were rebuilt in b111 - two frames each, at the spawn
and at the landmark the level is built around).

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

<img src="../GameData/Screenshots/Sektor_02_MegacityGrid_1.png" width="22%" alt="Campaign - Megacity Grid 1" />
<img src="../GameData/Screenshots/Sektor_02_MegacityGrid_2.png" width="22%" alt="Campaign - Megacity Grid 2" />
<img src="../GameData/Screenshots/Sektor_03_AegisBaseAlpha_1.png" width="22%" alt="Campaign - Aegis Base Alpha 1" />
<img src="../GameData/Screenshots/Sektor_03_AegisBaseAlpha_2.png" width="22%" alt="Campaign - Aegis Base Alpha 2" />
<img src="../GameData/Screenshots/AlShaddai_Metropolis_1.png" width="22%" alt="Campaign - AlShaddai 1" />
<img src="../GameData/Screenshots/AlShaddai_Metropolis_2.png" width="22%" alt="Campaign - AlShaddai 2" />
<img src="../GameData/Screenshots/Chasm_ForgottenMemories_1.png" width="22%" alt="Campaign - Chasm 1" />
<img src="../GameData/Screenshots/Chasm_ForgottenMemories_2.png" width="22%" alt="Campaign - Chasm 2" />
<img src="../GameData/Screenshots/Refinery_Complex_02B_1.png" width="22%" alt="Campaign - Refinery 1" />
<img src="../GameData/Screenshots/Refinery_Complex_02B_2.png" width="22%" alt="Campaign - Refinery 2" />
<img src="../GameData/Screenshots/Digital_Purgatory_1.png" width="22%" alt="Campaign - Digital Purgatory 1" />
<img src="../GameData/Screenshots/Digital_Purgatory_2.png" width="22%" alt="Campaign - Digital Purgatory 2" />
<img src="../GameData/Screenshots/Phantom_Hubs_1.png" width="22%" alt="Campaign - Phantom Hubs 1" />
<img src="../GameData/Screenshots/Phantom_Hubs_2.png" width="22%" alt="Campaign - Phantom Hubs 2" />
<img src="../GameData/Screenshots/SoulEater_Vaults_1.png" width="22%" alt="Campaign - Soul Vaults 1" />
<img src="../GameData/Screenshots/SoulEater_Vaults_2.png" width="22%" alt="Campaign - Soul Vaults 2" />
<img src="../GameData/Screenshots/Citadel_AstralMind_1.png" width="22%" alt="Campaign - Astral Mind 1" />
<img src="../GameData/Screenshots/Citadel_AstralMind_2.png" width="22%" alt="Campaign - Astral Mind 2" />
<img src="../GameData/Screenshots/Esoteric_Altar_Chambers_1.png" width="22%" alt="Campaign - Altar Chambers 1" />
<img src="../GameData/Screenshots/Esoteric_Altar_Chambers_2.png" width="22%" alt="Campaign - Altar Chambers 2" />
<img src="../GameData/Screenshots/NearDeath_Threshold_1.png" width="22%" alt="Campaign - Threshold 1" />
<img src="../GameData/Screenshots/NearDeath_Threshold_2.png" width="22%" alt="Campaign - Threshold 2" />

## Contents

| Page | Description |
|---|---|
| [Core Game](Core-Game) | Gameplay mechanics, controls, HUD, inventory, save system |
| [Engine Overview](Engine-Overview) | Architecture, source tree, key classes, package system |
| [Engine & Game Limits](Constraints) | Hard limits: movement, collision, lights, network, authoring |
| [Editor Usage](Editor-Usage) | AngelEd panels, toolbar, CSG, workflow, known gaps |
| [LightningScript](LightningScript) | Scripting language reference, opcodes, entity definitions |
| [World Format (OZONE)](World-Format-OZONE) | OZONE syntax and world directory layout |
| [World Design Pass](World-Design-Pass) | Campaign-map design standard, per-level intent and the authoring traps |
| [Building](Building) | Build instructions, prerequisites, test targets, CI |
| [Engine Roadmap](Engine-Roadmap) | Gap analysis and prioritized roadmap by tier |
| [Master Server](Master-Server) | Master server protocol, HTTPS uplink, client browser |
| [Aethelgard Ruins changes](Aethelgard-Ruins-Changes) | What the mission-world pass changed, line by line |
| [Outdoor maps](Outdoor-Maps-Changes) | SilentPines / Dusk Falls: terrain, water, sky seams, and the UV traps behind them |

## Related repositories

- [**Angels-95**](https://github.com/TribeWarez/Angels-95) — this repository: the game client, the
  AngelTech engine, the AngelEd editor, the dedicated server and the master server.
- [**angels95-master-server**](https://github.com/TribeWarez/angels95-master-server) — the hosted master
  server (heartbeat intake and the public server list). The in-repo `AngelMaster` implements the same
  protocol; this is the deployed service.
- [**OmegaTech**](https://github.com/paganswrath/OmegaTech) and
  [**OTEditor**](https://github.com/paganswrath/OTEditor) — the original engine and editor that Angels95
  was built on top of. If you want to know where this codebase came from, start there.
