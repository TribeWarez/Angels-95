#include "../../WindowsCompat.hpp"
#include "InventoryBehaviour.hpp"
#include "../Items.hpp"
#include "../OzPawnSystem.hpp"
#include "SlotBar.hpp"
#include "../../Script/LightningEntityManager.hpp"
#include "../../Script/LightningEntityRegistry.hpp"
#include "../../Renderer/OzAssetMapper.hpp"
#include "../../Client/Client.hpp"
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
// HUD: always-visible player stat bars
// ---------------------------------------------------------------------------
void InventoryBehaviour::DrawHud(const Camera3D& cam, int ticker, bool isCrouching, bool isSprinting) {
    const int sw = GetScreenWidth();
    const int sh = GetScreenHeight();
    const int bar_w = 220;
    const int bar_h = 14;
    const int pad = 6;
    int x = pad;
    int y = pad;

    // Level
    DrawText(TextFormat("Lv.%d", LightningEntityManager::Instance().GetPlayerLevel()), x, y, 14, WHITE);
    y += 18;

    // XP bar
    int xp_w = bar_w - 40;
    DrawText("XP", x, y, 12, LIGHTGRAY);
    int xpToNext = LightningEntityManager::Instance().GetPlayerXPToNext();
    if (xpToNext > 0) {
        int xp = LightningEntityManager::Instance().GetPlayerXP();
        float xp_pct = (float)xp / xpToNext;
        DrawRectangle(x + 30, y, xp_w, bar_h, (Color){30, 30, 30, 255});
        DrawRectangle(x + 30, y, (int)(xp_pct * xp_w), bar_h, SKYBLUE);
        DrawText(TextFormat("%d/%d", xp, xpToNext),
                 x + 34, y + 1, 10, WHITE);
    }
    y += bar_h + pad;

    // Health bar
    float hpMax = LightningEntityManager::Instance().GetPlayerMaxHealth();
    float hp = LightningEntityManager::Instance().GetPlayerHealth();
    float hp_pct = (hpMax > 0) ? (hp / hpMax) : 0;
    DrawText(TextFormat("HP %d/%d", (int)hp, (int)hpMax),
             x, y, 12, WHITE);
    DrawRectangle(x, y + 14, bar_w, bar_h, (Color){50, 10, 10, 255});
    DrawRectangle(x, y + 14, (int)(hp_pct * bar_w), bar_h, RED);
    y += 14 + bar_h + pad;

    // Mana bar
    float mpMax = LightningEntityManager::Instance().GetPlayerMaxMana();
    float mp = LightningEntityManager::Instance().GetPlayerMana();
    float mp_pct = (mpMax > 0) ? (mp / mpMax) : 0;
    DrawText(TextFormat("MP %d/%d", (int)mp, (int)mpMax),
             x, y, 12, WHITE);
    DrawRectangle(x, y + 14, bar_w, bar_h, (Color){10, 10, 50, 255});
    DrawRectangle(x, y + 14, (int)(mp_pct * bar_w), bar_h, BLUE);
    y += 14 + bar_h + pad;

    // Psychic Energy bar
    float peMax = LightningEntityManager::Instance().GetPlayerMaxPsychicEnergy();
    float pe = LightningEntityManager::Instance().GetPlayerPsychicEnergy();
    float pe_pct = (peMax > 0) ? (pe / peMax) : 0;
    DrawText(TextFormat("PE %d/%d", (int)pe, (int)peMax),
             x, y, 12, WHITE);
    DrawRectangle(x, y + 14, bar_w, bar_h, (Color){40, 10, 50, 255});
    DrawRectangle(x, y + 14, (int)(pe_pct * bar_w), bar_h, PURPLE);
    y += 14 + bar_h + pad;

    // Current selected item/weapon (from EntityManager hotbar)
    {
        auto& lem = LightningEntityManager::Instance();
        EntityInstance* selEnt = lem.SelectedEntity();
        int selSlot = lem.SelectedSlot() + 1;
        const char* label = "Empty";
        if (selEnt && selEnt->def)
            label = selEnt->def->name.c_str();
        DrawText(TextFormat("Slot %d: %s", selSlot, label),
                 x, y, 12, YELLOW);
        y += 16;
        // Ammo display for weapons
        if (selEnt && selEnt->def && selEnt->def->type == EntityType::WEAPON) {
            auto ait = selEnt->runtimeStats.find("ammo");
            auto mit = selEnt->runtimeStats.find("magazine");
            if (ait != selEnt->runtimeStats.end() && mit != selEnt->runtimeStats.end()) {
                Color ammoCol = (ait->second <= 0.0f) ? RED : WHITE;
                DrawText(TextFormat("Ammo: %.0f/%.0f", ait->second, mit->second),
                         x, y, 12, ammoCol);
                y += 16;
            }
        }
    }

    // Weapon fire indicator (brief pulse)
    static double lastFireTime = 0;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        lastFireTime = GetTime();
    }
    if (GetTime() - lastFireTime < 0.15) {
        DrawText("FIRE", x, y, 20, RED);
    }

    // Movement stance indicator (sprint / crouch)
    if (isCrouching)
        DrawText("CROUCH", pad, sh - 46, 16, (Color){120, 200, 255, 255});
    else if (isSprinting)
        DrawText("SPRINT", pad, sh - 46, 16, (Color){255, 210, 120, 255});

    // Scoreboard (top-right, below position/rotation)
    {
        if (!m_client) return;
        const auto& scores = m_client->scores();
        if (!scores.empty()) {
            int sbX = sw - 200;
            int sbY = 60;
            int rowH = 18;
            int sbH = (int)scores.size() * rowH + 8;
            DrawRectangle(sbX - 4, sbY - 4, 196, sbH, (Color){0, 0, 0, 160});
            int row = 0;
            for (const auto& [pid, ss] : scores) {
                DrawText(TextFormat("P%d  %d", pid, ss.score),
                         sbX, sbY + row * rowH, 14,
                         ss.team == 0 ? (Color){255, 200, 100, 255} : (Color){100, 200, 255, 255});
                row++;
            }
        }
    }

    // Match-over banner with fade (1.0s in, hold, 3.0s out)
    {
        if (!m_client) return;
        if (m_client->match_over()) {
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
                int w = MeasureText(banner, 28);
                DrawText(banner, (sw - w) / 2, sh / 2 - 40, 28,
                         (Color){255, 220, 100, (unsigned char)(alpha * 255)});
            }
        }
    }

    // Coordinates (top-right)
    float yaw = -atan2f(cam.target.x - cam.position.x, cam.target.z - cam.position.z) * RAD2DEG;
    float pitch = asinf((cam.target.y - cam.position.y) /
        Vector3Distance(cam.position, cam.target)) * RAD2DEG;
    int rx = sw - 280;
    DrawText(TextFormat("Pos: %.1f %.1f %.1f", cam.position.x, cam.position.y, cam.position.z),
             rx, 10, 14, LIGHTGRAY);
    DrawText(TextFormat("Rot: %.0f %.0f", yaw, pitch),
             rx, 28, 14, LIGHTGRAY);
    if ((ticker % 60) == 0) {
        fprintf(stderr, "POS: %.1f %.1f %.1f  ROT: %.0f %.0f\n",
                cam.position.x, cam.position.y, cam.position.z, yaw, pitch);
    }
}

