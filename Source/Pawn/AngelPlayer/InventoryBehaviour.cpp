#include "../../WindowsCompat.hpp"
#include "InventoryBehaviour.hpp"
#include "../Items.hpp"
#include "../OzPawnSystem.hpp"
#include "SlotBar.hpp"
#include "../../Script/LightningEntityManager.hpp"
#include "../../Script/LightningEntityRegistry.hpp"
#include "../../Input/Keybinds.hpp"
#include "../../Renderer/OzAssetMapper.hpp"
#include "../../Client/Client.hpp"
#include "../../Game/GameManager.hpp"
#include "../../Events/ObjectiveSystem.hpp"
#include "../../GameplayFlags.hpp"
#include "../../Log.hpp"
#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

void InventoryBehaviour::Emit(const std::string& msg) {
    if (m_messageSink) m_messageSink(msg);
}

// ---------------------------------------------------------------------------
// Consume one backpack item and apply its effect
// ---------------------------------------------------------------------------
bool InventoryBehaviour::UseBackpackItem(int slot) {
    if (slot < 0 || slot >= BACKPACK_SLOTS) return false;
    int itemId = gInventory.backpack[slot].itemId;
    if (itemId < 0 || gInventory.backpack[slot].quantity <= 0) return false;
    const ItemDBEntry* def = GetItemDef(itemId);
    if (!def) return false;

    // Single shared effect path (see ApplyItemEffect). Coins are excluded on
    // purpose: they previously fell into a usable case, so using a coin stack
    // minted a coin per click instead of doing nothing.
    if (def->category == ItemCategory::COIN) return false;
    if (!ApplyItemEffect(*def, 1)) return false;

    gInventory.RemoveFromBackpack(slot);
    Emit(std::string("Used ") + def->name);
    return true;
}

// ---------------------------------------------------------------------------
// Drop: remove an item/weapon from the player and leave it in the world
// ---------------------------------------------------------------------------
void InventoryBehaviour::SpawnDroppedPickup(const char* typeName, const Camera3D& cam) {
    if (!typeName || !*typeName) return;
    // In front of the player, at roughly foot height, so the pickup lands on the
    // ground in view instead of inside the player's own collect box.
    Vector3 fwd = Vector3Subtract(cam.target, cam.position);
    fwd.y = 0.0f;
    float len = sqrtf(fwd.x * fwd.x + fwd.z * fwd.z);
    if (len > 1e-4f) { fwd.x /= len; fwd.z /= len; }
    else             { fwd = Vector3{0.0f, 0.0f, 1.0f}; }

    PickupNode pn;
    pn.position = {cam.position.x + fwd.x * 1.4f,
                   cam.position.y - 1.2f,
                   cam.position.z + fwd.z * 1.4f};
    pn.typeName = typeName;
    pn.respawnTime = 0.0f;   // a dropped item is gone once picked back up
    pn.active = true;
    PawnSystem::Instance().AddPickup(pn);
}

bool InventoryBehaviour::DropBackpackItem(int slot, const Camera3D& cam) {
    if (slot < 0 || slot >= BACKPACK_SLOTS) return false;
    int itemId = gInventory.backpack[slot].itemId;
    if (itemId < 0 || gInventory.backpack[slot].quantity <= 0) return false;
    const ItemDBEntry* def = GetItemDef(itemId);
    const char* typeName = PickupTypeNameForItem(itemId);
    if (!typeName) {
        Emit(std::string("Cannot drop ") + (def ? def->name : "item"));
        return false;
    }
    // Remove ONE (a stack is dropped a piece at a time; PickupNode carries no
    // quantity, so dropping the whole stack would destroy the remainder).
    gInventory.RemoveFromBackpack(slot);
    SpawnDroppedPickup(typeName, cam);
    Emit(std::string("Dropped ") + (def ? def->name : typeName));
    if (m_feedbackSink) m_feedbackSink();
    return true;
}

bool InventoryBehaviour::DropHotbarSlot(int slot, const Camera3D& cam) {
    auto& lem = LightningEntityManager::Instance();
    int inst = lem.HotbarAt(slot);
    if (inst < 0) return false;
    EntityInstance* e = lem.Get(inst);
    if (!e || !e->def) return false;
    const std::string name = e->def->name;
    lem.HotbarAssign(slot, -1);   // fires on_unequip
    lem.Despawn(inst);
    SpawnDroppedPickup(name.c_str(), cam);
    Emit(std::string("Dropped ") + name);
    if (m_feedbackSink) m_feedbackSink();
    // A freed cell is the moment a stashed overflow weapon can finally land.
    lem.FlushNextStashedWeapon();
    return true;
}

bool InventoryBehaviour::DropEquipmentSlot(int slot, const Camera3D& cam) {
    auto& lem = LightningEntityManager::Instance();
    int inst = lem.EquipmentAt(slot);
    if (inst < 0) return false;
    EntityInstance* e = lem.Get(inst);
    if (!e || !e->def) return false;
    const std::string name = e->def->name;
    lem.EquipmentUnequip(slot);   // fires on_unequip and despawns the instance
    SpawnDroppedPickup(name.c_str(), cam);
    Emit(std::string("Dropped ") + name);
    if (m_feedbackSink) m_feedbackSink();
    return true;
}

