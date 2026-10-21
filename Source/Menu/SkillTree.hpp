#pragma once
// ---------------------------------------------------------------------------
// SkillTree — "Ethereal Skills" overlay (angelic skill tree).
//
// Displays every `skill` entity discovered by the registry as a tiered
// node graph, styled with the Gold menu frames. Left-click an available node to
// unlock it, spending mana or psychic energy. Unlocked nodes persist through
// LightningEntityManager (TF.sav) and apply their one-time stat bonuses
// (UnlockSkill / RespecSkills). This was labelled "STUB" long after it became a
// working, wired overlay (drawn from Main.cpp's DrawHomeScreen path).
//
// Client-only, header-only (included by Main.cpp).
// ---------------------------------------------------------------------------
#include "raylib.h"
#include "raymath.h"
#include <string>
#include <vector>
#include <algorithm>
#include "../Script/LightningEntityRegistry.hpp"
#include "../Script/LightningEntityManager.hpp"
#include "../Script/LightningEntityDef.hpp"
#include "../Package/PackageAssetLoader.hpp"
#include "../Renderer/TextSystem.hpp"
#include "../Log.hpp"

namespace oz {

namespace skilltree {

inline float StatF(const EntityDef* d, const char* k, float def) {
    if (!d) return def;
    auto it = d->stats.floats.find(k);
    return (it != d->stats.floats.end()) ? it->second : def;
}

inline std::string StatS(const EntityDef* d, const char* k, const std::string& def = "") {
    if (!d) return def;
    auto it = d->stats.strings.find(k);
    if (it == d->stats.strings.end()) return def;
    std::string s = it->second;
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = s.substr(1, s.size() - 2);
    return s;
}

struct Node {
    const EntityDef* def = nullptr;
    int tier = 1;
    int col = 0;
    float cost = 0.0f;
    std::string costType = "mana";
    std::string requiresSkill;
    Rectangle rect{};
};

inline void DrawOverlay(bool& show) {
    if (!show) return;

    auto& lem = LightningEntityManager::Instance();
    int sw = GetScreenWidth();
    int sh = GetScreenHeight();

    DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 175});

    const int panelW = 820, panelH = 560;
    const int px = (sw - panelW) / 2;
    const int py = (sh - panelH) / 2;

    // Ethereal panel: deep violet/indigo with a gold trim (angelic palette).
    DrawRectangle(px, py, panelW, panelH, (Color){18, 16, 38, 248});
    DrawRectangleLines(px, py, panelW, panelH, (Color){210, 180, 90, 255});
    DrawRectangleLines(px + 3, py + 3, panelW - 6, panelH - 6, (Color){90, 80, 150, 255});

    DrawText("ETHEREAL SKILLS", px + 20, py + 14, 26, (Color){245, 225, 150, 255});
    DrawText("Angel's Descent", px + 22, py + 44, 14, (Color){170, 160, 220, 255});

    // Resource readouts (top-right)
    {
        const char* manaTxt = TextFormat("Mana %d/%d",
            (int)lem.GetPlayerMana(), (int)lem.GetPlayerMaxMana());
        const char* peTxt = TextFormat("Psychic %d/%d",
            (int)lem.GetPlayerPsychicEnergy(), (int)lem.GetPlayerMaxPsychicEnergy());
        DrawText(manaTxt, px + panelW - 190, py + 18, 16, (Color){120, 170, 255, 255});
        DrawText(peTxt, px + panelW - 190, py + 40, 16, (Color){200, 120, 255, 255});
    }

    // Collect skill nodes.
    std::vector<const EntityDef*> defs;
    LightningEntityRegistry::Instance().FindByType(EntityType::SKILL, defs);

    std::vector<Node> nodes;
    nodes.reserve(defs.size());
    for (const EntityDef* d : defs) {
        if (!d) continue;
        Node n;
        n.def = d;
        n.tier = std::max(1, (int)StatF(d, "tier", 1.0f));
        n.col = (int)StatF(d, "col", 0.0f);
        n.cost = StatF(d, "cost", 0.0f);
        n.costType = StatS(d, "cost_type", "mana");
        n.requiresSkill = StatS(d, "requires");
        nodes.push_back(n);
    }

    // Layout grid below the header.
    const int nodeSize = 72;
    const int baseX = px + 90;
    const int baseY = py + 110;
    const int colSpacing = 175;
    const int tierSpacing = 118;
    for (auto& n : nodes) {
        n.rect = {(float)(baseX + n.col * colSpacing),
                  (float)(baseY + (n.tier - 1) * tierSpacing),
                  (float)nodeSize, (float)nodeSize};
    }

    auto findNode = [&](const std::string& name) -> Node* {
        for (auto& n : nodes)
            if (n.def && n.def->name == name) return &n;
        return nullptr;
    };

    // Connectors (required -> node).
    for (auto& n : nodes) {
        if (n.requiresSkill.empty()) continue;
        Node* req = findNode(n.requiresSkill);
        if (!req) continue;
        Vector2 a = {req->rect.x + req->rect.width / 2, req->rect.y + req->rect.height / 2};
        Vector2 b = {n.rect.x + n.rect.width / 2, n.rect.y + n.rect.height / 2};
        bool unlocked = lem.IsSkillUnlocked(n.def->name);
        Color c = unlocked ? (Color){220, 190, 100, 255} : (Color){90, 90, 130, 180};
        DrawLineEx(a, b, 2.0f, c);
    }

    // Nodes.
    Vector2 mouse = GetMousePosition();
    bool clicked = IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
    Node* hovered = nullptr;

