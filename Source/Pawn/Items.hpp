#ifndef OMEGA_ITEMS_HPP
#define OMEGA_ITEMS_HPP

#include <algorithm>
#include <cmath>
#include <string>
#include "raylib.h"
#include "../Script/LightningEntityManager.hpp"
#include "PickupItems.hpp"
#include "EquipSlots.hpp"

enum class ItemCategory {
    WEAPON = 0,
    HEALTH_VIAL,
    MANA_VIAL,
    ENERGY_CRYSTAL,
    KEY,
    COIN,
    POWERUP,
    ARMOR,
    JEWELRY,
    HELMET,
    BOOTS,
    LEGS,
    ACCESSORY
};

// EquipSlotType / EQUIP_SLOT_COUNT now live in EquipSlots.hpp, shared with
// LightningEntityManager (which owns the m_equipment[] array).
static constexpr int BACKPACK_COLS = 5;
static constexpr int BACKPACK_ROWS = 4;
static constexpr int BACKPACK_SLOTS = 20;
static constexpr int ITEM_DB_SIZE = 20;

struct BackpackSlot {
    int itemId;
    int quantity;
};

struct ItemDBEntry {
    int id;
    ItemCategory category;
    EquipSlotType equipSlot;
    const char* name;
    const char* iconPath;
    int value;
    const char* description;
    int maxStack;
};

// Ids come from item_id:: (Pawn/PickupItems.hpp), which is the same table the
// server uses to build net::PickupCollectedData::item_id. They used to be bare
// literals here with a comment demanding they match Source/Server/Server.cpp,
// which nothing enforced - retuning one side silently desynced what the server
// grants from what the client can render.
static const ItemDBEntry ItemDB[ITEM_DB_SIZE] = {
    {item_id::HEALTH_VIAL, ItemCategory::HEALTH_VIAL,    EquipSlotType::NONE, "Health Vial",          "GameData/Tex/Global/Items/HealthVial.png",     25,  "Restores 25 HP",                 10},
    {item_id::MANA_VIAL,   ItemCategory::MANA_VIAL,      EquipSlotType::NONE, "Mana Vial",            "GameData/Tex/Global/Items/ManaVial.png",       25,  "Restores 25 Mana",               10},
    {3, ItemCategory::ENERGY_CRYSTAL,  EquipSlotType::NONE, "Energy Crystal (111)", "GameData/Tex/Global/Items/EnergyCrystal.png",  111, "Grants 111 Psychic Energy",      5},
    {4, ItemCategory::ENERGY_CRYSTAL,  EquipSlotType::NONE, "Energy Crystal (222)", "GameData/Tex/Global/Items/EnergyCrystal.png",  222, "Grants 222 Psychic Energy",      5},
    {5, ItemCategory::ENERGY_CRYSTAL,  EquipSlotType::NONE, "Energy Crystal (333)", "GameData/Tex/Global/Items/EnergyCrystal.png",  333, "Grants 333 Psychic Energy",      5},
    {6, ItemCategory::ENERGY_CRYSTAL,  EquipSlotType::NONE, "Energy Crystal (444)", "GameData/Tex/Global/Items/EnergyCrystal.png",  444, "Grants 444 Psychic Energy",      5},
    {7, ItemCategory::ENERGY_CRYSTAL,  EquipSlotType::NONE, "Energy Crystal (555)", "GameData/Tex/Global/Items/EnergyCrystal.png",  555, "Grants 555 Psychic Energy",      5},
    {8, ItemCategory::ENERGY_CRYSTAL,  EquipSlotType::NONE, "Energy Crystal (666)", "GameData/Tex/Global/Items/EnergyCrystal.png",  666, "Grants 666 Psychic Energy",      5},
    {9, ItemCategory::ENERGY_CRYSTAL,  EquipSlotType::NONE, "Energy Crystal (777)", "GameData/Tex/Global/Items/EnergyCrystal.png",  777, "Grants 777 Psychic Energy",      5},
    {10,ItemCategory::ENERGY_CRYSTAL,  EquipSlotType::NONE, "Energy Crystal (888)", "GameData/Tex/Global/Items/EnergyCrystal.png",  888, "Grants 888 Psychic Energy",      5},
    {11,ItemCategory::ENERGY_CRYSTAL,  EquipSlotType::NONE, "Energy Crystal (999)", "GameData/Tex/Global/Items/EnergyCrystal.png",  999, "Grants 999 Psychic Energy",      5},
    {item_id::KEY,      ItemCategory::KEY,     EquipSlotType::NONE, "Key",       "GameData/Tex/Global/Items/Key.png",            1,   "Opens locked doors and chests",  20},
    {item_id::COIN,     ItemCategory::COIN,    EquipSlotType::NONE, "Coin",      "GameData/Tex/Global/Items/Coin.png",           1,   "Currency",                       99},
    {item_id::POWERUP,  ItemCategory::POWERUP, EquipSlotType::NONE, "Powerup",   "GameData/Tex/Global/Items/Powerup.png",        0,   "Mysterious power",               5},
    // Ammo previously shared item_id 14 with Powerup, so collecting an ammo box
    // granted a Powerup instead.
    {item_id::ARMOR,    ItemCategory::ARMOR,   EquipSlotType::NONE, "Armor Bundle", "GameData/Tex/Global/Items/Ammo_Box.png",      1,   "Restores 1 armor point",          5},
    {item_id::AMMO,     ItemCategory::POWERUP, EquipSlotType::NONE, "Ammo Box",    "GameData/Tex/Global/Items/Ammo_Box.png",     30,   "Refills the ammo pool",           5},
};