// ---------------------------------------------------------------------------
// HUD: always-visible player stat bars
// ---------------------------------------------------------------------------
void InventoryBehaviour::DrawHud(const Camera3D& cam, int ticker, bool isCrouching, bool isSprinting) {
    const int sw = GetScreenWidth();
    const int sh = GetScreenHeight();
    float hudS = g_hudScale;
    if (hudS < 0.5f) hudS = 0.5f;
    if (hudS > 2.0f) hudS = 2.0f;
    int bar_w = (int)(220 * hudS);
    int bar_h = (int)(14 * hudS);
    int pad = (int)(6 * hudS);
    int x = pad;
    int y = pad;

    if (m_xpFlashTimer > 0.0f) m_xpFlashTimer -= GetFrameTime();
    if (m_levelUpTimer > 0.0f) m_levelUpTimer -= GetFrameTime();

    // XP / level feedback detection. Both the single-player script path
    // (give_xp -> LEM::SetPlayerXP) and the networked path (Main.cpp copies
    // XP_UPDATE into the same LEM stats) land here, so one comparison covers
    // both. A level-up resets xp to 0, so the xp delta only reports gains
    // within a level; the level comparison reports the level itself.
    {
        auto& lem = LightningEntityManager::Instance();
        const int xpNow  = lem.GetPlayerXP();
        const int lvlNow = lem.GetPlayerLevel();
        if (m_lastXp < 0 || m_lastLevel < 0) {
            m_lastXp = xpNow;
            m_lastLevel = lvlNow;
        } else {
            if (xpNow > m_lastXp)  OnXpGain(xpNow - m_lastXp);
            if (lvlNow > m_lastLevel) OnLevelUp(lvlNow);
            m_lastXp = xpNow;
            m_lastLevel = lvlNow;
        }
    }

    // Level
    DrawTextEx(GetFontDefault(), TextFormat("Lv.%d", LightningEntityManager::Instance().GetPlayerLevel()),
               {(float)x, (float)y}, 14.0f * hudS, 1.0f, WHITE);
    y += (int)(18 * hudS);

    // XP bar
    int xp_w = bar_w - (int)(40 * hudS);
    DrawTextEx(GetFontDefault(), "XP", {(float)x, (float)y}, 12.0f * hudS, 1.0f, LIGHTGRAY);
    int xpToNext = LightningEntityManager::Instance().GetPlayerXPToNext();
    if (xpToNext > 0) {
        int xp = LightningEntityManager::Instance().GetPlayerXP();
        float xp_pct = (float)xp / xpToNext;
        if (xp_pct < 0.0f) xp_pct = 0.0f;
        if (xp_pct > 1.0f) xp_pct = 1.0f;
        int bx = x + (int)(30 * hudS);
        DrawRectangle(bx - 1, y - 1, xp_w + 2, bar_h + 2, (Color){10, 10, 10, 200});
        DrawRectangle(bx, y, xp_w, bar_h, (Color){25, 25, 35, 235});
        DrawRectangle(bx, y, (int)(xp_pct * xp_w), bar_h, SKYBLUE);
        // Gain flash: an overlay that fades out over the fill, instead of tinting
        // the fill itself (tinting made the bar look EMPTY at the end of the
        // flash, since the tint's alpha was the fade).
        if (m_xpFlashTimer > 0.0f) {
            float ft = m_xpFlashTimer / 0.6f;
            if (ft < 0.0f) ft = 0.0f;
            if (ft > 1.0f) ft = 1.0f;
            DrawRectangle(bx, y, (int)(xp_pct * xp_w), bar_h,
                          (Color){255, 246, 190, (unsigned char)(180 * ft)});
        }
        DrawTextEx(GetFontDefault(), TextFormat("%d/%d", xp, xpToNext), {(float)(bx + 4), (float)(y + 1)}, 10.0f * hudS, 1.0f, WHITE);
        if (m_xpFlashTimer > 0.0f && m_xpFlashGain > 0) {
            float ft = m_xpFlashTimer / 0.6f;
            if (ft < 0.0f) ft = 0.0f;
            Color fc = {255, 240, 160, (unsigned char)(230 * ft)};
            DrawTextEx(GetFontDefault(), TextFormat("+%d XP", m_xpFlashGain), {(float)(bx + xp_w - (int)(42 * hudS)), (float)(y - (int)(14 * hudS))}, 12.0f * hudS, 1.0f, fc);
        }
    } else {
        int bx = x + (int)(30 * hudS);
        DrawRectangle(bx - 1, y - 1, xp_w + 2, bar_h + 2, (Color){10, 10, 10, 200});
        DrawRectangle(bx, y, xp_w, bar_h, (Color){25, 25, 35, 235});
    }
    y += bar_h + pad;

    // Health bar
    float hpMax = LightningEntityManager::Instance().GetPlayerMaxHealth();
    float hp = LightningEntityManager::Instance().GetPlayerHealth();
    float hp_pct = (hpMax > 0) ? (hp / hpMax) : 0;
    if (hp_pct < 0.0f) hp_pct = 0.0f;
    if (hp_pct > 1.0f) hp_pct = 1.0f;
    DrawTextEx(GetFontDefault(), TextFormat("HP %d/%d", (int)hp, (int)hpMax), {(float)x, (float)y}, 12.0f * hudS, 1.0f, WHITE);
    DrawRectangle(x - 1, y + (int)(14 * hudS) - 1, bar_w + 2, bar_h + 2, (Color){10, 10, 10, 200});
    DrawRectangle(x, y + (int)(14 * hudS), bar_w, bar_h, (Color){60, 12, 12, 235});
    DrawRectangle(x, y + (int)(14 * hudS), (int)(hp_pct * bar_w), bar_h, (Color){230, 40, 40, 255});
    y += (int)(14 * hudS) + bar_h + pad;

    // Mana bar
    float mpMax = LightningEntityManager::Instance().GetPlayerMaxMana();
    float mp = LightningEntityManager::Instance().GetPlayerMana();
    float mp_pct = (mpMax > 0) ? (mp / mpMax) : 0;
    if (mp_pct < 0.0f) mp_pct = 0.0f;
    if (mp_pct > 1.0f) mp_pct = 1.0f;
    DrawTextEx(GetFontDefault(), TextFormat("MP %d/%d", (int)mp, (int)mpMax), {(float)x, (float)y}, 12.0f * hudS, 1.0f, WHITE);
    DrawRectangle(x - 1, y + (int)(14 * hudS) - 1, bar_w + 2, bar_h + 2, (Color){10, 10, 10, 200});
    DrawRectangle(x, y + (int)(14 * hudS), bar_w, bar_h, (Color){12, 18, 60, 235});
    DrawRectangle(x, y + (int)(14 * hudS), (int)(mp_pct * bar_w), bar_h, (Color){70, 130, 255, 255});
    y += (int)(14 * hudS) + bar_h + pad;

    // Psychic Energy bar
    float peMax = LightningEntityManager::Instance().GetPlayerMaxPsychicEnergy();
    float pe = LightningEntityManager::Instance().GetPlayerPsychicEnergy();
    float pe_pct = (peMax > 0) ? (pe / peMax) : 0;
    if (pe_pct < 0.0f) pe_pct = 0.0f;
    if (pe_pct > 1.0f) pe_pct = 1.0f;
    DrawTextEx(GetFontDefault(), TextFormat("PE %d/%d", (int)pe, (int)peMax), {(float)x, (float)y}, 12.0f * hudS, 1.0f, WHITE);
    DrawRectangle(x - 1, y + (int)(14 * hudS) - 1, bar_w + 2, bar_h + 2, (Color){10, 10, 10, 200});
    DrawRectangle(x, y + (int)(14 * hudS), bar_w, bar_h, (Color){38, 12, 54, 235});
    DrawRectangle(x, y + (int)(14 * hudS), (int)(pe_pct * bar_w), bar_h, (Color){170, 90, 255, 255});
    y += (int)(14 * hudS) + bar_h + pad;

    // Current selected item/weapon (from EntityManager hotbar)
    {
        auto& lem = LightningEntityManager::Instance();
        EntityInstance* selEnt = lem.SelectedEntity();
        int selSlot = lem.SelectedSlot() + 1;
        const char* label = "Empty";
        if (selEnt && selEnt->def)
            label = selEnt->def->name.c_str();
        DrawTextEx(GetFontDefault(), TextFormat("Slot %d: %s", selSlot, label),
                   {(float)x, (float)y}, 12.0f * hudS, 1.0f, YELLOW);
        y += (int)(16 * hudS);
        // Ammo display for weapons
        if (selEnt && selEnt->def && selEnt->def->type == EntityType::WEAPON) {
            auto ait = selEnt->runtimeStats.find("ammo");
            auto mit = selEnt->runtimeStats.find("magazine");
            if (ait != selEnt->runtimeStats.end() && mit != selEnt->runtimeStats.end()) {
                Color ammoCol = (ait->second <= 0.0f) ? RED : WHITE;
                DrawTextEx(GetFontDefault(), TextFormat("Ammo: %.0f/%.0f", ait->second, mit->second),
                           {(float)x, (float)y}, 12.0f * hudS, 1.0f, ammoCol);
                y += (int)(16 * hudS);
            }
        }
    }

    // Stashed overflow weapons. When the hotbar is full a collected weapon is
    // queued rather than lost, so the player needs to know it is pending and
    // how to free it (drop or consume a hotbar item).
    {
        int stashed = LightningEntityManager::Instance().StashedWeaponCount();
        if (stashed > 0) {
            DrawTextEx(GetFontDefault(), TextFormat("Stored weapons: %d (free a hotbar slot)", stashed),
                       {(float)x, (float)y}, 12.0f * hudS, 1.0f, (Color){255, 190, 90, 255});
            y += (int)(16 * hudS);
        }
    }

    // Weapon fire indicator (brief pulse)
    static double lastFireTime = 0;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        lastFireTime = GetTime();
    }
    if (GetTime() - lastFireTime < 0.15) {
        DrawTextEx(GetFontDefault(), "FIRE", {(float)x, (float)y}, 18.0f * hudS, 1.0f, RED);
    }

    // Movement stance indicator (sprint / crouch)
    if (isCrouching)
        DrawTextEx(GetFontDefault(), "CROUCH", {(float)pad, (float)(sh - (int)(46 * hudS))},
                   16.0f * hudS, 1.0f, (Color){120, 200, 255, 255});
    else if (isSprinting)
        DrawTextEx(GetFontDefault(), "SPRINT", {(float)pad, (float)(sh - (int)(46 * hudS))},
                   16.0f * hudS, 1.0f, (Color){255, 210, 120, 255});

    // Scoreboard (top-right, below position/rotation)
    // NOTE: these two blocks used to `return` when there was no client, which in
    // single-player skipped EVERYTHING after them - objectives, the match banner,
    // the level-up banner and the coordinate readout. A missing client is a
    // reason to skip the scoreboard, not to abandon the rest of the HUD.
    if (m_client) {
        const auto& scores = m_client->scores();
        if (!scores.empty()) {
            int sbX = sw - (int)(200 * hudS);
            int sbY = (int)(60 * hudS);
            int rowH = (int)(18 * hudS);
            int sbH = (int)scores.size() * rowH + (int)(8 * hudS);
            DrawRectangle(sbX - (int)(4 * hudS), sbY - (int)(4 * hudS),
                          (int)(196 * hudS), sbH, (Color){0, 0, 0, 160});
            int row = 0;
            for (const auto& [pid, ss] : scores) {
                DrawTextEx(GetFontDefault(), TextFormat("P%d  %d", pid, ss.score),
                           {(float)sbX, (float)(sbY + row * rowH)}, 14.0f * hudS, 1.0f,
                           ss.team == 0 ? (Color){255, 200, 100, 255} : (Color){100, 200, 255, 255});
                row++;
            }
        }
    }

    // Objectives. Co-op mirrors the server's OBJECTIVE_STATE; single-player
    // uses the local store GameManager seeds when a mission is bound — offline
    // there is no server to seed or advance one, and this block used to read
    // only the server mirror, so a campaign mission showed nothing at all.
    {
        const int serverWorld = m_client ? m_client->server_world_index() : -1;
        const Color kObjDone{120, 220, 120, 255};
        const Color kObjTodo{220, 220, 220, 255};
        int ox = (int)(16 * hudS), oy = (int)(300 * hudS);
        bool drew = false;

        auto stripPrefix = [](const std::string& id) -> std::string {
            static const std::string kPrefix = "Objective_";
            return id.rfind(kPrefix, 0) == 0 ? id.substr(kPrefix.size()) : id;
        };

        if (m_client && serverWorld >= 0) {
            auto objs = m_client->objectives(serverWorld);
            if (!objs.empty()) {
                DrawTextEx(GetFontDefault(), "OBJECTIVES", {(float)ox, (float)oy},
                           14.0f * hudS, 1.0f, (Color){255, 220, 140, 255});
                oy += (int)(20 * hudS);
                for (const auto& o : objs) {
                    const bool done = o.target > 0 && o.progress >= o.target;
                    DrawTextEx(GetFontDefault(), TextFormat("%s  %d/%d", stripPrefix(o.id).c_str(),
                                                            o.progress, o.target),
                               {(float)ox, (float)oy}, 13.0f * hudS, 1.0f,
                               done ? kObjDone : kObjTodo);
                    oy += (int)(16 * hudS);
                }
                drew = true;
            }
        }

        auto& gm = oz::game::GameManager::Instance();
        if (!drew && gm.Loaded() && !gm.ActiveObjectives().empty()) {
            auto& os = oz::events::ObjectiveSystem::Instance();
            DrawTextEx(GetFontDefault(), "OBJECTIVES", {(float)ox, (float)oy},
                       14.0f * hudS, 1.0f, (Color){255, 220, 140, 255});
            oy += (int)(20 * hudS);
            for (const auto& o : gm.ActiveObjectives()) {
                const auto* st = os.Get(o.id);
                const int progress = st ? st->progress : 0;
                const bool done = st && st->complete;
                const char* verb = (o.type == "collect") ? "Collect"
                                 : (o.type == "kill")    ? "Kill"
                                                         : "Do";
                const char* label = o.target.empty() ? "targets" : o.target.c_str();
                DrawTextEx(GetFontDefault(), TextFormat("%s %s  %d/%d", verb, label, progress, o.count),
                           {(float)ox, (float)oy}, 13.0f * hudS, 1.0f, done ? kObjDone : kObjTodo);
                oy += (int)(16 * hudS);
            }
        }
    }

    // Match-over banner with fade (1.0s in, hold, 3.0s out)
    {
        if (m_client && m_client->match_over()) {
            double elapsed = GetTime() - m_client->match_over_at();
            float alpha = 1.0f;
            if (elapsed < 1.0f) {
                alpha = (float)elapsed;
            } else if (elapsed > 4.0f) {
                alpha = 0.0f;
            } else if (elapsed > 1.0f) {
                alpha = 1.0f - (float)(elapsed - 1.0f) / 3.0f;
            }
            if (alpha > 0.0f) {
                const char* banner = m_client->winning_team() >= 0
                    ? TextFormat("Team %d wins!", m_client->winning_team())
                    : "Draw";
                int w = MeasureText(banner, (int)(28 * hudS));
                DrawText(banner, (sw - w) / 2, sh / 2 - (int)(40 * hudS), (int)(28 * hudS),
                         (Color){255, 220, 100, (unsigned char)(alpha * 255)});
            }
        }
    }

    // Level up notification (center top)
    if (m_levelUpTimer > 0.0f) {
        float lt = m_levelUpTimer / 2.5f;
        if (lt < 0.0f) lt = 0.0f;
        int ftSize = (int)(28.0f * hudS);
        const char* lvlMsg = TextFormat("LEVEL UP! - Lv.%d", m_levelUpLevel);
        int lw = MeasureText(lvlMsg, ftSize);
        Color lc = {255, 230, 150, (unsigned char)(255 * (lt > 0.2f ? 1.0f : lt / 0.2f))};
        DrawRectangle((sw - lw - 40) / 2, (int)(18 * hudS), lw + 40, (int)(36 * hudS), (Color){15, 15, 25, (unsigned char)(160 * lt)});
        DrawRectangleLines((sw - lw - 40) / 2, (int)(18 * hudS), lw + 40, (int)(36 * hudS), lc);
        DrawText(lvlMsg, (sw - lw) / 2, (int)(26 * hudS), ftSize, lc);
    }

    // Coordinates (top-right) - keep visible for debugging
    float yaw = -atan2f(cam.target.x - cam.position.x, cam.target.z - cam.position.z) * RAD2DEG;
    float pitch = asinf((cam.target.y - cam.position.y) /
        Vector3Distance(cam.position, cam.target)) * RAD2DEG;
    int rx = sw - (int)(290 * hudS);
    DrawTextEx(GetFontDefault(), TextFormat("Pos: %.1f %.1f %.1f", cam.position.x, cam.position.y, cam.position.z),
               {(float)rx, 10.0f}, 14.0f * hudS, 1.0f, LIGHTGRAY);
    DrawTextEx(GetFontDefault(), TextFormat("Rot: %.0f %.0f", yaw, pitch),
               {(float)rx, 10.0f + 18.0f * hudS}, 14.0f * hudS, 1.0f, LIGHTGRAY);
    if ((ticker % 60) == 0) {
        fprintf(stderr, "POS: %.1f %.1f %.1f  ROT: %.0f %.0f\n",
                cam.position.x, cam.position.y, cam.position.z, yaw, pitch);
    }
}

