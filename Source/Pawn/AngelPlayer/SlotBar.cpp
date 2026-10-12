#include "SlotBar.hpp"
#include "GameUi.hpp"
#include "../../Script/LightningEntityManager.hpp"

namespace {

// Deflate a rect by `amount` on every side (never past its centre).
Rectangle Inset(Rectangle r, float amount) {
    float d = amount * 2.0f;
    if (d >= r.width)  { r.width  = r.width * 0.5f;  r.x += r.width * 0.5f; }
    else               { r.x += amount; r.width -= d; }
    if (d >= r.height) { r.height = r.height * 0.5f; r.y += r.height * 0.5f; }
    else               { r.y += amount; r.height -= d; }
    return r;
}

// Scale `icon` to fit inside `box` preserving aspect ratio, centred.
void FitCentered(Texture2D icon, Rectangle box, Rectangle& src, Rectangle& dst) {
    src = {0.0f, 0.0f, (float)icon.width, (float)icon.height};
    if (icon.width <= 0 || icon.height <= 0 || box.width <= 0.0f || box.height <= 0.0f) {
        dst = {box.x, box.y, 0.0f, 0.0f};
        return;
    }
    float ta = (float)icon.width / (float)icon.height;
    float ba = box.width / box.height;
    float w, h;
    if (ta > ba) { w = box.width;  h = w / ta; }
    else         { h = box.height; w = h * ta; }
    dst = {box.x + (box.width - w) * 0.5f, box.y + (box.height - h) * 0.5f, w, h};
}

}  // namespace

// The ONE definition of the bar's screen rect. DrawSlotBar below positions from
// this, and SlotBarScreenRect publishes it so stacked HUD elements (the spell
// bar) do not re-derive it - see the note in SlotBar.hpp.
bool SlotBarScreenRect(const SlotBarOptions& opt, Rectangle& out) {
    GameUi& gui = GameUi::Instance();
    if (!gui.HasBar()) return false;

    Texture2D bar = gui.Texture();
    if (bar.id == 0 || bar.width <= 0 || bar.height <= 0) return false;

    const int sw = GetScreenWidth();
    const int sh = GetScreenHeight();
    if (sw <= 0 || sh <= 0) return false;

    float barW;
    if (opt.forcedWidth > 0.0f) {
        barW = opt.forcedWidth;
    } else {
        float pct = (opt.widthPct > 0.0f) ? opt.widthPct : gui.WidthPct();
        if (pct > 1.0f) pct = 1.0f;
        barW = (float)sw * pct;
    }
    if (barW > (float)sw - 8.0f) barW = (float)sw - 8.0f;
    if (barW < 1.0f) barW = 1.0f;

    const float scale = barW / (float)bar.width;
    const float barH  = (float)bar.height * scale;

    const float cx = (opt.centerX >= 0.0f) ? opt.centerX : (float)sw * 0.5f;
    const float by = (opt.bottomY >= 0.0f) ? opt.bottomY
                                           : (float)sh - gui.BottomMargin();

    out = {cx - barW * 0.5f, by - barH, barW, barH};
    return true;
}

