#pragma once
#include "LightningEntityDef.hpp"
#include <string>
#include <vector>
#include <unordered_map>

// LightningEntityRegistry — global registry of entity prototypes
// Scans packages + GameData/ for *.ozls files at startup
class LightningEntityRegistry {
public:
    static LightningEntityRegistry& Instance() {
        static LightningEntityRegistry instance;
        return instance;
    }

    // Scan all loaded packages + GameData/ for .ozls files
    void Init();

    // Look up an entity definition by name
    const EntityDef* Find(const std::string& name) const;

    // Find all definitions of a given type
    void FindByType(EntityType type, std::vector<const EntityDef*>& out) const;

    // Total definitions loaded
    int Count() const { return (int)m_defs.size(); }

    // Direct access
    const std::unordered_map<std::string, EntityDef>& GetAll() const { return m_defs; }

    // Register a single parsed definition (used by editor for live reload)
    bool Register(const EntityDef& def);

    // --- Editor hot reload -------------------------------------------------
    // Fold ONE loose .ozls file back into the registry without re-parsing the
    // whole tree, so a def edited while the editor is open becomes live without
    // a manual Refresh.
    //
    // This is deliberately NOT Init(): Init clears and rebuilds both containers,
    // which can relocate `EntityDef` objects. Find() returns a pointer into
    // m_defs and that pointer is stored for the life of the session by
    // LightningEntityManager instances and SkyZoneNode::def, so a rebuild can
    // silently repoint them at a different def. Here the by-name entry is
    // assigned IN PLACE (std::unordered_map keeps element addresses stable), so
    // every holder observes the new values.
    //
    // World policy is the same one Init()+LoadWorldOverrides() produce for the
    // open world: globals and this world's defs are visible, another world's
    // copy is not, and the last matching entry wins.
    //
    // A file that no longer parses keeps its LAST GOOD definitions: an editor
    // saving a half-typed entity must not make it vanish. An EMPTY file (or a
    // deleted one) does remove them - that is an explicit disable.
    struct FileReload {
        bool ok = false;                    // false = parse failure, registry unchanged
        std::vector<std::string> added;     // names that only exist in the new file
        std::vector<std::string> changed;   // names present before and after
        std::vector<std::string> removed;   // names the file no longer defines
    };
    FileReload ReloadFile(const std::string& path);

    // Re-point the by-name map at the definitions belonging to `worldDir`.
    //
    // m_defs keeps only the last-registered def per name, but world-scoped
    // names collide: every world auto-generates a "zone_sky_0" (and often
    // "zone_reverb_0" / "zone_sound_0"), so whichever .ozls file happened to be
    // parsed last wins for every world. The result is a level that plays
    // another map's skybox/ambient/fog and prints the wrong script messages.
    //
    // Classification is TYPE-first: game-hierarchy (`: game/: campaign/
    // : sector/: mission/: objective`) and `: trigger/: event` defs are global
    // regardless of source path (IsGlobalDefType). Everything else is
    // classified by path (WorldNameOfDef).
    //
    // Only one world is loaded at a time, so re-registering this world's defs —
    // and dropping the other worlds' colliding ones — makes every by-name lookup
    // (zone on_enter/on_exit, entity hooks, FindByType) resolve against the world
    // actually being played. Global defs (guns, items, pawns) are untouched.
    // Safe to call on every LoadWorld().
    void LoadWorldOverrides(const std::string& worldDir);

    // All parsed definitions (keeps every def even when names collide across
    // worlds — m_defs only retains the last-registered per name)
    const std::vector<EntityDef>& GetAllDefs() const { return m_allDefs; }

private:
    LightningEntityRegistry() = default;
    std::unordered_map<std::string, EntityDef> m_defs;
    std::vector<EntityDef> m_allDefs;

    // Re-derive m_defs[name] from m_allDefs after the backing files changed.
    // Returns true when a def for `name` remains visible to Find(). Shared by
    // ReloadFile and LoadWorldOverrides so the two cannot disagree about which
    // def wins.
    bool RepublishName(const std::string& name);

    // Last LoadWorldOverrides() target ("" = no world policy applies). Used by
    // RepublishName to decide whether a world-scoped def belongs to the world
    // being edited.
    std::string m_worldDir;
};