// ---------------------------------------------------------------------------
// Inventory Overlay (Diablo I style)
// ---------------------------------------------------------------------------
void InventoryBehaviour::DrawOverlay(const Camera3D& cam) {
    const int sw = GetScreenWidth();
    const int sh = GetScreenHeight();

    // --- Scaling -------------------------------------------------------------
    // The panel was authored at a fixed 720x520 with every offset a literal, so
    // on a 4K display it covered a sixth of the screen and at 1280x720 it barely
    // fit. Everything below is now derived from ONE factor:
    //   * the user's UI scale (same value the menu uses), pushed into g_hudScale;
    //   * an automatic fit so the panel never exceeds the window.
    // 720x520 still fits 1280x720 at 1.0, which is the supported minimum.
    const float baseW = 720.0f, baseH = 520.0f;
    float ui = g_hudScale;
    if (ui < 0.5f) ui = 0.5f;
    if (ui > 2.0f) ui = 2.0f;
    float fitW = (float)(sw - 32) / baseW;
    float fitH = (float)(sh - 32) / baseH;
    float fit  = fitW < fitH ? fitW : fitH;
    float S = fit * ui;
    if (S < 0.55f) S = 0.55f;   // floor: below this the labels are unreadable
    if (S > 2.5f)  S = 2.5f;

    // Scaled font size / pixel offset helper. Every literal size below goes
    // through these so the panel scales as one piece.
    auto FS = [S](float v) { int r = (int)(v * S + 0.5f); return r < 1 ? 1 : r; };

    const int panel_w = (int)(baseW * S);
    const int panel_h = (int)(baseH * S);
    const int px = (sw - panel_w) / 2;
    const int py = (sh - panel_h) / 2;

    // Layout constants shared by the draw pass and the drag hit-test below.
    const int ex = px + FS(20);          // equipment column x
    const int slotH = FS(38), slotW = FS(150);
    const int bx = px + FS(190);         // backpack grid x
    const int by = py + FS(50);
    const int cellSize = FS(52), cellGap = FS(4);

    auto& lem = LightningEntityManager::Instance();

    // === Drag-and-drop: resolve an in-flight drag before drawing ===
    //
    // Click-to-pick used to live inside each cell's hover block, so a press
    // both selected AND armed a use. A drag needs press / release on DIFFERENT
    // cells, which cannot be decided in the per-cell block (the release cell is
    // a different iteration). So: a press that lands on an occupied cell starts a
    // drag; the drop is resolved here against the current cursor position, and
    // the per-cell blocks below only start drags and render the drop highlight.
    //
    // Namespace rule: a drag only lands on the same kind it started from
    // (backpack<->backpack, equipment<->equipment). Item ids and entity
    // instances are not interchangeable and there is no bridge between them, so
    // a cross-kind release is ignored rather than half-applying.
    if (m_dragKind != DragKind::NONE) {
        const Vector2 mp = GetMousePosition();
        // Right-click cancels rather than dropping, so a mis-press is undoable
        // without the pieces having moved.
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            m_dragKind = DragKind::NONE;
            m_dragSlot = -1;
        } else if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (m_dragKind == DragKind::BACKPACK) {
                for (int i = 0; i < BACKPACK_SLOTS; i++) {
                    const int r = i / BACKPACK_COLS, c = i % BACKPACK_COLS;
                    Rectangle cell = {(float)(bx + c * (cellSize + cellGap)),
                                      (float)(by + r * (cellSize + cellGap)),
                                      (float)cellSize, (float)cellSize};
                    if (CheckCollisionPointRec(mp, cell)) {
                        if (i != m_dragSlot) gInventory.SwapBackpackSlots(m_dragSlot, i);
                        break;
                    }
                }
            } else { // EQUIPMENT
                for (int s = 0; s < EQUIP_SLOT_COUNT; s++) {
                    Rectangle r = {(float)ex, (float)(py + 50 + s * (slotH + 4)),
                                   (float)slotW, (float)slotH};
                    if (CheckCollisionPointRec(mp, r)) {
                        if (s != m_dragSlot) lem.EquipmentSwap(m_dragSlot, s);
                        break;
                    }
                }
            }
            m_dragKind = DragKind::NONE;
            m_dragSlot = -1;
        }
        // If the release happened outside every drop target, clear the drag so a
        // stale source can never apply to a freshly-opened panel.
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) &&
            !IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            !IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_dragKind = DragKind::NONE;
            m_dragSlot = -1;
        }
    }

    // Panel chrome: dimmed backdrop, body, hairline border, top accent rule.
    DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 170});
    DrawRectangle(px, py, panel_w, panel_h, (Color){22, 24, 34, 248});
    DrawRectangleLines(px, py, panel_w, panel_h, (Color){190, 190, 215, 255});
    DrawRectangle(px + FS(2), py + FS(2), panel_w - FS(4), FS(2), (Color){210, 200, 150, 90});

    DrawTextEx(GetFontDefault(), "INVENTORY", {(float)(px + FS(15)), (float)(py + FS(10))},
               22.0f * S, 1.0f, WHITE);
    {
        const char* coinTxt = TextFormat("Coins: %d", gInventory.coins);
        int cw = MeasureText(coinTxt, FS(16));
        DrawTextEx(GetFontDefault(), coinTxt,
                   {(float)(px + panel_w - cw - FS(15)), (float)(py + FS(14))},
                   16.0f * S, 1.0f, GOLD);
    }

    int ey = py + FS(50);

    // Hover targets for the Q-to-drop action, resolved by the draw loops below.
    int hoveredEq = -1;
    int hoveredBp = -1;
    int hoveredHotbar = -1;

    DrawTextEx(GetFontDefault(), "EQUIPMENT", {(float)ex, (float)(ey - FS(18))},
               12.0f * S, 1.0f, LIGHTGRAY);

    // Labels now live in Pawn/EquipSlots.hpp, sized by EQUIP_SLOT_COUNT, so a new
    // equipment slot without a label is a compile error instead of a blank row.

    for (int i = 0; i < EQUIP_SLOT_COUNT; i++) {
        int idx = lem.EquipmentAt(i);
        bool owned = (idx >= 0 && lem.Get(idx) && lem.Get(idx)->owned);
        Color c = owned ? WHITE : (Color){80, 80, 80, 255};
        Color bg = owned ? (Color){40, 50, 45, 255} : (Color){20, 25, 20, 255};

        Rectangle eqRect = {(float)ex, (float)ey, (float)slotW, (float)slotH};
        bool eqHover = CheckCollisionPointRec(GetMousePosition(), eqRect);
        if (eqHover && owned) hoveredEq = i;
        // Drop target highlight while an equipment drag is in flight.
        bool eqDropTarget = (m_dragKind == DragKind::EQUIPMENT && m_dragSlot >= 0 &&
                             m_dragSlot != i && eqHover);
        if (eqDropTarget) c = (Color){120, 200, 255, 255};

        // Start a drag on press over an owned piece. The drop resolves in the
        // pre-pass above (a release can land on a different row).
        if (eqHover && owned && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            m_dragKind == DragKind::NONE) {
            m_dragKind = DragKind::EQUIPMENT;
            m_dragSlot = i;
        }
        // Right-click / press-and-release-in-place unequips (kept from the old
        // click-to-unequip; a same-slot release is a no-op swap).
        if (eqHover && owned && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            lem.EquipmentUnequip(i);
            m_dragKind = DragKind::NONE;
            m_dragSlot = -1;
        }

        DrawRectangle(ex, ey, slotW, slotH, bg);
        DrawRectangleLines(ex, ey, slotW, slotH, c);
        DrawTextEx(GetFontDefault(), kEquipSlotLabels[i],
                   {(float)(ex + FS(6)), (float)(ey + FS(12))}, 12.0f * S, 1.0f, c);

        if (owned && lem.Get(idx)->iconIdx >= 0) {
            Texture2D* iconTex = (Texture2D*)lem.GetIcon(lem.Get(idx)->iconIdx);
            if (iconTex && iconTex->id > 0)
                DrawTextureEx(*iconTex, (Vector2){(float)(ex + slotW - FS(34)), (float)(ey + FS(2))},
                              0, 1.5f * S, WHITE);
        }
        ey += slotH + FS(4);
    }

    // === BACKPACK (right side) ===
    // bx/by/cellSize/cellGap are declared once at the top of DrawOverlay so the
    // drag hit-test and this draw pass agree; redeclaring them here used to be
    // the pattern and is what made the two passes drift.
    DrawTextEx(GetFontDefault(), "BACKPACK", {(float)bx, (float)(by - FS(18))},
               12.0f * S, 1.0f, LIGHTGRAY);

    for (int row = 0; row < BACKPACK_ROWS; row++) {
        for (int col = 0; col < BACKPACK_COLS; col++) {
            int idx = row * BACKPACK_COLS + col;
            int cx = bx + col * (cellSize + cellGap);
            int cy = by + row * (cellSize + cellGap);

            int itemId = gInventory.backpack[idx].itemId;
            int qty = gInventory.backpack[idx].quantity;
            bool hasItem = itemId >= 0 && qty > 0;

            Rectangle cellRect = {(float)cx, (float)cy, (float)cellSize, (float)cellSize};
            bool hover = CheckCollisionPointRec(GetMousePosition(), cellRect);
            if (hover && hasItem) hoveredBp = idx;

            // Press on an occupied cell starts a drag (and selects it). The drop
            // resolves in the pre-pass; a press-and-release on the SAME cell is a
            // no-op swap, so click-to-select still works. Right-click keeps the
            // old use-an-item behaviour.
            if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (hasItem && m_dragKind == DragKind::NONE) {
                    m_dragKind = DragKind::BACKPACK;
                    m_dragSlot = idx;
                    m_selectedBpSlot = idx;
                } else if (!hasItem && m_dragKind == DragKind::BACKPACK) {
                    // Drop onto an empty cell: the pre-pass already moved the
                    // source here, so just clear the selection highlight.
                    m_selectedBpSlot = -1;
                }
            }
            if (hover && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && hasItem) {
                if (UseBackpackItem(idx)) { itemId = -1; qty = 0; hasItem = false; }
            }

            Color bgC = hasItem ? (Color){40, 40, 55, 255} : (Color){15, 15, 20, 255};
            Color borderC = hasItem ? WHITE : (Color){50, 50, 50, 255};

            if (hover) borderC = (Color){120, 200, 255, 255};
            // Green outline = legal drop target for the drag in flight.
            if (m_dragKind == DragKind::BACKPACK && hover && m_dragSlot != idx)
                borderC = (Color){80, 255, 120, 255};
            if (m_selectedBpSlot == idx) {
                borderC = (Color){255, 255, 0, 255};
            }
            // Dim the source while it is being dragged.
            if (m_dragKind == DragKind::BACKPACK && m_dragSlot == idx)
                bgC = (Color){30, 30, 45, 255};

            DrawRectangle(cx, cy, cellSize, cellSize, bgC);
            DrawRectangleLines(cx, cy, cellSize, cellSize, borderC);
            // Subtle top highlight so a cell reads as a bevelled well rather
            // than a flat rectangle (same trick as the panel accent rule).
            DrawRectangle(cx + 1, cy + 1, cellSize - 2, 1, (Color){255, 255, 255, 18});

            if (hasItem) {
                const ItemDBEntry* def = GetItemDef(itemId);
                if (def) {
                    Texture2D* icon = nullptr;
                    const char* iconAlias = nullptr;
                    switch (def->category) {
                        case ItemCategory::HEALTH_VIAL:    iconAlias = "HealthVial"; break;
                        case ItemCategory::MANA_VIAL:      iconAlias = "ManaVial"; break;
                        case ItemCategory::ENERGY_CRYSTAL: iconAlias = "EnergyCrystal"; break;
                        case ItemCategory::KEY:            iconAlias = "Key"; break;
                        case ItemCategory::COIN:           iconAlias = "Coin"; break;
                        case ItemCategory::POWERUP:        iconAlias = "Powerup"; break;
                        default: break;
                    }
                    if (iconAlias) {
                        Texture2D t = AssetMapper::Instance().GetTexture(iconAlias);
                        if (t.id > 0) {
                            static Texture2D s_cachedIcon = t;
                            s_cachedIcon = t;
                            icon = &s_cachedIcon;
                        }
                    }
                    if (icon && icon->id > 0) {
                        float scale = (float)cellSize / (float)icon->width * 0.7f;
                        DrawTextureEx(*icon, (Vector2){(float)(cx + FS(6)), (float)(cy + FS(4))}, 0, scale, WHITE);
                    }
                    // Quantity text
                    if (qty > 1) {
                        const char* qt = TextFormat("%d", qty);
                        int qw = MeasureText(qt, FS(12));
                        DrawTextEx(GetFontDefault(), qt,
                                   {(float)(cx + cellSize - qw - FS(4)),
                                    (float)(cy + cellSize - FS(16))}, 12.0f * S, 1.0f, WHITE);
                    }
                }
            }
        }
    }

    // Keyboard navigation over the backpack (arrows move, Enter uses)
    {
        int sel = m_selectedBpSlot;
        int row = (sel >= 0) ? (sel / BACKPACK_COLS) : 0;
        int col = (sel >= 0) ? (sel % BACKPACK_COLS) : 0;
        bool moved = false;
        if (IsKeyPressed(KEY_RIGHT)) { col = (col + 1) % BACKPACK_COLS; moved = true; }
        if (IsKeyPressed(KEY_LEFT))  { col = (col + BACKPACK_COLS - 1) % BACKPACK_COLS; moved = true; }
        if (IsKeyPressed(KEY_DOWN))  { row = (row + 1) % BACKPACK_ROWS; moved = true; }
        if (IsKeyPressed(KEY_UP))    { row = (row + BACKPACK_ROWS - 1) % BACKPACK_ROWS; moved = true; }
        if (moved) m_selectedBpSlot = row * BACKPACK_COLS + col;
        if (IsKeyPressed(KEY_ENTER) && m_selectedBpSlot >= 0)
            UseBackpackItem(m_selectedBpSlot);
    }

    // Hover tooltip for backpack items
    {
        Vector2 mp = GetMousePosition();
        for (int index = 0; index < BACKPACK_SLOTS; index++) {
            int row = index / BACKPACK_COLS, col = index % BACKPACK_COLS;
            Rectangle r = {(float)(bx + col * (cellSize + cellGap)),
                           (float)(by + row * (cellSize + cellGap)),
                           (float)cellSize, (float)cellSize};
            if (!CheckCollisionPointRec(mp, r)) continue;
            int itemId = gInventory.backpack[index].itemId;
            if (itemId < 0) break;
            const ItemDBEntry* def = GetItemDef(itemId);
            if (!def) break;
            const int nameFs = FS(13), descFs = FS(11);
            int nameW = MeasureText(def->name, nameFs);
            int descW = MeasureText(def->description, descFs);
            int tw = (nameW > descW ? nameW : descW) + FS(16);
            int tipH = nameFs + descFs + FS(18);
            int tipX = (int)mp.x + FS(14), tipY = (int)mp.y + FS(14);
            if (tipX + tw > sw) tipX = sw - tw - FS(4);
            if (tipY + tipH > sh) tipY = sh - tipH - FS(4);
            DrawRectangle(tipX + FS(2), tipY + FS(2), tw, tipH, (Color){0, 0, 0, 120});
            DrawRectangle(tipX, tipY, tw, tipH, (Color){10, 10, 20, 240});
            DrawRectangleLines(tipX, tipY, tw, tipH, (Color){210, 180, 90, 255});
            DrawTextEx(GetFontDefault(), def->name, {(float)(tipX + FS(8)), (float)(tipY + FS(5))},
                       13.0f * S, 1.0f, WHITE);
            DrawTextEx(GetFontDefault(), def->description,
                       {(float)(tipX + FS(8)), (float)(tipY + FS(5) + nameFs + FS(4))},
                       11.0f * S, 1.0f, LIGHTGRAY);
            break;
        }
    }

    // === STATS (between equipment and backpack) ===
    int sx = px + FS(530);
    int sy = py + FS(50);
    DrawTextEx(GetFontDefault(), "STATS", {(float)sx, (float)(sy - FS(18))},
               12.0f * S, 1.0f, LIGHTGRAY);

    auto drawStatBar = [&](const char* label, float value, float maxv, Color fill,
                           Color well, int& outY) {
        const int barW = FS(150), barH = FS(10);
        float pct = maxv > 0.0f ? value / maxv : 0.0f;
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 1.0f) pct = 1.0f;
        DrawRectangle(sx - 1, outY - 1, barW + 2, barH + 2, (Color){10, 10, 10, 200});
        DrawRectangle(sx, outY, barW, barH, well);
        DrawRectangle(sx, outY, (int)(barW * pct), barH, fill);
        DrawRectangle(sx + 1, outY + 1, barW - 2, 1, (Color){255, 255, 255, 16});
        DrawTextEx(GetFontDefault(), label, {(float)(sx + 1), (float)(outY + barH + FS(2))},
                   12.0f * S, 1.0f, fill);
        outY += barH + FS(18);
    };

    sy += FS(4);
    DrawTextEx(GetFontDefault(), TextFormat("Level: %d", LightningEntityManager::Instance().GetPlayerLevel()),
               {(float)sx, (float)sy}, 14.0f * S, 1.0f, WHITE);
    sy += FS(22);
    {
        int xp = LightningEntityManager::Instance().GetPlayerXP();
        int xpN = LightningEntityManager::Instance().GetPlayerXPToNext();
        DrawTextEx(GetFontDefault(), TextFormat("XP: %d/%d", xp, xpN),
                   {(float)sx, (float)sy}, 14.0f * S, 1.0f, WHITE);
        sy += FS(22);
    }

    float hpInv = LightningEntityManager::Instance().GetPlayerHealth();
    float hpMaxInv = LightningEntityManager::Instance().GetPlayerMaxHealth();
    drawStatBar(TextFormat("HP: %.0f/%.0f", hpInv, hpMaxInv), hpInv, hpMaxInv,
                (Color){230, 40, 40, 255}, (Color){60, 12, 12, 235}, sy);

    float mpInv = LightningEntityManager::Instance().GetPlayerMana();
    float mpMaxInv = LightningEntityManager::Instance().GetPlayerMaxMana();
    drawStatBar(TextFormat("MP: %.0f/%.0f", mpInv, mpMaxInv), mpInv, mpMaxInv,
                (Color){70, 130, 255, 255}, (Color){12, 18, 60, 235}, sy);

    float peInv = LightningEntityManager::Instance().GetPlayerPsychicEnergy();
    float peMaxInv = LightningEntityManager::Instance().GetPlayerMaxPsychicEnergy();
    drawStatBar(TextFormat("PE: %.0f/%.0f", peInv, peMaxInv), peInv, peMaxInv,
                (Color){170, 90, 255, 255}, (Color){38, 12, 54, 235}, sy);

    // Selected item info
    if (m_selectedBpSlot >= 0) {
        int itemId = gInventory.backpack[m_selectedBpSlot].itemId;
        if (itemId >= 0) {
            const ItemDBEntry* def = GetItemDef(itemId);
            if (def) {
                DrawTextEx(GetFontDefault(), def->name, {(float)sx, (float)(sy + FS(14))},
                           14.0f * S, 1.0f, WHITE);
                DrawTextEx(GetFontDefault(), def->description, {(float)sx, (float)(sy + FS(32))},
                           12.0f * S, 1.0f, LIGHTGRAY);
            }
        }
    }

    // Hotbar preview at the bottom, drawn with the same authored bar art as the
    // in-world HUD (GameUI.ozls) so the two never drift apart.
    {
        const int   hy       = py + panel_h - FS(48);
        const float miniCell = 34.0f * S;
        const int   nSlots    = LightningEntityManager::HOTBAR_SIZE;

        SlotBarOptions opt;
        opt.firstSlot       = 0;
        opt.slotCount       = nSlots;
        opt.forcedWidth     = miniCell * (float)nSlots * 1.18f;
        opt.centerX         = (float)px + (float)FS(20) + opt.forcedWidth * 0.5f;
        opt.bottomY         = (float)(hy + FS(40));
        opt.drawSlotNumbers = true;
        opt.drawSlotName    = false;   // the panel already labels things
        // The mini bar is interactive here too: click selects, press-drag-release
        // reorders. The panel's own drag (backpack/equipment) is a separate state
        // resolved in the pre-pass above, so the two do not fight - a press can
        // only land on one kind of cell.
        opt.clickToSelect   = true;
        opt.dragToReorder   = true;

        int hover = -1;
        if (!DrawSlotBar(opt, hover)) {
            // No authored bar: fall back to plain cells (missing art / headless).
            int hx = px + FS(20);
            const int fcSize = FS(32);
            DrawTextEx(GetFontDefault(), "HOTBAR:", {(float)hx, (float)hy}, 12.0f * S, 1.0f, DARKGRAY);
            hx += FS(60);
            for (int i = 0; i < nSlots; i++) {
                Rectangle cell{(float)hx, (float)hy, (float)fcSize, (float)fcSize};
                if (CheckCollisionPointRec(GetMousePosition(), cell)) {
                    hover = i;
                    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                        lem.SelectSlot(i);
                        lem.HotbarBeginDrag(i);
                    }
                }
                int idx = lem.HotbarAt(i);
                bool hasItem = (idx >= 0 && lem.Get(idx) && lem.Get(idx)->def);
                Color c = hasItem ? WHITE : (Color){50, 50, 50, 255};
                DrawRectangle(hx, hy, fcSize, fcSize, (Color){20, 20, 30, 255});
                DrawRectangleLines(hx, hy, fcSize, fcSize, c);
                if (hasItem && lem.Get(idx)->iconIdx >= 0) {
                    Texture2D* iconTex = (Texture2D*)lem.GetIcon(lem.Get(idx)->iconIdx);
                    if (iconTex && iconTex->id > 0)
                        DrawTextureEx(*iconTex, (Vector2){(float)(hx + FS(4)), (float)(hy + FS(4))}, 0, S, WHITE);
                }
                hx += fcSize + FS(4);
            }
            if (lem.IsHotbarDragging()) lem.HotbarEndDragOnRelease(hover);
        }
        hoveredHotbar = hover;
    }

    // === Drop (rebindable; default G) ===
    // Drops whatever the cursor is over: a backpack item, an equipment piece, or
    // a hotbar cell. Suppressed while a drag is in flight so a drop cannot race
    // the drag resolution.
    //
    // This used to be Q, the SAME key as the spellbook toggle. Pressing it with
    // the inventory open dropped the item AND switched to the spellbook, which
    // closed the overlay - so "drop" looked like it dismissed the inventory.
    if (Keybinds::Pressed(GameAction::DropItem) && m_dragKind == DragKind::NONE) {
        if (hoveredBp >= 0)             DropBackpackItem(hoveredBp, cam);
        else if (hoveredEq >= 0)        DropEquipmentSlot(hoveredEq, cam);
        else if (hoveredHotbar >= 0)    DropHotbarSlot(hoveredHotbar, cam);
    }

    // === Drag ghost ===
    // The dragged piece follows the cursor, semi-transparent, so the player can
    // see what they are carrying and where it will land.
    if (m_dragKind != DragKind::NONE && m_dragSlot >= 0) {
        Vector2 mp = GetMousePosition();
        if (m_dragKind == DragKind::BACKPACK) {
            const int itemId = gInventory.backpack[m_dragSlot].itemId;
            const int qty = gInventory.backpack[m_dragSlot].quantity;
            const ItemDBEntry* def = GetItemDef(itemId);
            DrawRectangle(mp.x - 4, mp.y - 4, cellSize, cellSize, (Color){255, 255, 255, 60});
            DrawRectangleLinesEx({mp.x - 4, mp.y - 4, (float)cellSize, (float)cellSize}, 1,
                                 (Color){255, 255, 255, 200});
            if (def) {
                const char* iconAlias = nullptr;
                switch (def->category) {
                    case ItemCategory::HEALTH_VIAL:    iconAlias = "HealthVial"; break;
                    case ItemCategory::MANA_VIAL:      iconAlias = "ManaVial"; break;
                    case ItemCategory::ENERGY_CRYSTAL: iconAlias = "EnergyCrystal"; break;
                    case ItemCategory::KEY:            iconAlias = "Key"; break;
                    case ItemCategory::COIN:           iconAlias = "Coin"; break;
                    case ItemCategory::POWERUP:        iconAlias = "Powerup"; break;
                    default: break;
                }
                if (iconAlias) {
                    Texture2D t = AssetMapper::Instance().GetTexture(iconAlias);
                    if (t.id > 0) {
                        float scale = (float)cellSize / (float)t.width * 0.7f;
                        DrawTextureEx(t, {mp.x + 2, mp.y + 0}, 0, scale, (Color){255,255,255,200});
                    }
                }
                DrawTextEx(GetFontDefault(), def->name, {(float)((int)mp.x + FS(6)), (float)((int)mp.y - FS(16))},
                           12.0f * S, 1.0f, WHITE);
                if (qty > 1)
                    DrawTextEx(GetFontDefault(), TextFormat("%d", qty),
                               {(float)((int)mp.x + cellSize - FS(14)),
                                (float)((int)mp.y + cellSize - FS(16))}, 12.0f * S, 1.0f, WHITE);
            }
        } else {
            const int inst = lem.EquipmentAt(m_dragSlot);
            DrawRectangle(mp.x - 4, mp.y - 4, slotW / 2, slotH, (Color){255, 255, 255, 60});
            DrawRectangleLinesEx({mp.x - 4, mp.y - 4, (float)slotW / 2, (float)slotH}, 1,
                                 (Color){255, 255, 255, 200});
            if (inst >= 0 && lem.Get(inst) && lem.Get(inst)->def)
                DrawTextEx(GetFontDefault(), lem.Get(inst)->def->name.c_str(),
                           {(float)((int)mp.x + FS(6)), (float)((int)mp.y)}, 12.0f * S, 1.0f, WHITE);
        }
    }

    // Controls hint. The keys come from the same table the input reads, so a
    // rebind can never leave the overlay advertising the old key.
    {
        const std::string dropKey = Keybinds::KeyName(Keybinds::Key(GameAction::DropItem));
        const std::string useKey  = Keybinds::KeyName(Keybinds::Key(GameAction::Interact));
        const std::string skillKey = Keybinds::KeyName(Keybinds::Key(GameAction::Skills));
        const std::string consoleKey = Keybinds::KeyName(Keybinds::Key(GameAction::Console));
        DrawTextEx(GetFontDefault(),
                   TextFormat("TAB: close  |  Drag to reorder  |  Right-click use / unequip  |  %s: drop hovered  |  ENTER use  |  %s: pickup  |  %s: skills  |  %s: console",
                              dropKey.c_str(), useKey.c_str(), skillKey.c_str(), consoleKey.c_str()),
                   {(float)(px + FS(20)), (float)(py + panel_h - FS(22))}, 12.0f * S, 1.0f, DARKGRAY);
    }
}