// ---------------------------------------------------------------------------
// Inventory Overlay (Diablo I style)
// ---------------------------------------------------------------------------
void InventoryBehaviour::DrawOverlay() {
    const int sw = GetScreenWidth();
    const int sh = GetScreenHeight();
    const int panel_w = 720;
    const int panel_h = 520;
    const int px = (sw - panel_w) / 2;
    const int py = (sh - panel_h) / 2;

    // Layout constants shared by the draw pass and the drag hit-test below.
    const int ex = px + 20;          // equipment column x
    const int slotH = 38, slotW = 150;
    const int bx = px + 190;         // backpack grid x
    const int by = py + 50;
    const int cellSize = 52, cellGap = 4;

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

    DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 160});
    DrawRectangle(px, py, panel_w, panel_h, (Color){25, 25, 35, 245});
    DrawRectangleLines(px, py, panel_w, panel_h, (Color){180, 180, 200, 255});

    DrawText("INVENTORY", px + 15, py + 10, 22, WHITE);
    DrawText(TextFormat("Coins: %d", gInventory.coins), px + panel_w - 150, py + 14, 16, GOLD);

    int ey = py + 50;

    DrawText("EQUIPMENT", ex, ey - 18, 12, LIGHTGRAY);

    // Labels now live in Pawn/EquipSlots.hpp, sized by EQUIP_SLOT_COUNT, so a new
    // equipment slot without a label is a compile error instead of a blank row.

    for (int i = 0; i < EQUIP_SLOT_COUNT; i++) {
        int idx = lem.EquipmentAt(i);
        bool owned = (idx >= 0 && lem.Get(idx) && lem.Get(idx)->owned);
        Color c = owned ? WHITE : (Color){80, 80, 80, 255};
        Color bg = owned ? (Color){40, 50, 45, 255} : (Color){20, 25, 20, 255};

        Rectangle eqRect = {(float)ex, (float)ey, (float)slotW, (float)slotH};
        bool eqHover = CheckCollisionPointRec(GetMousePosition(), eqRect);
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
        DrawText(kEquipSlotLabels[i], ex + 6, ey + 12, 12, c);

        if (owned && lem.Get(idx)->iconIdx >= 0) {
            Texture2D* iconTex = (Texture2D*)lem.GetIcon(lem.Get(idx)->iconIdx);
            if (iconTex && iconTex->id > 0)
                DrawTextureEx(*iconTex, (Vector2){(float)ex + slotW - 34, (float)ey + 2}, 0, 1.5f, WHITE);
        }
        ey += slotH + 4;
    }

    // === BACKPACK (right side) ===
    // bx/by/cellSize/cellGap are declared once at the top of DrawOverlay so the
    // drag hit-test and this draw pass agree; redeclaring them here used to be
    // the pattern and is what made the two passes drift.
    DrawText("BACKPACK", bx, by - 18, 12, LIGHTGRAY);

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
                        DrawTextureEx(*icon, (Vector2){(float)cx + 6, (float)cy + 4}, 0, scale, WHITE);
                    }
                    // Quantity text
                    if (qty > 1) {
                        DrawText(TextFormat("%d", qty), cx + cellSize - 20, cy + cellSize - 16, 12, WHITE);
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
            int nameW = MeasureText(def->name, 13);
            int descW = MeasureText(def->description, 11);
            int tw = (nameW > descW ? nameW : descW) + 16;
            int tipX = (int)mp.x + 14, tipY = (int)mp.y + 14;
            if (tipX + tw > sw) tipX = sw - tw - 4;
            if (tipY + 42 > sh) tipY = sh - 46;
            DrawRectangle(tipX, tipY, tw, 42, (Color){10, 10, 20, 240});
            DrawRectangleLines(tipX, tipY, tw, 42, (Color){210, 180, 90, 255});
            DrawText(def->name, tipX + 8, tipY + 5, 13, WHITE);
            DrawText(def->description, tipX + 8, tipY + 23, 11, LIGHTGRAY);
            break;
        }
    }

    // === STATS (between equipment and backpack) ===
    int sx = px + 530;
    int sy = py + 50;
    DrawText("STATS", sx, sy - 18, 12, LIGHTGRAY);

    sy += 4;
    DrawText(TextFormat("Level: %d", LightningEntityManager::Instance().GetPlayerLevel()), sx, sy, 14, WHITE); sy += 22;
    DrawText(TextFormat("XP: %d/%d", LightningEntityManager::Instance().GetPlayerXP(), LightningEntityManager::Instance().GetPlayerXPToNext()), sx, sy, 14, WHITE); sy += 22;

    float hpInv = LightningEntityManager::Instance().GetPlayerHealth();
    float hpMaxInv = LightningEntityManager::Instance().GetPlayerMaxHealth();
    DrawRectangle(sx, sy, 150, 10, (Color){50, 0, 0, 255});
    float hpPct = hpMaxInv > 0 ? hpInv / hpMaxInv : 0;
    DrawRectangle(sx, sy, (int)(150 * hpPct), 10, RED);
    DrawText(TextFormat("HP: %.0f/%.0f", hpInv, hpMaxInv), sx + 1, sy + 12, 12, RED); sy += 28;

    float mpInv = LightningEntityManager::Instance().GetPlayerMana();
    float mpMaxInv = LightningEntityManager::Instance().GetPlayerMaxMana();
    DrawRectangle(sx, sy, 150, 10, (Color){0, 0, 50, 255});
    float mpPct = mpMaxInv > 0 ? mpInv / mpMaxInv : 0;
    DrawRectangle(sx, sy, (int)(150 * mpPct), 10, BLUE);
    DrawText(TextFormat("MP: %.0f/%.0f", mpInv, mpMaxInv), sx + 1, sy + 12, 12, BLUE); sy += 28;

    float peInv = LightningEntityManager::Instance().GetPlayerPsychicEnergy();
    float peMaxInv = LightningEntityManager::Instance().GetPlayerMaxPsychicEnergy();
    DrawRectangle(sx, sy, 150, 10, (Color){30, 0, 30, 255});
    float pePct = peMaxInv > 0 ? peInv / peMaxInv : 0;
    DrawRectangle(sx, sy, (int)(150 * pePct), 10, PURPLE);
    DrawText(TextFormat("PE: %.0f/%.0f", peInv, peMaxInv), sx + 1, sy + 12, 12, PURPLE);

    // Selected item info
    if (m_selectedBpSlot >= 0) {
        int itemId = gInventory.backpack[m_selectedBpSlot].itemId;
        if (itemId >= 0) {
            const ItemDBEntry* def = GetItemDef(itemId);
            if (def) {
                DrawText(def->name, sx, sy + 40, 14, WHITE);
                DrawText(def->description, sx, sy + 58, 12, LIGHTGRAY);
            }
        }
    }

    // Hotbar preview at the bottom, drawn with the same authored bar art as the
    // in-world HUD (GameUI.ozls) so the two never drift apart.
    {
        const int  hy        = py + panel_h - 48;
        const float miniCell = 34.0f;
        const int  nSlots    = LightningEntityManager::HOTBAR_SIZE;

        SlotBarOptions opt;
        opt.firstSlot       = 0;
        opt.slotCount       = nSlots;
        opt.forcedWidth     = miniCell * (float)nSlots * 1.18f;
        opt.centerX         = (float)px + 20.0f + opt.forcedWidth * 0.5f;
        opt.bottomY         = (float)(hy + 40);
        opt.drawSlotNumbers = true;
        opt.drawSlotName    = false;   // the panel already labels things
        opt.clickToSelect   = false;   // the panel owns its own clicks

        int hover = -1;
        if (!DrawSlotBar(opt, hover)) {
            // No authored bar: fall back to plain cells (missing art / headless).
            int hx = px + 20;
            DrawText("HOTBAR:", hx, hy, 12, DARKGRAY);
            hx += 60;
            for (int i = 0; i < nSlots; i++) {
                int idx = lem.HotbarAt(i);
                bool hasItem = (idx >= 0 && lem.Get(idx) && lem.Get(idx)->def);
                Color c = hasItem ? WHITE : (Color){50, 50, 50, 255};
                DrawRectangle(hx, hy, 32, 32, (Color){20, 20, 30, 255});
                DrawRectangleLines(hx, hy, 32, 32, c);
                if (hasItem && lem.Get(idx)->iconIdx >= 0) {
                    Texture2D* iconTex = (Texture2D*)lem.GetIcon(lem.Get(idx)->iconIdx);
                    if (iconTex && iconTex->id > 0)
                        DrawTextureEx(*iconTex, (Vector2){(float)hx + 4, (float)hy + 4}, 0, 1.0f, WHITE);
                }
                hx += 36;
            }
        }
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
                DrawText(def->name, (int)mp.x + 6, (int)mp.y - 16, 12, WHITE);
                if (qty > 1)
                    DrawText(TextFormat("%d", qty), (int)mp.x + cellSize - 14,
                             (int)mp.y + cellSize - 16, 12, WHITE);
            }
        } else {
            const int inst = lem.EquipmentAt(m_dragSlot);
            DrawRectangle(mp.x - 4, mp.y - 4, slotW / 2, slotH, (Color){255, 255, 255, 60});
            DrawRectangleLinesEx({mp.x - 4, mp.y - 4, (float)slotW / 2, (float)slotH}, 1,
                                 (Color){255, 255, 255, 200});
            if (inst >= 0 && lem.Get(inst) && lem.Get(inst)->def)
                DrawText(lem.Get(inst)->def->name.c_str(), (int)mp.x + 6, (int)mp.y, 12, WHITE);
        }
    }

    // Controls hint
    DrawText("TAB: close  |  Drag to reorder / move equipment  |  Right-click use / unequip  |  ENTER use  |  E: pickup  |  K: skills  |  ^: console",
             px + 20, py + panel_h - 22, 12, DARKGRAY);
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
    if (lem.HotbarPlaceFirstFree(instIdx)) Emit(std::string("Picked up weapon: ") + def->name);
    else { OZ_WARN("Hotbar full — weapon %s lost", def->name.c_str()); lem.Despawn(instIdx); }

    if (m_feedbackSink) m_feedbackSink();
}
