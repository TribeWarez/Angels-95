#pragma once
#include "LightningEntityDef.hpp"
#include "../Pawn/EquipSlots.hpp"
#include "LightningScriptContext.hpp"
#include "raylib.h"
#include "raymath.h"   // Vector3Normalize in the stamina helpers
#include <string>
#include <vector>
#include <unordered_map>

// A runtime instance of an entity definition
struct EntityInstance {
    const EntityDef* def = nullptr;
    LightningScriptContext ctx;
    int modelIdx = -1;       // slot in manager's model cache
    int textureIdx = -1;
    int iconIdx = -1;
    bool owned = false;
    int variantIndex = 0;
    std::unordered_map<std::string, float> runtimeStats;
    float cooldownRemaining = 0.0f;
};

// LightningEntityManager — runtime entity instances + dynamic hotbar + equipment
// Replaces Objects.hpp with a fully dynamic 512-entity / 8-slot system
class LightningEntityManager {
public:
    static constexpr int MAX_ENTITIES = 512;
static constexpr int HOTBAR_SIZE = 8;
// EQUIP_SLOT_COUNT lives in Pawn/EquipSlots.hpp (derived from EquipSlotType).
// It used to be declared here AND in Pawn/Items.hpp as two independent literal
// 8s, so changing the equipment row meant hunting for both.

    static LightningEntityManager& Instance() {
        static LightningEntityManager instance;
        return instance;
    }

    void Init();
    void Update(float dt);

    // --- Lifecycle ---
    int Spawn(const std::string& defName);
    int Spawn(const EntityDef* def);
    void Despawn(int index);
    EntityInstance* Get(int index);
    int Count() const { return (int)m_instances.size(); }

    // --- Hotbar ---
    void HotbarAssign(int slot, int instanceIndex);
    void HotbarSwap(int slotA, int slotB);

    // Drag reorder. HotbarSwap previously had no caller outside the tests, so
    // the hotbar could not be rearranged at all. The renderer that knows the
    // slot rectangles drives these: BeginDrag on press, UpdateDrag with the
    // hovered slot each frame, EndDrag on release.
    void HotbarBeginDrag(int slot);
    void HotbarUpdateDrag(int hoveredSlot);
    void HotbarEndDrag();
    // Complete a pending drag against a hovered slot, handling the release edge.
    // Called once per frame by the fallback bar; returns true when a drag was
    // pending. No-op in test builds, where there is no mouse.
    bool HotbarEndDragOnRelease(int hoveredSlot);
    bool IsHotbarDragging() const;
    int  HotbarDragFrom() const;
    int  HotbarDragTo() const;   // -1 when not over a slot
    int  HotbarAt(int slot) const;
    // First slot holding no instance, or -1 when the bar is full. Single source
    // of truth for "where does a newly collected item go" — the walk-over, the
    // collect reply and the /summon console command each had their own copy,
    // and only one of them reported a full bar.
    int  HotbarFirstFreeSlot() const;
    void SelectSlot(int slot);
    // Move `dir` slots from `from`, skipping empty ones and wrapping. If every
    // slot is empty the selection is left where it was.
    void SelectSlotSkippingEmpty(int from, int dir);
    // Place `instanceIndex` in the first free slot. Returns false (leaving the
    // caller to despawn or refund) when the bar is full.
    bool HotbarPlaceFirstFree(int instanceIndex);
    int  SelectedSlot() const { return m_selectedSlot; }
    EntityInstance* SelectedEntity() const;

    // --- Equipment slots ---
    int  EquipmentAt(int slot) const;           // returns instance index or -1
    void EquipmentAssign(int slot, int instanceIndex);
    void EquipmentUnequip(int slot);            // despawns equipped instance
    // Swap two equipment slots (drag-and-drop between rows). Fires
    // on_unequip/on_equip once per affected instance and does NOT despawn the
    // occupants, so neither piece is lost. Out-of-range or same-slot is a no-op.
    void EquipmentSwap(int a, int b);
    void EquipmentClear();                      // despawns all equipped
    int  EquipmentFindFreeSlot() const;         // first empty slot, or -1

    // Map an authored `equip_slot` string (e.g. "helmet") to a slot index, or -1.
    static int EquipmentSlotFromName(const std::string& name);
    // Spawn an armor/upgrade def and assign it to its authored equip_slot.
    // Returns false when the def has no usable equip_slot.
    bool AutoEquip(const EntityDef* def);
    // Summed `defense` of all equipped items (incoming-damage mitigation).
    float GetPlayerDefense() const;

