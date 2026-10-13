# Angels95 — OmegaTech Engine

![Angels95 Title Splash](../GameData/Tex/Title/splash.png)

Angels95 reimagines the OmegaTech Engine as a **multiplayer game world** — a persistent, server-authoritative realm where players explore partitioned worlds, collect power-ups, level up, and fight NPCs alongside other connected players.

Built on [raylib](https://www.raylib.com/) 5.5 with PS1-inspired retro aesthetics and a custom OZONE world format. Current release: **b82**.

The player is an **AngelPlayer** built from behaviour modules (GameUi bridge, InventoryBehaviour, WeaponBehaviour, PlayerController); the **SlotBar** object-bar HUD renders a data-driven atlas with click-to-select. **ZoneManager** owns zone volumes, portals and environment overrides; **SoundManager** is the single facade for every sound/music call; the menu's Internet browser talks to the official HTTPS master; and `--shot` captures deterministic in-engine screenshots.

## Showcase

In-engine `--shot` renders across the four shipped worlds:

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
