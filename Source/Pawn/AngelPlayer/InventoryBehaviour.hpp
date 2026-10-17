#pragma once
#include "raylib.h"
#include <functional>
#include <string>

// InventoryBehaviour — HUD + inventory overlay + item/weapon pickup handling for
// the AngelPlayer. Part of the AngelPlayer player stack.
//
// Core.hpp / Data.hpp are single-translation-unit globals, so this behaviour
// receives its rendering context (camera / ticker / stance) as arguments and
// emits log + feedback through injected sinks instead of reaching into
// OmegaTechTextSystem / SoundManager directly. Item/inventory data comes
// from the shared Pawn/Items.hpp.
class InventoryBehaviour {
public:
    static InventoryBehaviour& Instance() {
        static InventoryBehaviour instance;
        return instance;
    }

    // Output sinks, wired by the host once at startup.
    void SetMessageSink(std::function<void(const std::string&)> sink) {
        m_messageSink = std::move(sink);
    }
    void SetFeedbackSink(std::function<void()> sink) {
        m_feedbackSink = std::move(sink);
    }

    // Set the OmegaClient instance for scoreboard rendering. Called once from
    // Main.cpp after the client is created; the client is a file-static there,
    // not a singleton.
    void SetClient(class OmegaClient* client) { m_client = client; }

    // Consume one backpack item and apply its effect. Returns true if used.
    bool UseBackpackItem(int slot);

    // Always-visible stat bars (draw inside the 2D pass).
    void DrawHud(const Camera3D& cam, int ticker, bool isCrouching, bool isSprinting);

    // Diablo-style inventory overlay (draw when the inventory is open). The
    // camera is needed to place a dropped item in front of the player.
    void DrawOverlay(const Camera3D& cam);

    // Drop actions (the overlay's Q key). Backpack items are removed and
    // respawned as a world pickup; hotbar/equipment entities are despawned and
    // respawned the same way. Return false when there is nothing to drop.
    bool DropBackpackItem(int slot, const Camera3D& cam);
    bool DropHotbarSlot(int slot, const Camera3D& cam);
    bool DropEquipmentSlot(int slot, const Camera3D& cam);

    // Network pickup hooks (delegated from OmegaClient callbacks).
    void OnItemCollected(int item_id, int quantity);
    void OnWeaponCollected(const char* weapon_def_name);

private:
    InventoryBehaviour() = default;

    void Emit(const std::string& msg);

    // Spawn a world pickup for a dropped item/weapon def in front of the player.
    void SpawnDroppedPickup(const char* typeName, const Camera3D& cam);

    class OmegaClient* m_client = nullptr;  // scoreboard rendering; not owned

    int m_selectedBpSlot = -1;
    // Drag-and-drop. Two sources share the drag: the backpack (item ids) and
    // the equipment rows (entity instances). They are separate namespaces, so a
    // drag may only be dropped on the SAME kind it started from - the drag kind
    // is carried in m_dragKind and an incompatible drop is ignored rather than
    // silently doing nothing.
    enum class DragKind { NONE, BACKPACK, EQUIPMENT };
    DragKind m_dragKind = DragKind::NONE;
    int  m_dragSlot = -1;        // source backpack cell or equipment slot
    std::function<void(const std::string&)> m_messageSink;
    std::function<void()> m_feedbackSink;
};