    // Returns the texture/icon pointer from the cache by index. Models go
    // through GetModelByResourceIdx, which returns a typed pointer.
    void* GetTexture(int idx) const;
    void* GetIcon(int idx) const;

    // Pre-load a model for a named entity def and return resource slot index (or -1)
    int PrecacheModelForDef(const std::string& defName);
    // Get model by pre-cached index
    Model* GetModelByResourceIdx(int idx) const;

    // --- Input + rendering ---
    // Hotbar input: number keys 1-8 plus mouse-wheel cycling.
    // `uiBlocking` is the host's "a modal is open" flag (inventory, skill tree,
    // console, screenshot mode). Without it these keys fired underneath menus.
    void HandleInput(bool uiBlocking = false);
    void DrawHotbar();

    // --- Projectile spawning (for weapon entities) ---
    int FireSelectedWeapon(const Vector3& origin, const Vector3& direction);
    bool ReloadSelectedWeapon();

    // Play a sound stat authored on an entity def (hit_sound, reload_sound,
    // equip_sound). Volume/pitch default to unity. No-ops when the key is
    // absent, so an unauthored event is silent rather than borrowing a global
    // default clip. Paths resolve def-relative, like projectile_mesh.
    void PlayDefStatSound(EntityInstance* ent, const char* pathKey,
                          const char* volKey, const char* pitchKey);

    // Melee support.
    //
    // Report a resolved melee hit to the server, which re-validates reach and
    // applies the damage. Only used for networkControlled pawns; local pawns are
    // damaged directly by FireSelectedWeapon.
    void ReportMeleeHit(const struct Pawn& target, int damage, float reach,
                        float staminaCost, Vector3 origin, Vector3 dir);

    // True when solid world geometry lies between the two points. Uses the same
    // SegmentVsAABB the projectile impact path uses, so melee and bullets agree
    // on what counts as a wall. A segment starting inside a volume reports no
    // hit, so standing in a doorway still lets the swing land.
    static bool MeleeBlockedByGeometry(Vector3 from, Vector3 to);

    // Ammo-change hook (fire/reload) for network sync; action: 0=fire, 1=reload.
    // Signatures: (hotbarSlot, ammoInMag, magazineSize, action)
    void set_on_ammo_changed(std::function<void(int, int, int, int)> cb) { m_on_ammo_changed = std::move(cb); }

    // Melee hit sink. Set by the client so the manager can report a resolved
    // hit without depending on the networking layer (which cannot see Main.cpp's
    // file-static OmegaClient, and which the test builds do not link).
    using MeleeHitFn = std::function<void(int, int, int, int, float, float,
                                          float, float, float, float, float, float)>;
    void set_on_melee_hit(MeleeHitFn cb) { m_on_melee_hit = std::move(cb); }

    // True when the last ReloadSelectedWeapon / auto-reload actually started a
    // reload. Set by the manager (both reload paths), consumed by the player
    // layer to trigger the view-model reload clip. Reading clears the flag.
    // Previously the clip was triggered from a raw KEY_R poll in the draw
    // loop, so it played even when the reload was rejected (full magazine,
    // no magazine stat, wrong weapon type).
    bool ConsumeReloadStarted() {
        bool r = m_reloadStarted;
        m_reloadStarted = false;
        return r;
    }

    // Weapon stash. When the player walks over a weapon def and the hotbar is
    // full, the pickup is queued by def name rather than silently dropped
    // (Despawn). FlushNextStashedWeapon pulls the first queued def into the
    // first free hotbar slot and returns true on success. StashedWeaponCount
    // is surfaced in the HUD so the player knows something is pending.
    int  StashedWeaponCount() const { return (int)m_stashedWeapons.size(); }
    void StashWeapon(const std::string& defName);
    bool FlushNextStashedWeapon();
    const std::vector<std::string>& StashedWeapons() const { return m_stashedWeapons; }

    // --- Zone actions (called by PawnSystem on zone enter/exit) ---
    void TriggerZoneAction(const std::string& zoneName, const std::string& actionName);
    void TriggerZoneAction(const EntityDef* def, const std::string& actionName);
    // Applies a skyzone def's declarative fog_color / ambient_light /
    // fog_density / music. These keys parsed but nothing read them, so authored
    // values were silently ignored.
    void ApplyZoneEnvFields(const EntityDef& def);

