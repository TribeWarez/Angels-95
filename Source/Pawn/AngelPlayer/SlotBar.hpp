#pragma once
#include "raylib.h"

// SlotBar — shared HUD renderer for the GameUI-declared object/slot bar.
//
// The art is a single atlas texture plus a per-cell rect list, both authored in
// `GameData/Tex/Engine/GameUI.ozls` (see the GameUi bridge). This maps the atlas
// to the screen, draws it once, then fills each occupied cell with the owning
// entity's icon, and handles hover/click-to-select.
//
// It lives outside LightningEntityManager so the in-world hotbar
// (LightningEntityManager::DrawHotbar) and the inventory overlay's mini bar
// (InventoryBehaviour::DrawOverlay) render identically from one code path.
struct SlotBarOptions {
    // Negative means "use the value from GameUI.ozls" (or the screen default).
    float centerX    = -1.0f;  // <0 = horizontal centre of the screen
    float bottomY    = -1.0f;  // <0 = hud_bottom_margin above the screen bottom
    float widthPct   = -1.0f;  // <0 = hud_width_pct
    float forcedWidth = -1.0f; // >0 = absolute pixel width, overrides widthPct

    // Draw cells [firstSlot, firstSlot + slotCount). slotCount < 0 means "as
    // many cells as the atlas declares", still clamped to the hotbar capacity.
    int   firstSlot  = 0;
    int   slotCount  = -1;

    bool drawSlotNumbers = true;  // "N" in each cell's top-left corner
    bool drawSlotName    = true;  // selected slot's entity name under the bar
    bool clickToSelect   = true;  // left-click a cell to select that slot

    // Allow press-drag-release to reorder the hotbar (LightningEntityManager's
    // HotbarSwap, which previously had no caller at all). Only the in-world bar
    // enables this: the inventory overlay's mini bar reorders the same array
    // underneath the player's cursor, which would fight the backpack drag.
    bool dragToReorder   = false;
};

// Draws the bar. Returns false when GameUI declares no usable bar (missing
// texture or no slot_rects) so callers can fall back to their own chrome.
// `hoverSlot` receives the hovered slot index, or -1.
bool DrawSlotBar(const SlotBarOptions& opt, int& hoverSlot);

// The screen-space rectangle the bar occupies for `opt`, using the SAME layout
// DrawSlotBar uses. Returns false when there is no usable bar.
//
// Exposed so HUD elements that STACK above the bar (the spell bar) can position
// themselves against it without re-deriving the layout. A second copy of the
// formula would drift the moment hud_width_pct, hud_bottom_margin or the atlas
// changes - and the drift is screen-size dependent, so it would not show up on
// the developer's own resolution.
bool SlotBarScreenRect(const SlotBarOptions& opt, Rectangle& out);
