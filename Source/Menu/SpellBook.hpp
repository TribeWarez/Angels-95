#pragma once
// ============================================================================
// Menu/SpellBook.hpp
//
// The spell bar and its modal editor.
//
// Two pieces, both header-only (no Makefile / ci.yml entry - see AGENTS.md):
//   DrawSpellBar()  - the persistent HUD row, drawn every frame
//   DrawSpellbook() - the modal opened by the Spellbook action (Q by default): bind spells to the bar
//
// Header-only and raygui-free, matching Menu/MenuStyle.hpp, which supplies every
// primitive used here.
//
// WHY A SEPARATE BAR
// The weapon hotbar (DrawHotbar) is 8 slots bound to the 1-8 row and is
// persistent state saved in TF.sav. Reusing it for spells would mean a weapon
// and a spell compete for the same slot, and the `-1 / 0 / >0` return of
// FireSelectedWeapon (refused / melee / N projectiles) has no room for "refused
// because the pool is empty" - that is a different failure from "refused because
// it is reloading".
//
// SLOT OWNERSHIP
// Slots are independent: binding the same def to two slots gives each its own
// EntityInstance (tracked by EntityInstance::spellSlot), so one slot's cooldown
// never blocks the other. That is why ownership is recorded per instance rather
// than looked up by def name.
// ============================================================================
#include "raylib.h"
#include "MenuStyle.hpp"
#include "../Script/LightningEntityManager.hpp"
#include "../Script/LightningEntityRegistry.hpp"
#include "../Log.hpp"
#include "../Renderer/TextSystem.hpp"
#include "../Pawn/AngelPlayer/SlotBar.hpp"
#include <algorithm>
#include <string>
#include <vector>