// ---------------------------------------------------------------------------
// Network pickup hooks
// ---------------------------------------------------------------------------
void InventoryBehaviour::OnItemCollected(int item_id, int quantity) {
    const char* name = "unknown";
    const ItemDBEntry* def = GetItemDef(item_id);
    if (def) name = def->name;
    Emit(std::string(TextFormat("Collected: %s x%d", name, quantity)));

    if (!def) {
        if (item_id > 0) gInventory.AddToBackpack(item_id, quantity);
    } else if (def->category == ItemCategory::COIN) {
        gInventory.coins += def->value * quantity;
    } else if (!ApplyItemEffect(*def, quantity)) {
        // No instant effect (armor/ammo/powerup/keys): fall back to storage so
        // the grant is not silently dropped. Previously this branch also
        // swallowed psychic crystals, sending the energy to the backpack.
        if (!gInventory.AddToBackpack(item_id, quantity))
            OZ_WARN("Collect: backpack full, '%s' x%d lost", def->name, quantity);
    }

    // Mirror the single-player path so a networked collect flashes too: the
    // SP walk-over set this, the networked path never did.
    auto& fb = PawnSystem::Instance().m_pickupFeedback;
    fb.collected = false;   // the console line comes from Emit() above
    fb.typeName = def ? def->name : name;
    fb.itemId = item_id;
    fb.flashTimer = 0.5f;

    if (m_feedbackSink) m_feedbackSink();
}