    // --- Generic entity action (not SKYZONE-restricted; used for on_collect) ---
    void TriggerEntityAction(const EntityDef* def, const std::string& actionName);

    // --- Pickup collect hook (called by PawnSystem::UpdatePickups) ---
    void TriggerCollectAction(const std::string& defName);

    // --- Execute a named action label on an instance ---
    void RunAction(EntityInstance* inst, const std::string& actionName);

    // --- Player entity ---
    bool HasPlayerEntity() const { return m_playerEntityIndex >= 0; }
    int  PlayerEntityIndex() const { return m_playerEntityIndex; }

    // Player stat accessors (read/write to player entity runtimeStats)
    float GetPlayerStat(const std::string& name, float defaultVal = 0.0f) const;
    void  SetPlayerStat(const std::string& name, float val);
    float GetPlayerHealth() const;
    float GetPlayerMaxHealth() const;
    void  SetPlayerHealth(float v);
    float GetPlayerMana() const;

    // Stamina: the melee swing budget. Backed by the "stamina" stat, spent by
    // a weapon's `stamina_cost` and regenerated by UpdateStamina(). Both melee
    // defs author stamina_cost (etheral_waver 18, selenite_blade 15) and it had
    // zero code references, so a heavy weapon swung infinitely fast.
    float GetPlayerStamina() const;
    float GetPlayerMaxStamina() const;
    void  SetPlayerStamina(float v);
    // Deduct `cost`, clamped at 0. Returns false when the pool cannot cover it,
    // in which case nothing is spent and the caller should refuse the swing.
    bool  SpendStamina(float cost);
    bool  HasStamina(float cost) const;
    // Regenerate toward max. `dt` in seconds.
    void  UpdateStamina(float dt);
    float GetPlayerMaxMana() const;
    void  SetPlayerMana(float v);
    float GetPlayerPsychicEnergy() const;
    float GetPlayerMaxPsychicEnergy() const;
    void  SetPlayerPsychicEnergy(float v);
    int   GetPlayerLevel() const;
    void  SetPlayerLevel(int v);
    int   GetPlayerXP() const;
    void  SetPlayerXP(int v);
    int   GetPlayerXPToNext() const;
    void  SetPlayerXPToNext(int v);
    // Authored walk-speed multiplier from the Player entity def (`movement_speed`).
    float GetPlayerMovementSpeed() const;

    // --- Skills (ethereal / angelic tree) ---
    bool IsSkillUnlocked(const std::string& name) const;
    void UnlockSkill(const std::string& name);
    const std::vector<std::string>& UnlockedSkills() const { return m_unlockedSkills; }
    // Refund every unlocked node (cost back to mana/psychic, bonuses removed)
    // and clear the tree so it can be re-specced.
    void RespecSkills();

    // --- Script result routing (read by host after Update) ---
    const std::string& PendingMessage() const { return m_pendingMessage; }
    void ClearPendingMessage() { m_pendingMessage.clear(); }
    bool PlayerHurt() const { return m_playerHurt; }
    void ClearPlayerHurt() { m_playerHurt = false; }

    // External stat provider for script $name tokens (player + selected weapon stats)
    float ResolveScriptStat(const std::string& name) const;
    void ApplyPlayerStatOps(const std::vector<LightningScriptContext::PlayerStatOp>& ops);

    // --- Serialization (for save/load) ---
    std::string SerializeState() const;  // compact string of hotbar + equipment state
    bool DeserializeState(const std::string& data);  // restore from SerializeState()

    // --- Script side-effect query (called by host after Update) ---
    bool HasPendingFog() const { return m_pendingFog; }
    float PendingFogR() const { return m_fogR; }
    float PendingFogG() const { return m_fogG; }
    float PendingFogB() const { return m_fogB; }
    float PendingFogDensity() const { return m_fogDensity; }
    void ClearPendingFog() { m_pendingFog = false; }

    bool HasPendingSkybox() const { return !m_pendingSkybox.empty(); }
    const std::string& PendingSkybox() const { return m_pendingSkybox; }
    void ClearPendingSkybox() { m_pendingSkybox.clear(); }