bool DrawSlotBar(const SlotBarOptions& opt, int& hoverSlot) {
    hoverSlot = -1;

    GameUi& gui = GameUi::Instance();
    if (!gui.HasBar()) return false;

    Texture2D bar = gui.Texture();
    if (bar.id == 0 || bar.width <= 0 || bar.height <= 0) return false;

    const int sw = GetScreenWidth();
    const int sh = GetScreenHeight();
    if (sw <= 0 || sh <= 0) return false;

    auto& lem = LightningEntityManager::Instance();

    // --- Which cells to draw -------------------------------------------------
    const int cells = gui.SlotCount();
    if (cells <= 0) return false;

    int first = opt.firstSlot;
    if (first < 0) first = 0;
    int count = (opt.slotCount < 0) ? cells : opt.slotCount;
    if (first + count > cells) count = cells - first;
    if (first >= LightningEntityManager::HOTBAR_SIZE) count = 0;
    if (first + count > LightningEntityManager::HOTBAR_SIZE)
        count = LightningEntityManager::HOTBAR_SIZE - first;
    if (count <= 0) return true;   // valid bar, nothing in range

#ifdef OZ_SLOTBAR_PROBE
    {   // Opt-in test hook (never defined by the shipped build): fill empty
        // slots so a headless --shot-hud screenshot can prove the icon path.
        static const char* kProbe[] = {"pistol_01", "rifle_01", "etheral_waver",
                                       "Medkit", "Ammo", "Coin", "ManaTonic",
                                       "HealthVial"};
        for (int i = 0; i < count && i < 8; ++i) {
            int s = first + i;
            if (lem.HotbarAt(s) >= 0) continue;
            int inst = lem.Spawn(kProbe[i]);
            if (inst >= 0) lem.HotbarAssign(s, inst);
        }
    }
#endif

    // --- Layout --------------------------------------------------------------
    // Geometry comes from SlotBarScreenRect so there is exactly one copy of it.
    const Rectangle barRect = [&] {
        Rectangle r{};
        SlotBarScreenRect(opt, r);
        return r;
    }();
    const float barX = barRect.x;
    const float barY = barRect.y;
    const float barW = barRect.width;
    const float barH = barRect.height;
    const float cx   = barX + barW * 0.5f;
    const float scale = barW / (float)bar.width;

    // --- Atlas ---------------------------------------------------------------
    DrawTexturePro(bar,
                   {0.0f, 0.0f, (float)bar.width, (float)bar.height},
                   {barX, barY, barW, barH},
                   {0.0f, 0.0f}, 0.0f, WHITE);

    // --- Cells ---------------------------------------------------------------
    const float insetPx = gui.IconInset() * scale;
    const bool  wantNumbers = opt.drawSlotNumbers && gui.ShowSlotNumbers();
    const int   selected = lem.SelectedSlot();
    const Vector2 mouse = GetMousePosition();
    bool mouseConsumed = false;
    // Dragged cell is redrawn last so it paints over its neighbours.
    int dragFrom = opt.dragToReorder ? lem.HotbarDragFrom() : -1;

    for (int pass = 0; pass < 2; ++pass) {
    for (int i = 0; i < count; ++i) {
        const int slot = first + i;

        Rectangle cellSrc;
        if (!gui.SlotRect(slot, cellSrc)) continue;

        const Rectangle cellDst{barX + cellSrc.x * scale, barY + cellSrc.y * scale,
                                cellSrc.width * scale, cellSrc.height * scale};
        if (cellDst.width <= 0.0f || cellDst.height <= 0.0f) continue;

        const bool isSel  = (slot == selected);
        const bool isHover = CheckCollisionPointRec(mouse, cellDst);
        if (isHover) {
            hoverSlot = slot;
            if (opt.clickToSelect && !mouseConsumed && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                lem.SelectSlot(slot);
                mouseConsumed = true;
                if (opt.dragToReorder) lem.HotbarBeginDrag(slot);
            }
        }

        // Two passes: the dragged cell is skipped until the second one so it
        // draws on top of whatever it is dragged over.
        if ((pass == 1) != (dragFrom == slot)) continue;

        // Selection / hover outline sits just inside the art's own glowing
        // border, so the two read as one frame instead of fighting.
        if (isSel || isHover) {
            const Color c = isSel ? Color{250, 205, 95, 255} : Color{235, 245, 235, 200};
            DrawRectangleLinesEx(Inset(cellDst, 2.0f), 2.0f, c);
        }

        // Icon
        const int inst = lem.HotbarAt(slot);
        if (inst >= 0) {
            EntityInstance* e = lem.Get(inst);
            if (e && e->def && e->iconIdx >= 0) {
                if (Texture2D* icon = static_cast<Texture2D*>(lem.GetIcon(e->iconIdx))) {
                    if (icon->id > 0) {
                        Rectangle src, dst;
                        FitCentered(*icon, Inset(cellDst, insetPx), src, dst);
                        if (dst.width > 0.0f && dst.height > 0.0f)
                            DrawTexturePro(*icon, src, dst, {0.0f, 0.0f}, 0.0f, WHITE);
                    }
                }
            }
        }

        // Slot number
        if (wantNumbers) {
            char num[4] = {(char)('1' + (slot % 10)), '\0', '\0', '\0'};
            // Inset past the art's glowing border and the selection outline so
            // the digit never lands on top of either.
            DrawText(num, (int)cellDst.x + 8, (int)cellDst.y + 6, 13,
                     isSel ? WHITE : (Color){150, 200, 150, 220});
        }
    }
    }

#ifndef OMEGA_TEST_ENV
    // Complete the drag: release reorders, and releasing off the bar cancels.
    if (opt.dragToReorder) {
        if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) {
            lem.HotbarUpdateDrag(hoverSlot);
            lem.HotbarEndDrag();
        } else if (lem.IsHotbarDragging()) {
            lem.HotbarUpdateDrag(hoverSlot);
        }
    }
#endif

    // --- Selected slot name --------------------------------------------------
    if (opt.drawSlotName && gui.ShowSlotName()) {
        int inst = lem.HotbarAt(selected);
        EntityInstance* e = (inst >= 0) ? lem.Get(inst) : nullptr;
        if (e && e->def) {
            int w = MeasureText(e->def->name.c_str(), 14);
            DrawText(e->def->name.c_str(), (int)cx - w / 2, (int)barY - 20, 14,
                     (Color){225, 225, 235, 235});
        }
    }

    return true;
}