inline const ItemDBEntry* GetItemDef(int id) {
    for (int i = 0; i < ITEM_DB_SIZE; i++)
        if (ItemDB[i].name && ItemDB[i].id == id) return &ItemDB[i];
    return nullptr;
}

// ---------------------------------------------------------------------------
// Shared item-effect application.
//
// Three call sites used to switch on ItemCategory themselves: the walk-over
// pickup path, the backpack "use" path, and the networked collect reply. The
// third hardcoded item ids 13/1/2 and a flat +25 instead of reading def->value,
// so any retune of ItemDB silently disagreed between single- and multiplayer.
//
// Returns true when the item was consumed by its own effect. False means "this
// item has no instant effect" and the caller decides the fallback (stash it in
// the backpack, spawn an entity, and so on).
//
// Coins are deliberately NOT consumable: treating COIN as a usable effect let
// right-clicking a coin stack in the backpack mint a coin per click.
// ---------------------------------------------------------------------------
inline bool ApplyItemEffect(const ItemDBEntry& def, int quantity = 1) {
    auto& lem = LightningEntityManager::Instance();
    const float amount = (float)def.value * (float)(quantity > 0 ? quantity : 1);
    switch (def.category) {
        case ItemCategory::HEALTH_VIAL:
            lem.SetPlayerHealth(fminf(lem.GetPlayerMaxHealth(), lem.GetPlayerHealth() + amount));
            return true;
        case ItemCategory::MANA_VIAL:
            lem.SetPlayerMana(fminf(lem.GetPlayerMaxMana(), lem.GetPlayerMana() + amount));
            return true;
        case ItemCategory::ENERGY_CRYSTAL:
            lem.SetPlayerPsychicEnergy(fminf(lem.GetPlayerMaxPsychicEnergy(),
                                             lem.GetPlayerPsychicEnergy() + amount));
            return true;
        default:
            return false;
    }
}

// Category colour used by the on-collect screen flash and the HUD.
inline Color ItemFlashColor(const ItemDBEntry& def) {
    switch (def.category) {
        case ItemCategory::HEALTH_VIAL:    return {255, 50, 50, 255};
        case ItemCategory::MANA_VIAL:      return {50, 100, 255, 255};
        case ItemCategory::ENERGY_CRYSTAL: return {200, 50, 255, 255};
        case ItemCategory::COIN:           return {255, 215, 0, 255};
        case ItemCategory::ARMOR:          return {190, 190, 200, 255};
        default:                           return {255, 255, 255, 255};
    }
}

struct InventorySystem {
    BackpackSlot backpack[BACKPACK_SLOTS];
    int coins;

    InventorySystem() {
        for (auto& s : backpack)  { s.itemId = -1; s.quantity = 0; }
        coins = 0;
    }

    // Add item to first free backpack slot
    bool AddToBackpack(int itemId, int qty = 1) {
        const ItemDBEntry* def = GetItemDef(itemId);
        if (!def) return false;
        // Try to stack on existing
        for (auto& s : backpack) {
            if (s.itemId == itemId && s.quantity < def->maxStack) {
                int canAdd = def->maxStack - s.quantity;
                int add = (qty < canAdd) ? qty : canAdd;
                s.quantity += add;
                qty -= add;
                if (qty <= 0) return true;
            }
        }
        // Fill empty slots
        for (auto& s : backpack) {
            if (s.itemId == -1) {
                int add = (qty < def->maxStack) ? qty : def->maxStack;
                s.itemId = itemId;
                s.quantity = add;
                qty -= add;
                if (qty <= 0) return true;
            }
        }
        return qty <= 0;
    }

    // Remove one item from backpack
    bool RemoveFromBackpack(int slot) {
        if (slot < 0 || slot >= BACKPACK_SLOTS) return false;
        if (backpack[slot].itemId == -1) return false;
        if (--backpack[slot].quantity <= 0) {
            backpack[slot].itemId = -1;
            backpack[slot].quantity = 0;
        }
        return true;
    }

    // Swap two backpack slots wholesale (drag-and-drop reorder). Both indices
    // are validated; a no-op swap (same slot, or either out of range) is
    // rejected so a stray release cannot corrupt the array. Whole-slot swap
    // (not stack-merge) so dragging never silently changes quantities.
    bool SwapBackpackSlots(int a, int b) {
        if (a < 0 || a >= BACKPACK_SLOTS) return false;
        if (b < 0 || b >= BACKPACK_SLOTS) return false;
        if (a == b) return false;
        BackpackSlot tmp = backpack[a];
        backpack[a] = backpack[b];
        backpack[b] = tmp;
        return true;
    }

    // NOTE: equipment (armor/upgrade) is handled by the .ozls-driven
    // LightningEntityManager equipment slots (EquipmentAssign etc.), not here.

    // Summon a pickup item by name (for /summon command)
    int SummonItem(const char* name) {
        for (int i = 0; i < ITEM_DB_SIZE; i++) {
            const char* n = ItemDB[i].name;
            if (!n) continue; // uninitialized tail entries
            // Compare lowercase
            const char* a = name;
            const char* b = n;
            bool match = true;
            while (*a && *b) {
                char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
                char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
                if (ca != cb) { match = false; break; }
                a++; b++;
            }
            if (match && *a == *b) return ItemDB[i].id;
        }
        return -1;
    }
};

inline InventorySystem gInventory;

#endif