    bool HasPendingAmbient() const { return m_pendingAmbient; }
    float PendingAmbientR() const { return m_ambientR; }
    float PendingAmbientG() const { return m_ambientG; }
    float PendingAmbientB() const { return m_ambientB; }
    void ClearPendingAmbient() { m_pendingAmbient = false; }

    // restore_fog / restore_ambient / restore_skybox: the host must revert the
    // corresponding override. All three opcodes used to clear the very value the
    // host reads (or blank a name it rejects), so they did nothing at all.
    bool HasPendingRestore() const {
        return m_pendingFogRestore || m_pendingAmbientRestore || m_pendingSkyboxRestore;
    }
    bool TakePendingFogRestore() { bool v = m_pendingFogRestore; m_pendingFogRestore = false; return v; }
    bool TakePendingAmbientRestore() { bool v = m_pendingAmbientRestore; m_pendingAmbientRestore = false; return v; }
    bool TakePendingSkyboxRestore() { bool v = m_pendingSkyboxRestore; m_pendingSkyboxRestore = false; return v; }

    // Zone music authored via `music = "..."` on a skyzone def.
    bool HasPendingMusic() const { return !m_pendingMusic.empty(); }
    const std::string& PendingMusic() const { return m_pendingMusic; }
    void ClearPendingMusic() { m_pendingMusic.clear(); }

private:
    LightningEntityManager();
    std::vector<EntityInstance> m_instances;
    int m_hotbar[HOTBAR_SIZE];
    int m_selectedSlot = 0;

    // Mouse-wheel slot cycling. m_wheelDir latches the last scroll direction so
    // a high-resolution wheel emitting several sub-notch values per flick still
    // advances one slot; m_wheelLock throttles repeats while held down.
    int  m_wheelDir = 0;
    float m_wheelLock = 0.0f;

    // Equipment slots — instance index, -1 = empty
    int m_equipment[EQUIP_SLOT_COUNT];

    // Player entity index (set during Init)
    int m_playerEntityIndex = -1;

    // Unlocked ethereal skill node names (persisted in TF.sav).
    std::vector<std::string> m_unlockedSkills;

    // Cached model/texture/icon handles
    struct CachedResource {
        int type = 0; // 0=unused, 1=model, 2=texture
        Model model;
        Texture2D texture;
    };
    std::vector<CachedResource> m_resources;

    // Pending fog/skybox/ambient state (set by script execution, read by host)
    bool m_pendingFog = false;
    float m_fogR = 0.0f, m_fogG = 0.0f, m_fogB = 0.0f, m_fogDensity = 0.0f;
    std::string m_pendingSkybox;
    bool m_pendingAmbient = false;
    bool m_pendingFogRestore = false;
    bool m_pendingAmbientRestore = false;
    bool m_pendingSkyboxRestore = false;
    std::string m_pendingMusic;
    // Name of the zone def whose declarative env fields are currently applied,
    // so a re-trigger of the same zone is idempotent.
    std::string m_activeEnvZoneDef;
    float m_ambientR = 0.0f, m_ambientG = 0.0f, m_ambientB = 0.0f;
    std::string m_pendingMessage;
    bool m_playerHurt = false;

    // Ammo-change hook (bridged by the host to the network layer)
    std::function<void(int, int, int, int)> m_on_ammo_changed;
    MeleeHitFn m_on_melee_hit;
    bool m_reloadStarted = false;
    // Melee swing budget. A member rather than a playerstat because
    // GetPlayerStat/SetPlayerStat are no-ops with no player entity loaded (see
    // the accessor comment). Reset to full by Init().
    float m_stamina = 100.0f;
    // Pending weapons queued by name when the hotbar is full at pickup time.
    std::vector<std::string> m_stashedWeapons;

    // Hotbar drag reorder state. -1 = not dragging / not over a slot.
    int m_dragFrom = -1;
    int m_dragTo = -1;

    int CacheModel(const std::string& path);
    int CacheTexture(const std::string& path);
    void UncacheResource(int idx);

    // Sum a numeric stat over all equipped items (`key`, e.g. "max_health_bonus").
    float EquipmentStatSum(const char* key) const;

    // Drain an instance's pending script side-effects (sound/msg/stats/pickup spawn)
    void ApplyEntityScriptEffects(EntityInstance& inst);
};