namespace oz::spellbook {

// Keys bound to slots 0..4. Kept in step with
// LightningEntityManager::HandleSpellInput, which is the other place they
// appear - and there is no keymap to share them through.
inline const char* SlotKeyLabel(int slot) {
    static const char* kKeys[] = {"Z", "X", "C", "V", "B"};
    return (slot >= 0 && slot < (int)(sizeof(kKeys) / sizeof(kKeys[0]))) ? kKeys[slot] : "?";
}

// Which pool a def spends, resolved the same way CastSpell resolves it.
inline bool UsesPsychicEnergy(const EntityDef* d) {
    if (!d) return false;
    auto it = d->stats.strings.find("cost_type");
    return it != d->stats.strings.end() &&
           (it->second == "psychic_energy" || it->second == "pe");
}

// Is the spell currently castable? Drives the greyed-out HUD slots, so a player
// can see WHY a key will not fire instead of pressing it and getting silence.
inline bool CanCast(int slot) {
    auto& lem = LightningEntityManager::Instance();
    if (slot < 0 || slot >= LightningEntityManager::SPELL_BAR_SIZE) return false;
    if (lem.GetSpellInSlot(slot).empty()) return false;
    if (lem.GetSpellCooldown(slot) > 0.0f) return false;
    const EntityDef* d = LightningEntityRegistry::Instance().Find(lem.GetSpellInSlot(slot));
    const float cost = lem.GetSpellCost(slot);
    return UsesPsychicEnergy(d) ? lem.HasPsychicEnergy(cost) : lem.HasMana(cost);
}

// ---------------------------------------------------------------------------
// HUD spell bar
// ---------------------------------------------------------------------------
// Drawn just above the weapon hotbar. Every slot shows its bound spell, the key
// that casts it, and whether it is ready: a slot on cooldown is drawn with a
// wipe proportional to the remaining time, and an unaffordable spell is dimmed.
inline void DrawSpellBar(int screenW, int screenH) {
    auto& lem = LightningEntityManager::Instance();
    const int slots = LightningEntityManager::SPELL_BAR_SIZE;

    const float cellW = 46.0f, cellH = 46.0f, gap = 6.0f;
    const float barW = slots * cellW + (slots - 1) * gap;
    const float x0 = (screenW - barW) * 0.5f;

    // Stack directly above the weapon hotbar. The hotbar's height scales with
    // the SCREEN WIDTH (hud_width_pct times an atlas aspect), so a fixed pixel
    // offset collides with it at high resolutions - at 2560 wide the hotbar is
    // ~122px tall. Ask SlotBarScreenRect for the real rect instead of
    // duplicating the formula, so the two cannot drift.
    //
    // The vertical clearance also has to clear the hotbar's own slot-name label,
    // which SlotBar draws at barY-20; hence vGap rather than the cell gap.
    const float vGap = 26.0f;
    float hotbarTop;
    {
        SlotBarOptions opt;             // defaults == the in-world hotbar
        Rectangle hotbar{};
        if (SlotBarScreenRect(opt, hotbar)) hotbarTop = hotbar.y;
        else hotbarTop = (float)screenH - 70.0f;   // plain-rect fallback's top
    }
    const float y = hotbarTop - vGap - cellH;

    for (int i = 0; i < slots; i++) {
        const Rectangle cell = {x0 + i * (cellW + gap), y, cellW, cellH};
        const std::string& name = lem.GetSpellInSlot(i);

        if (name.empty()) {
            DrawRectangleRec(cell, (Color){10, 16, 34, 160});
            DrawRectangleLinesEx(cell, 1, angelsui::kBorderDim);
            DrawText(SlotKeyLabel(i), (int)(cell.x + 4), (int)(cell.y + 4), 10,
                     angelsui::kTextDim);
            continue;
        }

        const EntityDef* d = LightningEntityRegistry::Instance().Find(name);
        const float cost = lem.GetSpellCost(i);
        const float cd = lem.GetSpellCooldown(i);
        const bool pe = UsesPsychicEnergy(d);
        const bool ready = (cd <= 0.0f) &&
                           (pe ? lem.HasPsychicEnergy(cost) : lem.HasMana(cost));

        // Mana blue, PE violet - matches the HUD resource bars in
        // InventoryBehaviour so the row reads as "this spends that".
        const Color accent = pe ? (Color){176, 130, 255, 255} : (Color){96, 176, 255, 255};
        DrawRectangleRec(cell, ready ? (Color){18, 28, 56, 225} : (Color){12, 14, 24, 225});
        DrawRectangleLinesEx(cell, 1, ready ? accent : angelsui::kBorderDim);

        DrawText(name.c_str(), (int)(cell.x + 4), (int)(cell.y + 5), 10,
                 ready ? angelsui::kText : angelsui::kTextDim);

        // Cost, with the pool's initial so the player sees both halves of the
        // fraction at a glance.
        char costBuf[32];
        snprintf(costBuf, sizeof(costBuf), "%s%d/%d", pe ? "PE " : "MP ",
                 (int)(pe ? lem.GetPlayerPsychicEnergy() : lem.GetPlayerMana()),
                 (int)cost);
        DrawText(costBuf, (int)(cell.x + 4), (int)(cell.y + cellH - 22), 9,
                 ready ? angelsui::kTextDim : (Color){210, 120, 120, 255});

        DrawText(SlotKeyLabel(i), (int)(cell.x + 4), (int)(cell.y + 4), 10, accent);

        // Cooldown wipe: fills bottom-up as the spell comes back off cooldown.
        if (cd > 0.0f) {
            const float cdTotal = d ? d->stats.floats.count("cooldown")
                                          ? d->stats.floats.at("cooldown") : 1.0f
                                    : 1.0f;
            float t = 1.0f - (cd / (cdTotal > 0.0f ? cdTotal : 1.0f));
            if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
            DrawRectangleRec({cell.x + 1, cell.y + cellH - 4 - (cellH - 6) * t,
                             cellW - 2, (cellH - 6) * t},
                            (Color){40, 90, 170, 150});
        }
    }
}

// ---------------------------------------------------------------------------
// Modal editor
// ---------------------------------------------------------------------------
// Left column: the bar's current bindings (click to clear).
// Right column: every registered `spell` def; click to bind it to the selected
// row. This is the whole editor - there is no per-stat editing, because a spell
// def is an ordinary .ozls and the existing Script Manager already edits those.
inline void DrawSpellbook(bool& show) {
    if (!show) return;
    auto& lem = LightningEntityManager::Instance();

    const float W = 660.0f, H = 460.0f;
    const Rectangle win = {(GetScreenWidth() - W) * 0.5f,
                           (GetScreenHeight() - H) * 0.5f, W, H};

    if (angelsui::Panel(win, "Spellbook")) { show = false; return; }

    // Pool readout: the whole point of the panel is that cost is per-spell and
    // per-pool, so both pools are always visible.
    const float mana = lem.GetPlayerMana(), maxMana = lem.GetPlayerMaxMana();
    const float pe = lem.GetPlayerPsychicEnergy(), maxPe = lem.GetPlayerMaxPsychicEnergy();
    char pools[96];
    snprintf(pools, sizeof(pools), "MP %d/%d    PE %d/%d",
             (int)mana, (int)maxMana, (int)pe, (int)maxPe);
    DrawText(pools, (int)(win.x + 14), (int)(win.y + 38), 12, angelsui::kTextSoft);

    const float x0 = win.x + 14;
    const float leftW = 210.0f;
    const float rightX = win.x + 14 + leftW + 16;
    const float rightW = W - (rightX - win.x) - 14;
    const float rowH = 30.0f, rowGap = 4.0f;
    const float top = win.y + 62;

    // --- Left: current bindings ---
    angelsui::Group({x0, top - 18, leftW, 18 + rowH * LightningEntityManager::SPELL_BAR_SIZE},
                    "Spell bar (click to clear)");
    for (int i = 0; i < LightningEntityManager::SPELL_BAR_SIZE; i++) {
        const Rectangle r = {x0, top + i * (rowH + rowGap), leftW, rowH};
        const std::string& name = lem.GetSpellInSlot(i);
        DrawRectangleRec(r, name.empty() ? angelsui::kTrack : angelsui::kBtnNormal);
        DrawRectangleLinesEx(r, 1, angelsui::kBorderDim);
        const char* label = name.empty() ? "(empty)" : name.c_str();
        DrawText(SlotKeyLabel(i), (int)(r.x + 4), (int)(r.y + 8), 11, angelsui::kEdge);
        DrawText(label, (int)(r.x + 22), (int)(r.y + 8), 11,
                 name.empty() ? angelsui::kTextDim : angelsui::kText);
        if (angelsui::Hover(r) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && !name.empty())
            lem.SetSpellInSlot(i, "");
    }

    // --- Right: the registry of spell defs ---
    angelsui::Group({rightX, top - 18, rightW, 18}, "Known spells (click to bind)");

    std::vector<const EntityDef*> spells;
    LightningEntityRegistry::Instance().FindByType(EntityType::SPELL, spells);
    std::sort(spells.begin(), spells.end(),
              [](const EntityDef* a, const EntityDef* b) { return a->name < b->name; });

    float y = top;
    const float maxY = win.y + H - 16;
    for (const EntityDef* d : spells) {
        if (y + rowH > maxY) {
            // More spells than fit: say so rather than silently truncating the
            // list, which reads as "these are all the spells".
            DrawText("...", (int)(rightX + 4), (int)y, 12, angelsui::kTextDim);
            break;
        }
        const Rectangle r = {rightX, y, rightW, rowH};
        const bool hover = angelsui::Hover(r);
        DrawRectangleRec(r, hover ? angelsui::kBtnHover : angelsui::kBtnNormal);
        DrawRectangleLinesEx(r, 1, angelsui::kBorderDim);

        const bool usesPe = UsesPsychicEnergy(d);
        const float cost = 0.0f;
        char costBuf[48];
        auto cit = d->stats.floats.find(usesPe ? "pe_cost" : "mana_cost");
        const float c = (cit != d->stats.floats.end()) ? cit->second : 0.0f;
        snprintf(costBuf, sizeof(costBuf), "%s%d", usesPe ? "PE " : "MP ", (int)c);
        DrawText(d->name.c_str(), (int)(r.x + 8), (int)(r.y + 8), 11, angelsui::kText);
        DrawText(costBuf, (int)(r.x + r.width - 70), (int)(r.y + 8), 11,
                 usesPe ? (Color){176, 130, 255, 255} : (Color){96, 176, 255, 255});
        (void)cost;

        // Bind to the first slot that is not already occupied by this def.
        // Overwriting an existing binding silently would destroy loadouts with
        // no undo and no confirmation.
        if (hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            int target = -1;
            for (int i = 0; i < LightningEntityManager::SPELL_BAR_SIZE; i++) {
                if (lem.GetSpellInSlot(i).empty()) { target = i; break; }
            }
            if (target >= 0) {
                lem.SetSpellInSlot(target, d->name);
                AngelTechTextSystem.Write(std::string(d->name) + " bound to [" +
                                          SlotKeyLabel(target) + "]");
            } else {
                OZ_WARN("Spellbook: bar is full (5 slots); clear one first");
            }
        }
        y += rowH + rowGap;
    }

    if (spells.empty()) {
        DrawText("No 'spell' defs found under GameData/.",
                 (int)(rightX + 4), (int)top, 11, angelsui::kTextDim);
    }
}

} // namespace oz::spellbook