    for (auto& n : nodes) {
        bool unlocked = lem.IsSkillUnlocked(n.def->name);
        bool reqMet = n.requiresSkill.empty() || lem.IsSkillUnlocked(n.requiresSkill);
        bool available = !unlocked && reqMet;

        float cur = (n.costType == "psychic_energy")
            ? lem.GetPlayerPsychicEnergy() : lem.GetPlayerMana();
        bool afford = cur >= n.cost;

        bool hover = CheckCollisionPointRec(mouse, n.rect);
        if (hover) hovered = &n;

        Color fill, border;
        if (unlocked) { fill = (Color){86, 66, 20, 255}; border = (Color){245, 215, 120, 255}; }
        else if (available && afford) { fill = (Color){26, 32, 62, 255}; border = (Color){120, 200, 255, 255}; }
        else if (available) { fill = (Color){40, 24, 30, 255}; border = (Color){220, 110, 110, 255}; }
        else { fill = (Color){22, 22, 30, 255}; border = (Color){70, 70, 85, 255}; }

        DrawRectangleRec(n.rect, fill);
        DrawRectangleLinesEx(n.rect, hover ? 3.0f : 2.0f, border);

        // Node label (short) + cost.
        const char* label = n.def->name.c_str();
        int tw = MeasureText(label, 11);
        DrawText(label, (int)(n.rect.x + (n.rect.width - tw) / 2), (int)(n.rect.y + 8), 11, WHITE);
        const char* costTxt = TextFormat("%d %s", (int)n.cost,
            (n.costType == "psychic_energy") ? "PE" : "MP");
        int cw = MeasureText(costTxt, 10);
        DrawText(costTxt, (int)(n.rect.x + (n.rect.width - cw) / 2),
                 (int)(n.rect.y + n.rect.height - 16), 10,
                 unlocked ? (Color){245, 215, 120, 255} : LIGHTGRAY);

        if (clicked && hover && available) {
            if (afford) {
                if (n.costType == "psychic_energy")
                    lem.SetPlayerPsychicEnergy(cur - n.cost);
                else
                    lem.SetPlayerMana(cur - n.cost);
                lem.UnlockSkill(n.def->name);
                OZ_INFO("SkillTree: unlocked '%s'", n.def->name.c_str());
                AngelTechTextSystem.Write("Ethereal skill awakened: " + n.def->name);
            } else {
                AngelTechTextSystem.Write("Not enough " +
                    std::string(n.costType == "psychic_energy" ? "psychic energy" : "mana") +
                    " for " + n.def->name);
            }
        }
    }

    // Respec button (refunds every unlocked node).
    {
        int bw = 170, bh = 24;
        int bx = px + panelW - bw - 20;
        int by = py + 64;
        Rectangle btn = {(float)bx, (float)by, (float)bw, (float)bh};
        bool hover = CheckCollisionPointRec(mouse, btn);
        bool canRespec = !lem.UnlockedSkills().empty();
        Color fill = canRespec
            ? (hover ? (Color){120, 60, 40, 255} : (Color){80, 40, 30, 255})
            : (Color){40, 40, 48, 255};
        Color border = canRespec ? (Color){245, 160, 90, 255} : (Color){80, 80, 90, 255};
        DrawRectangleRec(btn, fill);
        DrawRectangleLinesEx(btn, 2.0f, border);
        const char* txt = "Respec (refund all)";
        int tw2 = MeasureText(txt, 12);
        DrawText(txt, bx + (bw - tw2) / 2, by + 6, 12, canRespec ? WHITE : LIGHTGRAY);
        if (canRespec && hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            lem.RespecSkills();
            AngelTechTextSystem.Write("Ethereal skills reset - resources refunded.");
        }
    }

    // Tooltip for the hovered node.
    if (hovered && hovered->def) {
        const EntityDef* d = hovered->def;
        std::vector<std::string> lines;
        lines.push_back(d->name);
        lines.push_back(TextFormat("Cost: %d", (int)hovered->cost));
        auto addBonus = [&](const char* key, const char* label) {
            auto it = d->stats.floats.find(key);
            if (it != d->stats.floats.end())
                lines.push_back(TextFormat("%s +%.0f", label, it->second));
        };
        addBonus("max_health_bonus", "Max Health");
        addBonus("max_mana_bonus", "Max Mana");
        addBonus("max_psychic_energy_bonus", "Max Psychic");
        addBonus("move_speed_bonus", "Move Speed");
        if (!hovered->requiresSkill.empty())
            lines.push_back("Requires: " + hovered->requiresSkill);

        int wMax = 0;
        for (auto& l : lines) wMax = std::max(wMax, MeasureText(l.c_str(), 12));
        int tw = wMax + 20;
        int th = (int)lines.size() * 16 + 12;
        int tx = std::min((int)mouse.x + 16, sw - tw - 4);
        int ty = std::min((int)mouse.y + 16, sh - th - 4);
        DrawRectangle(tx, ty, tw, th, (Color){10, 10, 24, 240});
        DrawRectangleLines(tx, ty, tw, th, (Color){210, 180, 90, 255});
        int ly = ty + 6;
        for (size_t i = 0; i < lines.size(); i++) {
            DrawText(lines[i].c_str(), tx + 8, ly, 12, i == 0 ? (Color){245, 225, 150, 255} : WHITE);
            ly += 16;
        }
    }

    DrawText("K: close   |   Left click an available node to unlock (costs Mana / Psychic Energy)",
             px + 20, py + panelH - 26, 12, (Color){150, 145, 180, 255});
}

} // namespace skilltree
} // namespace oz