void InventoryBehaviour::OnXpGain(int gained) {
    if (gained <= 0) return;
    m_xpFlashGain = gained;
    m_xpFlashTimer = 0.6f;
}

void InventoryBehaviour::ResetHudTracking() {
    m_lastXp = -1;
    m_lastLevel = -1;
    m_xpFlashTimer = 0.0f;
    m_xpFlashGain = 0;
    m_levelUpTimer = 0.0f;
    m_levelUpLevel = 0;
}

void InventoryBehaviour::OnLevelUp(int newLevel) {    m_levelUpLevel = newLevel;
    m_levelUpTimer = 2.5f;
    if (m_messageSink) {
        m_messageSink(TextFormat("LEVEL UP! You are now level %d", newLevel));
    } else {
        Emit(TextFormat("LEVEL UP! You are now level %d", newLevel));
    }
    if (m_feedbackSink) m_feedbackSink();
}

void InventoryBehaviour::OnWeaponCollected(const char* weapon_def_name) {
    auto& registry = LightningEntityRegistry::Instance();
    auto& lem = LightningEntityManager::Instance();
    const EntityDef* def = registry.Find(weapon_def_name);
    if (!def) {
        OZ_WARN("Weapon collect: unknown def '%s' — falling back to automag", weapon_def_name);
        def = registry.Find("automag");
        if (!def) return;
    }
    int instIdx = lem.Spawn(def->name);
    if (instIdx < 0) return;
    if (lem.HotbarPlaceFirstFree(instIdx)) {
        Emit(std::string("Picked up weapon: ") + def->name);
    } else {
        // Hotbar full: queue the def so it lands in the first free cell instead
        // of being despawned and lost. This is the same overflow the single-
        // player walk-over path uses (PawnSystem::UpdatePickups); the network
        // grant path was missing it, so a weapon collected over the wire with
        // every slot occupied vanished with only a warning.
        lem.Despawn(instIdx);
        lem.StashWeapon(def->name);
        Emit(std::string("Hotbar full — ") + def->name + " stored");
        OZ_WARN("Hotbar full — weapon %s stashed", def->name.c_str());
    }

    if (m_feedbackSink) m_feedbackSink();
}
