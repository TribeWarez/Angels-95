// ============================================================================
// PropsPanel.cpp
//
// FRAGMENT - not a standalone translation unit. Included by ../UiShell.cpp,
// which is the single TU for the whole UI layer. That is deliberate: see
// UiShell.cpp for why the layer is built as a unity build rather than as 16
// separate objects.
// ============================================================================

// =====================================================================
// Properties Panel - context-sensitive, dynamic controls
// =====================================================================
// Post the properties apply for whatever this panel is currently showing.
//
// The SelRef is captured HERE, at the moment Apply was pressed, so the handler cannot
// end up writing to a different entity than the one whose rows are on screen - the
// stale-index bug class this whole bus exists to remove. The edited VALUES are not in
// the payload; see Subsystems/PropsApply.cpp for why reading those live is deliberate.
//
// propsTargetType is sel::DEF_ONLY (-1) for a def-only panel, which ToBusKind maps to
// SelKind::None - so Apply on such a row posts an invalid SelRef and the handler skips
// it rather than acting on the previous selection.
static void PostApplyProperties() {
    ed::SelRef ref;
    ref.kind  = ToBusKind((SelType)g_editorPanels.propsTargetType);
    ref.index = g_editorPanels.propsTargetIndex;
    ed::EventBus::instance().post(ed::Ev::ApplyProperties, ref);
}

static const int ID_PP_POSX  = 401;
static const int ID_PP_POSY  = 402;
static const int ID_PP_POSZ  = 403;
static const int ID_PP_ROT   = 404;
static const int ID_PP_SX    = 405;
static const int ID_PP_SY    = 406;
static const int ID_PP_SZ    = 407;
static const int ID_PP_APPLY = 408;
static const int ID_PP_CLOSE = 409;
static const int ID_PP_LABEL = 410;
static const int ID_PP_TEX_SCALE_U = 411;
static const int ID_PP_TEX_SCALE_V = 412;
static const int ID_PP_TEX_OFF_U = 413;
static const int ID_PP_TEX_OFF_V = 414;
// Def-aligned sections
static const int ID_PP_DEFBLOCK = 415;   // read-only def summary
static const int ID_PP_EDITDEF  = 416;   // "Edit .ozls" button
static const int ID_PP_HEALTH   = 417;   // NPC instance
static const int ID_PP_SPEED    = 418;   // NPC instance
static const int ID_PP_RESPAWN  = 419;   // pickup instance
static const int ID_PP_ZONETYPE = 420;   // zone combo
static const int ID_PP_ZONEINT  = 421;   // zone intensity
static const int ID_PP_ZONENAME = 422;   // zone script-hook name
static const int ID_PP_ZONEGRAV    = 423; // zone gravity (PhysicsInfo)
static const int ID_PP_ZONEJUMP    = 424; // zone jump speed
static const int ID_PP_ZONETERM    = 425; // zone terminal velocity
static const int ID_PP_ZONEWGRAV   = 426; // zone water gravity
static const int ID_PP_ZONEWDRAG   = 427; // zone water drag
static const int ID_PP_ZONESWIM    = 428; // zone swim-up speed
static const int ID_PP_ZONELADDER  = 429; // zone ladder speed
static const int ID_PP_ZONEFLYMULT = 430; // zone fly/noclip speed multiplier
// Portal fields get their OWN range (465+). They previously reused 423-428,
// colliding with the eight zone-physics IDs above: GetDlgItem(id) resolves per
// window, so this only stayed harmless because zones and portals are mutually
// exclusive SelTypes. Any future section showing both would cross-wire silently.
// ID_PP_MESHPATH likewise moved off 430 (it aliased ID_PP_ZONEFLYMULT).
// Nothing else is renumbered, so no call site outside this block can be stale.
static const int ID_PP_PORTALWORLD  = 465;
static const int ID_PP_PSPAWNX      = 466;
static const int ID_PP_PSPAWNY      = 467;
static const int ID_PP_PSPAWNZ      = 468;
static const int ID_PP_PBIDIR       = 469;
// Portal delete. 470 (ID_PP_PORTALBROWSE) was declared but never created and never
// handled — there was no Browse button for the target world — so it is free. This
// is the ONLY route to RemovePortal() now that the old Zone window's portal list is
// gone; propsTargetIndex is already the index into ZoneManager::GetPortals(), which
// is what g_editorPanels.actionDeletePortal expects.
static const int ID_PP_PORTALDELETE = 470;
// GameEngine.Mesh scale/model-path fields. 471 was the last free slot inside the
// 401-470 layout; 637 is just past the end of the generated stat-row window
// (487 + 50 rows + 50 previews + 50 browses = 636). Both used to alias
// zone-physics / light IDs.
static const int ID_PP_SCALE    = 471;    // GameEngine.Mesh uniform scale
static const int ID_PP_MESHPATH = 637;    // GameEngine.Mesh model path
static const int ID_PP_MESHTEX  = 431;    // GameEngine.Mesh texture path
static const int ID_PP_ANIMCLIP = 432;    // GameEngine.Mesh.Skeletal clip
static const int ID_PP_MESHRELOAD = 433;  // force mesh reload button
static const int ID_PP_EMITTER_TYPE   = 434;
static const int ID_PP_EMITTER_TEX    = 435;
static const int ID_PP_EMITTER_RATE   = 436;
static const int ID_PP_EMITTER_LIFE   = 437;
static const int ID_PP_EMITTER_SPEED  = 438;
static const int ID_PP_EMITTER_SIZE   = 439;
static const int ID_PP_EMITTER_SPREAD = 440;
static const int ID_PP_EMITTER_R      = 441;
static const int ID_PP_EMITTER_G      = 442;
static const int ID_PP_EMITTER_B      = 443;
static const int ID_PP_PATHNAME       = 444;
static const int ID_PP_PATHRADIUS     = 445;
static const int ID_PP_PATHNEXT       = 446;
static const int ID_PP_PATHLOOP       = 447;
static const int ID_PP_MESHWIND       = 448;
static const int ID_PP_WIND_SX        = 449;
static const int ID_PP_WIND_SY        = 450;
static const int ID_PP_WIND_SZ        = 451;
static const int ID_PP_WIND_DIRX      = 452;
static const int ID_PP_WIND_DIRY      = 453;
static const int ID_PP_WIND_DIRZ      = 454;
static const int ID_PP_WIND_STRENGTH  = 455;
static const int ID_PP_WIND_FREQ      = 456;
static const int ID_PP_MESHTEX_BROWSE   = 457;
static const int ID_PP_MESHTEX_ACTIVE   = 458;
static const int ID_PP_EMITTERTEX_BROWSE = 459;
static const int ID_PP_EMITTERTEX_ACTIVE = 460;
static const int ID_PP_MESHANIMFILE       = 461;
static const int ID_PP_MESHANIMFILE_BROWSE = 462;
static const int ID_PP_MESHANIMSPEED      = 463;
static const int ID_PP_CONVERT_ANIMATED   = 464;
// Light editing (SelType::LIGHT). Own range 472-486: the old 470-484 overlapped
// the portal block (465-470) at 470 and, once MESHPATH moved to 471, at 471 too.
// The panel is rebuilt per entity type so live controls never collide in
// practice, but the constant namespace still must be disjoint so a future
// combined section cannot silently cross-wire two entities' fields.
static const int ID_PP_LIGHT_TYPE     = 472;
static const int ID_PP_LIGHT_EFFECT   = 473;
static const int ID_PP_LIGHT_R        = 474;
static const int ID_PP_LIGHT_G        = 475;
static const int ID_PP_LIGHT_B        = 476;
static const int ID_PP_LIGHT_INTENS   = 477;
static const int ID_PP_LIGHT_RADIUS   = 478;
static const int ID_PP_LIGHT_INNER    = 479;
static const int ID_PP_LIGHT_OUTER    = 480;
static const int ID_PP_LIGHT_FLARE    = 481;
static const int ID_PP_LIGHT_CORONA   = 482;
static const int ID_PP_LIGHT_TARGETX  = 483;
static const int ID_PP_LIGHT_TARGETY  = 484;
static const int ID_PP_LIGHT_TARGETZ  = 485;
static const int ID_PP_LIGHT_NAME     = 486;

// Editable .ozls stat rows. The count is a fixed ceiling rather than a
// per-key constant because rows are generated from the def's stat schema; row
// N uses ID_PP_STAT_FIELD_0 + N. Per-row IDs (rather than two shared Preview /
// Browse ids plus a hit-test) mean a click cannot land on the wrong row.
static const int ID_PP_STAT_FIELD_0    = 487;
static const int ID_PP_STAT_PREVIEW_0  = 537;
static const int ID_PP_STAT_BROWSE_0   = 587;
// Bound on editable rows. A def is free to author more stats than this; the
// extras still render as read-only rows rather than disappearing.
static const int ID_PP_STAT_MAX        = 45;

// Group headers. Parked at 20000: the row-ID space tops out at ID_PP_MESHPATH
// (637) with the Map rows at 700-727 and the zone Environment rows at 730-751,
// and the colour pickers mint ids at id + 10000. 20000 is clear of all of them.
static const int ID_PP_SECTION_0 = 20000;

// --- SelType::MAP control IDs ---
// Parked above the highest stat-row ID (587 + 50 = 637, and ID_PP_MESHPATH
// already took 637), so a Map row can never collide with a generated stat row.
static const int ID_PP_MAP_GAMETYPE     = 700;
static const int ID_PP_MAP_MAXPLAYERS   = 701;
static const int ID_PP_MAP_RESPAWN      = 702;
static const int ID_PP_MAP_TIMELIMIT_ON = 703;
static const int ID_PP_MAP_TIMELIMIT    = 704;
static const int ID_PP_MAP_SCORELIMIT   = 705;
static const int ID_PP_MAP_FRIENDLY     = 706;
static const int ID_PP_MAP_SUMMARY      = 707;
static const int ID_PP_MAP_SKYBOX       = 708;
static const int ID_PP_MAP_SKYBOX_BROWSE= 709;
static const int ID_PP_MAP_SKYBOX_ACTIVE= 710;
static const int ID_PP_MAP_SKYBOX_SIDE  = 711;
static const int ID_PP_MAP_PTYPE        = 720;
static const int ID_PP_MAP_PDENSITY     = 721;
static const int ID_PP_MAP_PSPEED       = 722;
static const int ID_PP_MAP_PR           = 723;
static const int ID_PP_MAP_PG           = 724;
static const int ID_PP_MAP_PB           = 725;
static const int ID_PP_MAP_PWINDX       = 726;
static const int ID_PP_MAP_PWINDZ       = 727;

// --- Zone environment override rows (SelType::ZONE) ---
static const int ID_PP_ZENV_FOG_ON      = 730;
static const int ID_PP_ZENV_FOG_R       = 731;
static const int ID_PP_ZENV_FOG_G       = 732;
static const int ID_PP_ZENV_FOG_B       = 733;
static const int ID_PP_ZENV_FOG_DENS   = 734;
static const int ID_PP_ZENV_FOG_START  = 735;
static const int ID_PP_ZENV_FOG_END    = 736;
static const int ID_PP_ZENV_AMB_ON      = 740;
static const int ID_PP_ZENV_AMB_R       = 741;
static const int ID_PP_ZENV_AMB_G       = 742;
static const int ID_PP_ZENV_AMB_B       = 743;
static const int ID_PP_ZENV_AMB_INT     = 744;
static const int ID_PP_ZENV_REVERB_MIX  = 750;
static const int ID_PP_ZENV_REVERB_DEC  = 751;
// Particle emitter authoring continued (760+). The original block (435-443)
// stopped at the START values; sizeEnd/colorEnd/gravity/radius/billboard were
// loader-only, so a hand-authored fade-out was lost on the first Apply.
static const int ID_PP_EMITTER_SIZEEND  = 760;
static const int ID_PP_EMITTER_REND     = 761;
static const int ID_PP_EMITTER_GEND     = 762;
static const int ID_PP_EMITTER_BEND     = 763;
static const int ID_PP_EMITTER_GRAVITY  = 764;
static const int ID_PP_EMITTER_RADIUS   = 765;
static const int ID_PP_EMITTER_BILLBOARD = 766;

// Read one editable stat row's field as text. Returns "" when the control is
// absent, which the callers treat as "no edit".
static std::string readStatRowText(HWND hwnd, int id) {
    HWND hCtrl = GetDlgItem(hwnd, id);
    if (!hCtrl) return "";
    wchar_t buf[512];
    GetWindowTextW(hCtrl, buf, 512);
    char out[512] = {0};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, 512, nullptr, nullptr);
    return std::string(out);
}

// ---------------------------------------------------------------------------
// Editable .ozls stat schema
//
// The 1:1 mapping between an authored key and the runtime that reads it. This
// is deliberately a list rather than "everything numeric is editable": writing
// a key nothing reads would be a silent no-op, and editing a key the runtime
// never consults is worse than showing it read-only.
//
// `float` keys render as numeric fields; `vec3` keys are edited as three
// comma-separated floats in one field; sound paths get Browse + Preview.
// ---------------------------------------------------------------------------
namespace {

struct StatSpec {
    const char* key;
    int kind;                 // 0 = float, 1 = string, 2 = vec3, 3 = sound path
    const char* label;
    // Sub-heading this row belongs under, emitted as a group header when it
    // changes. Empty means "no group" (the row joins whatever header came before,
    // or none at all). The panel used to render the schema as one flat wall of
    // ~30 rows, which is why it needed a scrollbar to reach Apply at all.
    // Grouping also earns the wider label column the def block uses.
    const char* group;
};

const StatSpec kWeaponStats[] = {
    {"damage",                 0, "Damage", "Damage"},
    {"fire_rate",              0, "Fire Rate", "Damage"},
    {"magazine",               0, "Magazine", "Handling"},
    {"reload_time",            0, "Reload Time", "Handling"},
    {"spread",                 0, "Spread", "Handling"},
    {"stamina_cost",           0, "Stamina Cost", "Handling"},
    {"recoil",                 0, "Recoil", "Handling"},
    // Melee rows moved up to sit with each other. They used to be interleaved
    // (swing_speed before magazine, reach after spread), which made the schema
    // read as one flat wall and defeated the grouping.
    {"swing_speed",            0, "Swing Speed", "Melee"},
    {"reach",                  0, "Melee Reach", "Melee"},
    {"projectile_speed",       0, "Proj Speed", "Projectile"},
    {"projectile_lifetime",    0, "Proj Lifetime", "Projectile"},
    {"projectile_count",       0, "Proj Count", "Projectile"},
    {"projectile_submesh",     0, "Proj Submesh", "Projectile"},
    {"projectile_scale",       0, "Proj Scale", "Projectile"},
    {"projectile_mesh",        1, "Projectile Mesh", "Projectile"},
    {"projectile_texture",     1, "Projectile Tex", "Projectile"},
    {"projectile_color",       2, "Projectile RGB", "Projectile"},
    {"viewmodel_mesh",         1, "Viewmodel Mesh", "Viewmodel"},
    {"viewmodel_texture",      1, "Viewmodel Tex", "Viewmodel"},
    {"viewmodel_offset",       2, "Viewmodel Pos", "Viewmodel"},
    {"viewmodel_rot",          2, "Viewmodel Rot", "Viewmodel"},
    {"viewmodel_scale",        0, "Viewmodel Scale", "Viewmodel"},
    // Data-driven audio. Volume/pitch default to 1.0 in the runtime, so an
    // omitted row plays the authored sound at unity.
    {"fire_sound",             3, "Fire Sound", "Audio"},
    {"fire_volume",            0, "Fire Volume", "Audio"},
    {"fire_pitch",             0, "Fire Pitch", "Audio"},
    {"swing_sound",            3, "Swing Sound", "Audio"},
    {"swing_volume",           0, "Swing Volume", "Audio"},
    {"swing_pitch",            0, "Swing Pitch", "Audio"},
    {"hit_sound",              3, "Hit Sound", "Audio"},
    {"hit_volume",             0, "Hit Volume", "Audio"},
    {"reload_sound",           3, "Reload Sound", "Audio"},
    {"reload_volume",          0, "Reload Vol", "Audio"},
    {"equip_sound",            3, "Equip Sound", "Audio"},
    {"equip_volume",           0, "Equip Volume", "Audio"},
};

// Spells. Kept separate from kWeaponStats rather than sharing it: a spell has no
// magazine/reload/viewmodel/recoil, and a weapon has no cost/radius/heal. A
// shared schema would show every weapon 8 rows it can never use (and vice
// versa), which is exactly the "shows stats it has never set" problem the
// schema-from-SCHEMA design already has to be careful about.
//
// `cost_type` is a STRING row: author it quote-free and space-free
// (psychic_energy or pe for the PE pool, anything else means mana).
const StatSpec kSpellStats[] = {
    {"mana_cost",              0, "Mana Cost", "Cost"},
    {"pe_cost",                0, "PE Cost", "Cost"},
    {"cost_type",              1, "Cost Type", "Cost"},
    {"cooldown",               0, "Cooldown", "Cost"},
    // Effect. `radius` > 0 makes it an instant area cast; otherwise projectiles.
    {"damage",                 0, "Damage", "Effect"},
    {"reach",                  0, "Reach", "Effect"},
    {"radius",                 0, "Radius (AoE)", "Effect"},
    {"targets_self",           0, "Targets Self", "Effect"},
    {"heal",                   0, "Heal", "Effect"},
    {"projectile_speed",       0, "Proj Speed", "Projectile"},
    {"projectile_lifetime",    0, "Proj Lifetime", "Projectile"},
    {"projectile_count",       0, "Proj Count", "Projectile"},
    {"projectile_submesh",     0, "Proj Submesh", "Projectile"},
    {"projectile_scale",       0, "Proj Scale", "Projectile"},
    {"spread",                 0, "Spread", "Projectile"},
    {"projectile_mesh",        1, "Projectile Mesh", "Projectile"},
    {"projectile_texture",     1, "Projectile Tex", "Projectile"},
    {"projectile_color",       2, "Projectile RGB", "Projectile"},
    {"cast_sound",             3, "Cast Sound", "Audio"},
    {"cast_volume",            0, "Cast Volume", "Audio"},
    {"hit_sound",              3, "Hit Sound", "Audio"},
    {"hit_volume",             0, "Hit Volume", "Audio"},
};

const StatSpec kPlayerStats[] = {
    {"health",                 0, "Health", "Vitals"},
    {"max_health",             0, "Max Health", "Vitals"},
    {"mana",                   0, "Mana", "Vitals"},
    {"max_mana",               0, "Max Mana", "Vitals"},
    {"psychic_energy",         0, "Psychic", "Vitals"},
    {"max_psychic_energy",     0, "Max Psychic", "Vitals"},
    {"level",                  0, "Level", "Progression"},
    {"xp",                     0, "XP", "Progression"},
    {"xp_to_next",             0, "XP To Next", "Progression"},
    // Rendered height for remote-player bodies, in engine units. Negative =
    // authored scale, no normalisation. See PlayerModel::NormalisedScale.
    {"model_height",           0, "Model Height", "Progression"},
    {"jump_sound",             3, "Jump Sound", "Audio"},
    {"jump_volume",            0, "Jump Volume", "Audio"},
    {"land_sound",             3, "Land Sound", "Audio"},
    {"land_volume",            0, "Land Volume", "Audio"},
    {"walk_sound",             3, "Walk Sound", "Audio"},
    {"walk_volume",            0, "Walk Volume", "Audio"},
    {"run_sound",              3, "Run Sound", "Audio"},
    {"run_volume",             0, "Run Volume", "Audio"},
    {"hurt_sound",             3, "Hurt Sound", "Audio"},
    {"hurt_volume",            0, "Hurt Volume", "Audio"},
    {"death_sound",            3, "Death Sound", "Audio"},
    {"death_volume",           0, "Death Volume", "Audio"},
};

const StatSpec kPawnStats[] = {
    {"speed",                  0, "Speed", "Combat"},
    {"aggroRange",             0, "Aggro Range", "Combat"},
    {"attackRange",            0, "Attack Range", "Combat"},
    {"damage",                 0, "Damage", "Combat"},
    {"maxHealth",              0, "Max Health", "Combat"},
    {"sprite_path",            1, "Sprite", "Appearance"},
    {"model_path",             1, "Model", "Appearance"},
    {"model_texture",          1, "Model Tex", "Appearance"},
    {"model_scale",            0, "Model Scale", "Appearance"},
    {"mesh_type",              1, "Mesh Type", "Animation"},
    {"anim_idle",              1, "Anim Idle", "Animation"},
    {"anim_patrol",            1, "Anim Patrol", "Animation"},
    {"anim_chase",             1, "Anim Chase", "Animation"},
    {"anim_return",            1, "Anim Return", "Animation"},
    {"anim_death",             1, "Anim Death", "Animation"},
    {"anim_speed",             0, "Anim Speed", "Animation"},
    {"hurt_sound",             3, "Hurt Sound", "Audio"},
    {"death_sound",            3, "Death Sound", "Audio"},
};

// A light's `.ozls` def is a DEFAULTS layer (see ApplyLightDefDefaults in
// OzOzoneLoader.cpp): a value the OZONE light line authored always wins. Only
// keys the line cannot express are listed - effect/flare/corona apply solely
// when the line omitted them, and the rest have no line syntax at all.
// intensity / radius / color are deliberately absent: they are positional on
// every light line, so offering them here would suggest an edit that the
// runtime ignores.
const StatSpec kLightStats[] = {
    {"effect",               0, "Effect", "Effect"},
    {"flare",                0, "Flare", "Effect"},
    {"corona",               0, "Corona", "Effect"},
    {"period",               0, "Period", "Effect"},
    {"cast_shadow",          0, "Cast Shadow", "Shadow"},
    {"is_static",            0, "Static (baked)", "Shadow"},
    {"inner_cone",           0, "Inner Cone", "Spot cone"},
    {"outer_cone",           0, "Outer Cone", "Spot cone"},
};

// The schema for `type`, or nullptr when nothing is editable for it. Only
// entity types whose def the panel can actually write get an entry.
const StatSpec* StatSchemaFor(EntityType type, int* outCount) {
    auto set = [&](const StatSpec* s, int n) { *outCount = n; return s; };
    switch (type) {
        case EntityType::WEAPON: return set(kWeaponStats, (int)(sizeof(kWeaponStats)/sizeof(StatSpec)));
        case EntityType::PAWN:   return set(kPawnStats,   (int)(sizeof(kPawnStats)/sizeof(StatSpec)));
        // Player.ozls is declared `: upgrade` (see GameData/Global/Objects),
        // so match on the def NAME as well - there is no dedicated player type.
        case EntityType::UPGRADE: return set(kPlayerStats, (int)(sizeof(kPlayerStats)/sizeof(StatSpec)));
        case EntityType::LIGHT:   return set(kLightStats,  (int)(sizeof(kLightStats)/sizeof(StatSpec)));
        case EntityType::SPELL:   return set(kSpellStats,  (int)(sizeof(kSpellStats)/sizeof(StatSpec)));
        default: *outCount = 0; return nullptr;
    }
}

}  // namespace

// Build a read-only def summary (stats + actions + PawnDef block). Shared by the
// Script Manager detail pane and the Properties panel def section.
static void BuildDefSummary(const EntityDef& def, const std::string& sourcePath,
                            int actionCountHint, std::string& out) {
    out.clear();
    out += def.name + "  [" + EntityTypeName(def.type) + "]\n";
    if (!sourcePath.empty()) out += "source: " + sourcePath + "\n";
    if (!def.mesh.empty())    out += "mesh: " + def.mesh + "\n";
    if (!def.texture.empty()) out += "texture: " + def.texture + "\n";
    if (!def.icon.empty())    out += "icon: " + def.icon + "\n";
    if (!def.skybox.empty())  out += "skybox: " + def.skybox + "\n";
    if (!def.music.empty())   out += "music: " + def.music + "\n";

    if (def.type == EntityType::PAWN) {
        for (const auto& pd : PawnSystem::Instance().GetDefs()) {
            if (pd.name != def.name) continue;
            out += "\npawn stats (PawnDefs/*.cfg):\n";
            out += "  speed = " + std::to_string(pd.speed) + "\n";
            out += "  aggroRange = " + std::to_string(pd.aggroRange) + "\n";
            out += "  attackRange = " + std::to_string(pd.attackRange) + "\n";
            out += "  damage = " + std::to_string(pd.damage) + "\n";
            out += "  maxHealth = " + std::to_string(pd.maxHealth) + "\n";
            if (!pd.sprite_path.empty()) out += "  sprite = " + pd.sprite_path + "\n";
            if (!pd.scream_path.empty()) out += "  scream = " + pd.scream_path + "\n";
            break;
        }
    }
    if (!def.stats.floats.empty()) {
        out += "\nstats:\n";
        std::vector<std::pair<std::string, float>> sv(def.stats.floats.begin(), def.stats.floats.end());
        std::sort(sv.begin(), sv.end());
        for (auto& [k, v] : sv) out += "  " + k + " = " + std::to_string(v) + "\n";
    }
    if (!def.stats.strings.empty()) {
        out += "\nstrings:\n";
        std::vector<std::pair<std::string, std::string>> ss(def.stats.strings.begin(), def.stats.strings.end());
        std::sort(ss.begin(), ss.end());
        for (auto& [k, v] : ss) out += "  " + k + " = " + v + "\n";
    }
    int actions = actionCountHint >= 0 ? actionCountHint : (int)def.actions.size();
    if (actions > 0) {
        out += "\nactions:\n";
        if (!def.actions.empty()) {
            for (const auto& a : def.actions)
                out += "  " + a.name + " (" + std::to_string(a.scriptLines.size()) + " lines)\n";
        } else {
            out += "  (" + std::to_string(actions) + ")\n";
        }
    }
}

// Compact number formatting for read-only def value rows (1 not 1.000000)
static std::string FormatStat(float v) {
    char buf[32];
    if (std::fabs(v - std::round(v)) < 0.0001f)
        snprintf(buf, sizeof(buf), "%d", (int)std::round(v));
    else
        snprintf(buf, sizeof(buf), "%g", v);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Properties-panel vertical scrolling
//
// A weapon def generates ~30 editable stat rows on top of the read-only dump,
// so the panel is far taller than any screen. It used to just grow until the
// Apply/Close buttons fell off the bottom of the work area, with no way to
// reach them. Instead the window is capped to the work area and every child is
// repositioned by -scrollPos.
//
// Children are real Win32 controls, so the offset is applied by moving each
// window rather than by painting (which is what the texture grid does, since it
// owns no children). Base Y is captured once after Populate builds the rows.
// ---------------------------------------------------------------------------
static int  g_propsScroll   = 0;
static int  g_propsContentH = 0;                       // full height the rows want
static std::vector<std::pair<HWND, int>> g_propsRows;  // child hwnd, base Y

// Record each row's un-scrolled Y so scrolling can move it without re-deriving
// the layout. Must run AFTER all children are created.
static void PropsCaptureRows(HWND hwnd) {
    g_propsRows.clear();
    g_propsScroll = 0;
    for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        RECT r;
        if (!GetWindowRect(c, &r)) continue;
        POINT p = { r.left, r.top };
        ScreenToClient(hwnd, &p);
        g_propsRows.push_back({ c, p.y });
    }
}

static int PropsMaxScroll(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int m = g_propsContentH - rc.bottom;
    return (m > 0) ? m : 0;
}

// Publish the scroll range and show the bar only when there is something to
// scroll.
//
// This MUST run after the window has been sized: nPage comes from the client
// height, so deciding WS_VSCROLL first and resizing afterwards left a panel
// that was taller than its content before the resize and shorter after it,
// with no bar to scroll the overflow with.
static void PropsApplyScroll(HWND hwnd) {
    const int maxScroll = PropsMaxScroll(hwnd);
    if (g_propsScroll > maxScroll) g_propsScroll = maxScroll;
    if (g_propsScroll < 0)          g_propsScroll = 0;

    for (const auto& row : g_propsRows) {
        if (!IsWindow(row.first)) continue;
        SetWindowPos(row.first, nullptr, 0, row.second - g_propsScroll, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    SCROLLINFO si = { sizeof(SCROLLINFO), SIF_POS, 0, 0, 0, 0 };
    si.nPos = g_propsScroll;
    SetScrollInfo(hwnd, SB_VERT, &si, FALSE);
}

// Publish the scroll range and show the bar only when there is something to
// scroll.
//
// This MUST run after the window has been sized: nPage comes from the client
// height, so deciding WS_VSCROLL first and resizing afterwards left a panel
// that was taller than its content before the resize and shorter after it,
// with no bar to scroll the overflow with.
static void PropsUpdateScroll(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int clientH = rc.bottom;

    int range = g_propsContentH - clientH;
    if (range < 0) range = 0;

    const bool needBar = (range > 0);
    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    const bool haveBar = (style & WS_VSCROLL) != 0;
    if (needBar != haveBar) {
        SetWindowLong(hwnd, GWL_STYLE,
                      needBar ? (style | WS_VSCROLL)
                              : (style & ~(LONG)WS_VSCROLL));
        // Toggling WS_VSCROLL on an overlapped window does not repaint the bar
        // on its own; SWP_FRAMECHANGED is what makes it appear or disappear.
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    SCROLLINFO si = { sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS };
    si.nMin  = 0;
    si.nMax  = range;
    si.nPage = (UINT)clientH;
    si.nPos  = g_propsScroll;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);

    PropsApplyScroll(hwnd);
}

// Size the window to what the rows need, but never past the monitor's work
// area, then publish the scroll range.
static void PropsFitWindow(HWND hwnd, int neededH) {
    g_propsContentH = neededH;

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    int availH = 0;
    if (GetMonitorInfo(mon, &mi))
        availH = mi.rcWork.bottom - mi.rcWork.top;
    if (availH < 200) availH = 600;   // no monitor info: keep some sane floor

    int wantH = neededH;
    if (wantH > availH) wantH = availH;

    RECT wr;
    GetWindowRect(hwnd, &wr);
    if (wr.bottom - wr.top != wantH)
        SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, wantH,
                     SWP_NOMOVE | SWP_NOZORDER);

    PropsUpdateScroll(hwnd);
}

// Rewrite the "resolved ruleset" line for the currently selected Game Mode.
//
// Deliberately uses ResolveGameTypeInfo rather than GameTypeInfoFor: the former
// layers built-in table -> .ozls : gametype override -> the level's own levelinfo
// numbers, which is the order the client resolves in (GameState::init_worlds).
// The legacy Zone Properties summary used GameTypeInfoFor, so it showed the
// built-in defaults and disagreed with what the game would actually run.
static void RefreshMapRulesetSummary(HWND hwnd) {
    HWND hLabel = GetDlgItem(hwnd, ID_PP_MAP_SUMMARY);
    if (!hLabel) return;
    HWND hCombo = GetDlgItem(hwnd, ID_PP_MAP_GAMETYPE);
    int sel = hCombo ? (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0) : -1;
    if (sel < 0) sel = g_editorPanels.propMapGameType;

    const auto& P = g_editorPanels;
    LevelSettings ls;
    ls.maxPlayers = P.propMapMaxPlayers;
    ls.respawnTime = P.propMapRespawnTime;
    ls.timeLimitEnabled = P.propMapTimeLimitEnabled;
    ls.timeLimitMinutes = P.propMapTimeLimitMinutes;
    ls.scoreLimit = P.propMapScoreLimit;
    ls.friendlyFire = P.propMapFriendlyFire;

    const oz::gametype::GameTypeInfo& info =
        oz::gametype::ResolveGameTypeInfo(static_cast<oz::gametype::GameType>(sel), ls, "");

    wchar_t buf[512];
    _snwprintf(buf, 511,
        L"%s | teams=%d | FF=%s | kill=%d | scoreLimit=%d%s | timeLimit=%s",
        std::wstring(info.label, info.label + strlen(info.label)).c_str(),
        info.teamCount,
        info.friendlyFire ? L"on" : L"off",
        info.killScore,
        info.scoreLimitEnabled ? info.scoreLimit : 0,
        info.scoreLimitEnabled ? L"" : L" (off)",
        info.timeLimitEnabled ? L"on" : L"off");
    buf[511] = 0;
    SetWindowTextW(hLabel, buf);
}

static void PopulatePropertiesPanel(HWND hwnd) {
    // Destroy existing controls
    HWND child = GetWindow(hwnd, GW_CHILD);
    while (child) {
        HWND next = GetWindow(child, GW_HWNDNEXT);
        DestroyWindow(child);
        child = next;
    }

    int selType = g_editorPanels.propsTargetType;
    int selIdx  = g_editorPanels.propsTargetIndex;
    RECT rc;
    GetClientRect(hwnd, &rc);
    int x = 10, y = 10, ew = 100, bw = 80, rowH = 24;
    // ONE label width for the whole panel. It used to be lw = 74 for the instance
    // rows (Pos/Rot/Size/Tex) and 96 for everything from the first group header on,
    // which put a visible 22px step in the value column at the boundary. Every row
    // below uses this, including addSection's headers.
    const int lw = 96;

    // Title label
    {
        char title[128];
        snprintf(title, sizeof(title), "Properties: %s",
                 g_editorPanels.propsTargetName.c_str());
        std::wstring wtitle(title, title + strlen(title));
        CreateLabel(hwnd, wtitle.c_str(), x, y, rc.right - 20, 20, ID_PP_LABEL);
        y += 26;
    }

    // Common position fields
    auto addField = [&](const wchar_t* label, int id, float val) {
        CreateLabel(hwnd, label, x, y, lw, 20, 0);
        std::wstring wval = std::to_wstring(val);
        HWND hEdit = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", wval.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            x + lw, y, ew, 22, hwnd, (HMENU)(INT_PTR)id, g_hInst, nullptr);
        y += rowH;
        return hEdit;
    };

    // Pos X/Y/Z are emitted before any type branch, so a type with no
    // transform needs them suppressed explicitly rather than by omitting a
    // branch. SelType::MAP is the level: no position, no rotation, no size.
    const bool hasTransform = (selType != sel::MAP) && (selType != sel::DEF_ONLY);

    if (hasTransform) {
        addField(L"Pos X:", ID_PP_POSX, g_editorPanels.propPosX);
        addField(L"Pos Y:", ID_PP_POSY, g_editorPanels.propPosY);
        addField(L"Pos Z:", ID_PP_POSZ, g_editorPanels.propPosZ);
    }

    // Type-specific fields
    if (selType == sel::BRUSH || selType == sel::ZONE || selType == sel::PORTAL) {  // add size fields
        // For a cylinder these three fields ARE rTop / rBot / height (see
        // OzoneLoader::UpdateBrushRenderable, which passes them as
        // BuildCylinder(size.x, size.y, size.z, ...)), so the generic
        // "Size X/Y/Z" labels hid the fact that a taper is authorable here. Name
        // them what they are.
        const wchar_t* sxLabel = L"Size X:";
        const wchar_t* syLabel = L"Size Y:";
        const wchar_t* szLabel = L"Size Z:";
        bool isCyl = false;
        if (selType == sel::BRUSH) {
            OzoneRenderable* br = OzoneLoader::Instance().Get(selIdx);
            isCyl = br && br->typeId == (int)OzonePrimitiveType::CYLINDER;
        }
        if (isCyl) {
            sxLabel = L"Radius Top:";
            syLabel = L"Radius Bot:";
            szLabel = L"Height:";
        }
        addField(sxLabel, ID_PP_SX, g_editorPanels.propSizeX);
        addField(syLabel, ID_PP_SY, g_editorPanels.propSizeY);
        addField(szLabel, ID_PP_SZ, g_editorPanels.propSizeZ);
    }

    // Texture scale/offset for brushes
    if (selType == sel::BRUSH) {
        addField(L"Tex U Scale:", ID_PP_TEX_SCALE_U, g_editorPanels.propTexScaleU);
        addField(L"Tex V Scale:", ID_PP_TEX_SCALE_V, g_editorPanels.propTexScaleV);
        addField(L"Tex U Off:", ID_PP_TEX_OFF_U, g_editorPanels.propTexOffsetU);
        addField(L"Tex V Off:", ID_PP_TEX_OFF_V, g_editorPanels.propTexOffsetV);
    }

    // Rotation — also a transform field, so suppressed for the level row.
    // Note the Rot row is seeded from a 0.0f default for types that have no yaw
    // concept; the apply handler gates its write on propsTargetHasRotation so it
    // cannot clobber an authored value (e.g. playerstart's yaw).
    if (hasTransform)
        addField(L"Rot:", ID_PP_ROT, g_editorPanels.propRotation);

    // ---- Def-aligned sections -------------------------------------------------
    // Section header + read-only key/value row helpers
    int g_sectionSeq = 0;   // resets per rebuild, so header IDs never go stale
    auto addSection = [&](const char* title) {
        std::wstring wt(title, title + strlen(title));
        // Distinct ID per header. They used to all share control ID 1, which is
        // useless twice over: GetDlgItem(hwnd, 1) returns an arbitrary one of them,
        // and a shared ID is how a "distinct control ID" invariant gets broken by
        // accident. The IDs start above the row-ID space so they can never collide
        // with a real field.
        HWND h = CreateLabel(hwnd, wt.c_str(), x, y, rc.right - 20, 18, ID_PP_SECTION_0 + g_sectionSeq++);
        // A STATIC control has no bold style bit, so the weight has to come from
        // the font. Without this the headers are the same size and weight as the
        // row labels, which is why the grouping read as decoration rather than
        // structure.
        SendMessage(h, WM_SETFONT, (WPARAM)GetBoldUiFont(), TRUE);
        y += 22;
    };
    // (a read-only row helper used to live here; addDefRow below replaced it for
    //  the def block so the whole block could share one wider label column)
    auto addTextField = [&](const wchar_t* label, int id, const std::string& val) {
        CreateLabel(hwnd, label, x, y, lw, 20, 0);
        std::wstring wval(val.begin(), val.end());
        HWND h = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", wval.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            x + lw, y, rc.right - (x + lw) - 20, 22, hwnd, (HMENU)(INT_PTR)id, g_hInst, nullptr);
        y += rowH;
        return h;
    };

    // Integer field, restricted to digits and a sign so a stray letter cannot be
    // typed into a count and silently parsed as 0 by wcstod on Apply.
    auto addIntField = [&](const wchar_t* label, int id, int val) {
        CreateLabel(hwnd, label, x, y, lw, 20, 0);
        std::wstring wval = std::to_wstring(val);
        HWND h = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", wval.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
            x + lw, y, ew, 22, hwnd, (HMENU)(INT_PTR)id, g_hInst, nullptr);
        y += rowH;
        return h;
    };

    // Checkbox row. Takes `y` by pointer because the caller advances the shared
    // layout cursor, and checkboxes are laid out inline rather than through the
    // field helpers above.
    auto addCheckBox = [&](const wchar_t* label, int id, bool checked, int* cursor, int rowH_) {
        HWND h = CreateCtrl(hwnd, L"BUTTON", label, x, *cursor, lw + 40, 22,
                            id, BS_AUTOCHECKBOX);
        SendMessage(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        *cursor += rowH_;
        return h;
    };

    // One RGB triple as three numeric fields plus a picker button.
    //
    // The legacy Zone Properties dialog used three HSCROLL sliders per colour,
    // which cannot express an exact value and gives no way to type one — so a
    // value round-tripped from a file could only be nudged, never entered. These
    // are plain fields (typeable, exact) with the picker as the convenience path.
    auto addColorRow = [&](const wchar_t* label, int idR, int idG, int idB,
                           int r, int g, int b,
                           int lx, int ly, int lw_, int ew_, int rowH_,
                           int rightEdge) {
        CreateLabel(hwnd, label, lx, ly, lw_, 20, 0);
        const int cw = 52;
        auto chan = [&](int id, int v, int off) {
            std::wstring wv = std::to_wstring(v);
            CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", wv.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                lx + lw_ + off, ly, cw, 22, hwnd, (HMENU)(INT_PTR)id, g_hInst, nullptr);
        };
        chan(idR, r, 0);
        chan(idG, g, cw + 4);
        chan(idB, b, (cw + 4) * 2);
        // Picker sits to the right of the three channels, or on the next line if
        // the value column is too narrow for it.
        const int px = lx + lw_ + (cw + 4) * 3 + 6;
        if (px + 70 <= rightEdge - 10) {
            CreateButton(hwnd, L"Pick...", px, ly, 70, 22, idR + 10000);
        }
        return px;
    };

    // Stat rows are the one place that shows BOTH the readable label and the raw
    // .ozls key, so they need more room than an instance row. The whole def block
    // (read-only dump below, PawnDefs rows and the schema rows) shares this width,
    // so the value column steps once, at the "Definition" header - not in the
    // middle of a section.
    const int statLabelW = 190;
    auto addDefRow = [&](const std::string& key, const std::string& val) {
        std::wstring wk(key.begin(), key.end());
        CreateLabel(hwnd, wk.c_str(), x, y, statLabelW, 20, 0);
        std::wstring wv(val.begin(), val.end());
        // No control ID: this is the panel's "a read-only row cannot be applied"
        // invariant. Apply reads editable stat rows by controlId and
        // readStatRowText() returns "" for a missing control, so giving a
        // read-only row an ID would let it be read - or worse, treated as blank.
        CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", wv.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
            x + statLabelW, y, rc.right - (x + statLabelW) - 20, 22,
            hwnd, nullptr, g_hInst, nullptr);
        y += rowH;
    };

    // Definition block (read-only, per-key rows) - shown for NPC/pickup/zone
    if (!g_editorPanels.propDefTitle.empty()) {
        addSection("Definition (read-only)");
        addDefRow("type", g_editorPanels.propDefTitle);
        if (!g_editorPanels.propDefSource.empty())
            addDefRow("source", g_editorPanels.propDefSource);
        if (!g_editorPanels.propDefPawnFields.empty()) {
            addSection("PawnDefs stats");
            for (auto& f : g_editorPanels.propDefPawnFields) addDefRow(f.key, f.value);
        }
        if (!g_editorPanels.propDefFields.empty()) {
            addSection(".ozls stats (keys with no edit field)");
            for (auto& f : g_editorPanels.propDefFields) addDefRow(f.key, f.value);
        }
        if (!g_editorPanels.propDefActions.empty()) {
            addSection("Actions");
            for (auto& f : g_editorPanels.propDefActions) addDefRow(f.key, f.value);
        }

        // Editable stat rows for the def's entity type. Separate from the
        // read-only dump above so the full authored set stays visible, including
        // keys this schema does not know about.
        if (!g_editorPanels.propDefEditable.empty()) {
            addSection(g_editorPanels.propDefWritable
                           ? "Edit stats (writes to .ozls on Apply)"
                           : "Stats (read-only: def is packaged)");
            if (!g_editorPanels.propDefWritable) {
                CreateLabel(hwnd,
                    L"Def has no editable source file. Edit the GameData .ozls and repack.",
                    x, y, rc.right - 20, 18, 0);
                y += 20;
            }
// statLabelW is declared above, shared with the read-only def rows.
        const bool defWritable = g_editorPanels.propDefWritable;
        std::string curGroup;
        for (const auto& row : g_editorPanels.propDefEditable) {
            // Group header whenever the schema moves to a new group.
            if (!row.group.empty() && row.group != curGroup) {
                curGroup = row.group;
                addSection(row.group.c_str());
            }
            // Label primary, key alongside: the label is what the author reads,
            // the key is what Apply writes, and showing only one of them has been
            // the source of both confusion ("which key is reload_time?") and
            // duplicated state (an authored label that was never displayed).
            std::string cap = row.label;
            if (!row.label.empty() && row.label != row.key)
                cap += "  (" + row.key + ")";
            else if (row.label.empty())
                cap = row.key;
            std::wstring wk(cap.begin(), cap.end());
            CreateLabel(hwnd, wk.c_str(), x, y, statLabelW, 20, 0);
            std::wstring wv(row.value.begin(), row.value.end());
            // ES_NUMBER would reject a vec3 "(0.1, 0.2, 0.3)" and a float field
            // must still allow a leading '-', so numeric validation is left to
            // Apply rather than enforced by the control.
            //
            // A read-only row gets NO control ID. That is load-bearing, not
            // tidiness: Apply's edit scan reads each row by controlId and
            // readStatRowText() returns "" for a missing control, so an ID-less
            // row would read as "" and be queued as an ERASE — a packaged def
            // would strip its own stats on the first Apply.
            // ES_RIGHT on the control rather than a later EM_SETALIGN message: EM_SETALIGN
            // is not declared by MinGW's richedit headers, and the style bit is
            // what EM_SETALIGN sets anyway. Hint only — numeric validation stays
            // in Apply (see below).
            CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", wv.c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL |
                    (defWritable ? WS_TABSTOP : ES_READONLY) |
                    (row.isFloat ? ES_RIGHT : 0),
                x + statLabelW, y, rc.right - (x + statLabelW) - 20, 22,
                hwnd, defWritable ? (HMENU)(INT_PTR)row.controlId : nullptr,
                g_hInst, nullptr);
            y += rowH;
            // Sound rows get Preview (play it) and Browse (pick a file).
            // Stacked under the field rather than beside it, because the
            // panel is narrow and a third column would squeeze the path.
            if (row.isSoundPath && defWritable) {
                const int idx = row.controlId - ID_PP_STAT_FIELD_0;
                CreateButton(hwnd, L"Preview", x + statLabelW, y, 70, 22,
                             ID_PP_STAT_PREVIEW_0 + idx);
                CreateButton(hwnd, L"Browse...", x + statLabelW + 76, y, 80, 22,
                             ID_PP_STAT_BROWSE_0 + idx);
                y += rowH;
            }
        }
        }   // end: !propDefEditable.empty()

        if (!g_editorPanels.propDefPath.empty()) {
            CreateButton(hwnd, L"Edit .ozls", x, y, 100, 24, ID_PP_EDITDEF);
            y += 30;
        }
    }

    if (selType == sel::NPC) {  // instance overrides
        addSection("Instance overrides");
        addField(L"Health:", ID_PP_HEALTH, g_editorPanels.propHealth);
        addField(L"Speed:", ID_PP_SPEED, g_editorPanels.propSpeed);
    } else if (selType == sel::LIGHT) {
        addSection("Light");
        addTextField(L"Name:", ID_PP_LIGHT_NAME, g_editorPanels.propLightName);
        // Type / Effect combos. The stored ints are the LitLightType /
        // LitLightEffect enum values and the combo order matches them.
        CreateLabel(hwnd, L"Type:", x, y, lw, 20, 0);
        {
            HWND hCombo = CreateWindowEx(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                x + lw, y, ew, 200, hwnd, (HMENU)(INT_PTR)ID_PP_LIGHT_TYPE, g_hInst, nullptr);
            const wchar_t* lt[] = { L"directional", L"point", L"spot" };
            for (auto* t : lt) SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)t);
            int lsel = g_editorPanels.propLightType;
            if (lsel < 0 || lsel > 2) lsel = 1;
            SendMessage(hCombo, CB_SETCURSEL, lsel, 0);
        }
        y += rowH;
        CreateLabel(hwnd, L"Effect:", x, y, lw, 20, 0);
        {
            HWND hCombo = CreateWindowEx(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                x + lw, y, ew, 200, hwnd, (HMENU)(INT_PTR)ID_PP_LIGHT_EFFECT, g_hInst, nullptr);
            const wchar_t* le[] = { L"none", L"watery", L"torch", L"fire", L"lamp" };
            for (auto* t : le) SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)t);
            int lsel = g_editorPanels.propLightEffect;
            if (lsel < 0 || lsel > 4) lsel = 0;
            SendMessage(hCombo, CB_SETCURSEL, lsel, 0);
        }
        y += rowH;
        addField(L"Red:", ID_PP_LIGHT_R, (float)g_editorPanels.propLightR);
        addField(L"Green:", ID_PP_LIGHT_G, (float)g_editorPanels.propLightG);
        addField(L"Blue:", ID_PP_LIGHT_B, (float)g_editorPanels.propLightB);
        addField(L"Intensity:", ID_PP_LIGHT_INTENS, g_editorPanels.propLightIntensity);
        addField(L"Radius:", ID_PP_LIGHT_RADIUS, g_editorPanels.propLightRadius);
        // Spot cone, edited in degrees because cos(half-angle) is unreadable.
        // LightNode stores the cosine for the shader; conversion is on apply.
        addField(L"Inner Angle:", ID_PP_LIGHT_INNER, g_editorPanels.propLightInnerAngle);
        addField(L"Outer Angle:", ID_PP_LIGHT_OUTER, g_editorPanels.propLightOuterAngle);
        // A directional light is authored by its source point and aimed at the
        // world origin, so a target row would be meaningless here (and is
        // forced back to the origin on apply).
        if (g_editorPanels.propLightType != (int)LitLightType::DIRECTIONAL) {
            addField(L"Target X:", ID_PP_LIGHT_TARGETX, g_editorPanels.propLightTarget[0]);
            addField(L"Target Y:", ID_PP_LIGHT_TARGETY, g_editorPanels.propLightTarget[1]);
            addField(L"Target Z:", ID_PP_LIGHT_TARGETZ, g_editorPanels.propLightTarget[2]);
        }
        CreateCtrl(hwnd, L"BUTTON", L"Lens Flare", x, y, 120, 22, ID_PP_LIGHT_FLARE, BS_AUTOCHECKBOX);
        CreateCtrl(hwnd, L"BUTTON", L"Corona", x + 124, y, 100, 22, ID_PP_LIGHT_CORONA, BS_AUTOCHECKBOX);
        SendMessage(GetDlgItem(hwnd, ID_PP_LIGHT_FLARE), BM_SETCHECK,
                    g_editorPanels.propLightFlare ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessage(GetDlgItem(hwnd, ID_PP_LIGHT_CORONA), BM_SETCHECK,
                    g_editorPanels.propLightCorona ? BST_CHECKED : BST_UNCHECKED, 0);
        y += rowH;
    } else if (selType == sel::PICKUP) {  // instance overrides
        addSection("Instance overrides");
        addField(L"Respawn:", ID_PP_RESPAWN, g_editorPanels.propRespawnTime);
    } else if (selType == sel::ZONE) {
        addSection("Zone");
        CreateLabel(hwnd, L"Type:", x, y, lw, 20, 0);
        {
            HWND hCombo = CreateWindowEx(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                x + lw, y, ew, 200, hwnd, (HMENU)(INT_PTR)ID_PP_ZONETYPE, g_hInst, nullptr);
            const wchar_t* zt[] = { L"water", L"ladder", L"sky", L"reverb", L"sound" };
            for (auto* t : zt) SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)t);
            int zsel = g_editorPanels.propZoneType;
            if (zsel < 0 || zsel > 4) zsel = 0;
            SendMessage(hCombo, CB_SETCURSEL, zsel, 0);
        }
        y += rowH;
        addField(L"Intensity:", ID_PP_ZONEINT, g_editorPanels.propZoneIntensity);
        addTextField(L"Name:", ID_PP_ZONENAME, g_editorPanels.propZoneName);
        // Per-zone physics overrides (round-trip through the OZONE export)
        addSection("Physics");
        addField(L"Gravity:", ID_PP_ZONEGRAV, g_editorPanels.propZoneGravity);
        addField(L"Jump:", ID_PP_ZONEJUMP, g_editorPanels.propZoneJump);
        addField(L"Terminal:", ID_PP_ZONETERM, g_editorPanels.propZoneTerminal);
        addField(L"Water Gravity:", ID_PP_ZONEWGRAV, g_editorPanels.propZoneWaterGravity);
        addField(L"Water Drag:", ID_PP_ZONEWDRAG, g_editorPanels.propZoneWaterDrag);
        addField(L"Swim Up:", ID_PP_ZONESWIM, g_editorPanels.propZoneSwimUp);
        addField(L"Ladder Speed:", ID_PP_ZONELADDER, g_editorPanels.propZoneLadderSpeed);
        addField(L"Fly Mult:", ID_PP_ZONEFLYMULT, g_editorPanels.propZoneFlyMult);

        // --- Environment overrides ---------------------------------------
        // These write zone.envOverrides, which is what ZoneManager merges into
        // PointRegion::combinedEnv and Core.hpp applies on zone entry. They are
        // NOT the old Zone Properties Fog/Ambient tabs: those poked a single
        // level-global shader uniform and their fogStart/fogEnd were never
        // consumed at all.
        addSection("Environment");
        addCheckBox(L"Override Fog", ID_PP_ZENV_FOG_ON,
                    g_editorPanels.propZoneApplyFog, &y, rowH);
        addColorRow(L"Fog Color:", ID_PP_ZENV_FOG_R, ID_PP_ZENV_FOG_G, ID_PP_ZENV_FOG_B,
                    g_editorPanels.propZoneFogR, g_editorPanels.propZoneFogG,
                    g_editorPanels.propZoneFogB, x, y, lw, ew, rowH, rc.right);
        addField(L"Fog Density:", ID_PP_ZENV_FOG_DENS, g_editorPanels.propZoneFogDensity);
        addField(L"Fog Start:", ID_PP_ZENV_FOG_START, g_editorPanels.propZoneFogStart);
        addField(L"Fog End:", ID_PP_ZENV_FOG_END, g_editorPanels.propZoneFogEnd);
        addCheckBox(L"Override Ambient", ID_PP_ZENV_AMB_ON,
                    g_editorPanels.propZoneApplyAmbient, &y, rowH);
        addColorRow(L"Ambient Color:", ID_PP_ZENV_AMB_R, ID_PP_ZENV_AMB_G, ID_PP_ZENV_AMB_B,
                    g_editorPanels.propZoneAmbR, g_editorPanels.propZoneAmbG,
                    g_editorPanels.propZoneAmbB, x, y, lw, ew, rowH, rc.right);
        addField(L"Ambient Intensity:", ID_PP_ZENV_AMB_INT, g_editorPanels.propZoneAmbIntensity);
        addSection("Reverb");
        addField(L"Mix:", ID_PP_ZENV_REVERB_MIX, g_editorPanels.propZoneReverbMix);
        addField(L"Decay:", ID_PP_ZENV_REVERB_DEC, g_editorPanels.propZoneReverbDecay);
    } else if (selType == sel::PORTAL) {
        addSection("Destination");
        addTextField(L"Target World:", ID_PP_PORTALWORLD, g_editorPanels.propPortalWorld);
        addField(L"Spawn X:", ID_PP_PSPAWNX, g_editorPanels.propPortalSpawn[0]);
        addField(L"Spawn Y:", ID_PP_PSPAWNY, g_editorPanels.propPortalSpawn[1]);
        addField(L"Spawn Z:", ID_PP_PSPAWNZ, g_editorPanels.propPortalSpawn[2]);
        {
            HWND hCheck = CreateWindowEx(0, L"BUTTON", L"Bidirectional",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                x, y, 200, 22, hwnd, (HMENU)(INT_PTR)ID_PP_PBIDIR, g_hInst, nullptr);
            SendMessage(hCheck, BM_SETCHECK, g_editorPanels.propPortalBidir ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        y += rowH;
        // Delete Portal. Added because the old Zone window owned the only
        // portal list; removing that window must not remove the ability to
        // remove a portal. propsTargetIndex is the GetPortals() index, which is
        // what actionDeletePortal consumes.
        CreateButton(hwnd, L"Delete Portal", x, y, 110, 22, ID_PP_PORTALDELETE);
        y += rowH;
    } else if (selType == sel::MESH) {
        addSection("Mesh");
        addTextField(L"Mesh Path:", ID_PP_MESHPATH, g_editorPanels.propMeshPath);
        addTextField(L"Texture:", ID_PP_MESHTEX, g_editorPanels.propMeshTex);
        // Pick a texture from the Texture Manager ("Use Active Tex") or the OS
        // file dialog ("Browse...") instead of typing/copying a path.
        CreateButton(hwnd, L"Browse Tex...", x, y, 90, 22, ID_PP_MESHTEX_BROWSE);
        CreateButton(hwnd, L"Use Active Tex", x + 96, y, 110, 22, ID_PP_MESHTEX_ACTIVE);
        y += rowH;
        addTextField(L"Anim Clip:", ID_PP_ANIMCLIP, g_editorPanels.propAnimClip);
        addTextField(L"Anim File:", ID_PP_MESHANIMFILE, g_editorPanels.propMeshAnimFile);
        CreateButton(hwnd, L"Browse Anim...", x, y, 110, 22, ID_PP_MESHANIMFILE_BROWSE);
        CreateButton(hwnd, L"Convert to Animated", x + 116, y, 150, 22, ID_PP_CONVERT_ANIMATED);
        y += rowH;
        addField(L"Anim Speed:", ID_PP_MESHANIMSPEED, g_editorPanels.propMeshAnimSpeed);
        addField(L"Scale:", ID_PP_SCALE, g_editorPanels.propScale);
        {
            HWND hCheck = CreateWindowEx(0, L"BUTTON", L"Wind Affected",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                x, y, 200, 22, hwnd, (HMENU)(INT_PTR)ID_PP_MESHWIND, g_hInst, nullptr);
            SendMessage(hCheck, BM_SETCHECK, g_editorPanels.propMeshWind ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        y += rowH;
        CreateButton(hwnd, L"Reload Mesh", x, y, 110, 24, ID_PP_MESHRELOAD);
        y += 30;
    } else if (selType == sel::PARTICLE) {
        addSection("Particle Emitter");
        addTextField(L"Type:", ID_PP_EMITTER_TYPE, g_editorPanels.propEmitterType);
        addTextField(L"Texture:", ID_PP_EMITTER_TEX, g_editorPanels.propEmitterTex);
        CreateButton(hwnd, L"Browse Tex...", x, y, 90, 22, ID_PP_EMITTERTEX_BROWSE);
        CreateButton(hwnd, L"Use Active Tex", x + 96, y, 110, 22, ID_PP_EMITTERTEX_ACTIVE);
        y += rowH;
        addField(L"Rate:", ID_PP_EMITTER_RATE, g_editorPanels.propEmitterRate);
        addField(L"Lifetime:", ID_PP_EMITTER_LIFE, g_editorPanels.propEmitterLife);
        addField(L"Speed:", ID_PP_EMITTER_SPEED, g_editorPanels.propEmitterSpeed);
        addField(L"Size:", ID_PP_EMITTER_SIZE, g_editorPanels.propEmitterSize);
        addField(L"Spread:", ID_PP_EMITTER_SPREAD, g_editorPanels.propEmitterSpread);
        addField(L"Gravity:", ID_PP_EMITTER_GRAVITY, g_editorPanels.propEmitterGravity);
        addField(L"Radius:", ID_PP_EMITTER_RADIUS, g_editorPanels.propEmitterRadius);
        addField(L"Color R:", ID_PP_EMITTER_R, (float)g_editorPanels.propEmitterR);
        addField(L"Color G:", ID_PP_EMITTER_G, (float)g_editorPanels.propEmitterG);
        addField(L"Color B:", ID_PP_EMITTER_B, (float)g_editorPanels.propEmitterB);
        addField(L"Size End:", ID_PP_EMITTER_SIZEEND, g_editorPanels.propEmitterSizeEnd);
        addField(L"End R:", ID_PP_EMITTER_REND, (float)g_editorPanels.propEmitterREnd);
        addField(L"End G:", ID_PP_EMITTER_GEND, (float)g_editorPanels.propEmitterGEnd);
        addField(L"End B:", ID_PP_EMITTER_BEND, (float)g_editorPanels.propEmitterBEnd);
        {
            // Unchecked draws the particle as a lit sphere; checked billboards it.
            HWND hChk = CreateWindowEx(0, L"BUTTON", L"Billboard",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                x, y, 200, 22, hwnd, (HMENU)(INT_PTR)ID_PP_EMITTER_BILLBOARD, g_hInst, nullptr);
            SendMessage(hChk, BM_SETCHECK,
                        g_editorPanels.propEmitterBillboard ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    } else if (selType == sel::PATHNODE) {
        addSection("Path Node");
        addTextField(L"Name:", ID_PP_PATHNAME, g_editorPanels.propPathName);
        addField(L"Radius:", ID_PP_PATHRADIUS, g_editorPanels.propPathRadius);
        addTextField(L"Next (a,b):", ID_PP_PATHNEXT, g_editorPanels.propPathNext);
        {
            HWND hCheck = CreateWindowEx(0, L"BUTTON", L"Loop",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                x, y, 200, 22, hwnd, (HMENU)(INT_PTR)ID_PP_PATHLOOP, g_hInst, nullptr);
            SendMessage(hCheck, BM_SETCHECK, g_editorPanels.propPathLoop ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        y += rowH;
    } else if (selType == sel::WINDZONE) {
        addSection("Wind Zone");
        addField(L"Size X:", ID_PP_WIND_SX, g_editorPanels.propWindSizeX);
        addField(L"Size Y:", ID_PP_WIND_SY, g_editorPanels.propWindSizeY);
        addField(L"Size Z:", ID_PP_WIND_SZ, g_editorPanels.propWindSizeZ);
        addField(L"Dir X:", ID_PP_WIND_DIRX, g_editorPanels.propWindDirX);
        addField(L"Dir Y:", ID_PP_WIND_DIRY, g_editorPanels.propWindDirY);
        addField(L"Dir Z:", ID_PP_WIND_DIRZ, g_editorPanels.propWindDirZ);
        addField(L"Strength:", ID_PP_WIND_STRENGTH, g_editorPanels.propWindStrength);
        addField(L"Frequency:", ID_PP_WIND_FREQ, g_editorPanels.propWindFrequency);
    } else if (selType == sel::MAP) {
        // The level itself. Replaces the Zone Properties dialog's GameType /
        // Particles / Skybox tabs, which were the only UI for this state.
        addSection("Level");

        // --- Game Mode -------------------------------------------------------
        CreateLabel(hwnd, L"Game Mode:", x, y, lw, 20, 0);
        {
            HWND hCombo = CreateWindowEx(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                x + lw, y, ew + 60, 240, hwnd,
                (HMENU)(INT_PTR)ID_PP_MAP_GAMETYPE, g_hInst, nullptr);
            // AllGameTypes() is ordered 0..N-1 with no gaps, so the combo index IS
            // the GameType value. That identity is what lets Apply do
            // (GameType)CB_GETCURSEL without a lookup table.
            for (oz::gametype::GameType t : oz::gametype::AllGameTypes()) {
                const char* label = oz::gametype::GameTypeName(t);
                if (!label) continue;
                std::wstring wlabel(label, label + strlen(label));
                SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)wlabel.c_str());
            }
            SendMessage(hCombo, CB_SETCURSEL, g_editorPanels.propMapGameType, 0);
        }
        y += rowH;
        addIntField(L"Max Players:", ID_PP_MAP_MAXPLAYERS, g_editorPanels.propMapMaxPlayers);
        addField(L"Respawn (s):", ID_PP_MAP_RESPAWN, g_editorPanels.propMapRespawnTime);
        addCheckBox(L"Time Limit Enabled", ID_PP_MAP_TIMELIMIT_ON,
                    g_editorPanels.propMapTimeLimitEnabled, &y, rowH);
        addField(L"Time Limit (min):", ID_PP_MAP_TIMELIMIT, g_editorPanels.propMapTimeLimitMinutes);
        addIntField(L"Score Limit:", ID_PP_MAP_SCORELIMIT, g_editorPanels.propMapScoreLimit);
        addCheckBox(L"Friendly Fire", ID_PP_MAP_FRIENDLY,
                    g_editorPanels.propMapFriendlyFire, &y, rowH);

        // Resolved-ruleset summary. Refreshed on every combo change via
        // RefreshMapRulesetSummary — the legacy Zone Properties copy of this label
        // was created once and never updated, despite a comment claiming it was.
        CreateLabel(hwnd, L"", x, y, rc.right - 20, 20, ID_PP_MAP_SUMMARY);
        RefreshMapRulesetSummary(hwnd);
        y += rowH;

        // --- Skybox ----------------------------------------------------------
        addSection("Skybox");
        addTextField(L"Texture:", ID_PP_MAP_SKYBOX, g_editorPanels.propMapSkybox);
        CreateButton(hwnd, L"Browse...", x + lw, y, 90, 22, ID_PP_MAP_SKYBOX_BROWSE);
        CreateButton(hwnd, L"Use Active Tex", x + lw + 96, y, 110, 22, ID_PP_MAP_SKYBOX_ACTIVE);
        y += rowH;
        addTextField(L"Side/Cap Tex:", ID_PP_MAP_SKYBOX_SIDE, g_editorPanels.propMapSkyboxSide);

        // --- Weather ---------------------------------------------------------
        addSection("Weather");
        CreateLabel(hwnd, L"Particle Type:", x, y, lw, 20, 0);
        {
            HWND hCombo = CreateWindowEx(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                x + lw, y, ew, 160, hwnd,
                (HMENU)(INT_PTR)ID_PP_MAP_PTYPE, g_hInst, nullptr);
            const wchar_t* pTypes[] = { L"None", L"Snow", L"Rain", L"Void Realm", L"Psychic Realm" };
            for (auto* pt : pTypes) SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)pt);
            int sel = g_editorPanels.propMapParticleType;
            if (sel < 0 || sel > 4) sel = 0;
            SendMessage(hCombo, CB_SETCURSEL, sel, 0);
        }
        y += rowH;
        addField(L"Density:", ID_PP_MAP_PDENSITY, g_editorPanels.propMapParticleDensity);
        addField(L"Speed:", ID_PP_MAP_PSPEED, g_editorPanels.propMapParticleSpeed);
        addColorRow(L"Fog/Color:", ID_PP_MAP_PR, ID_PP_MAP_PG, ID_PP_MAP_PB,
                    g_editorPanels.propMapParticleR, g_editorPanels.propMapParticleG,
                    g_editorPanels.propMapParticleB, x, y, lw, ew, rowH, rc.right);
        // Wind had NO control at all before (ID_SF_PAR_WINDX/Z were declared and
        // never created), so a wind value in a world file could only ever be
        // preserved, never authored.
        addField(L"Wind X:", ID_PP_MAP_PWINDX, g_editorPanels.propMapParticleWindX);
        addField(L"Wind Z:", ID_PP_MAP_PWINDZ, g_editorPanels.propMapParticleWindZ);
    }
    // ---------------------------------------------------------------------------

y += 8;
    CreateButton(hwnd, L"Apply", x, y, bw, 26, ID_PP_APPLY);
    CreateButton(hwnd, L"Close", x + bw + 6, y, bw, 26, ID_PP_CLOSE);

    // Capture the un-scrolled row positions, then size the window to the work
    // area and publish the scroll range instead of growing past the screen.
    PropsCaptureRows(hwnd);
    PropsFitWindow(hwnd, y + 26 + 24);  // buttons + bottom margin
}

static LRESULT CALLBACK PropsPanelProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        PopulatePropertiesPanel(hwnd);
        break;
    }
case WM_USER + 50: {
        PopulatePropertiesPanel(hwnd);
        break;
    }
    case WM_VSCROLL: {
        // The non-client scrollbar drives g_propsScroll; every child row is
        // then moved by the difference.
        SCROLLINFO si = { sizeof(SCROLLINFO), SIF_ALL };
        GetScrollInfo(hwnd, SB_VERT, &si);
        const int line = 24;   // one row
        const int page = si.nPage ? (int)si.nPage : 1;

        int delta = 0;
        switch (LOWORD(w)) {
            case SB_LINEUP:   delta = -line; break;
            case SB_LINEDOWN: delta =  line; break;
            case SB_PAGEUP:   delta = -page; break;
            case SB_PAGEDOWN: delta =  page; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: delta = (int)si.nTrackPos - g_propsScroll; break;
            case SB_TOP:      delta = -g_propsScroll; break;
            case SB_BOTTOM:   delta = PropsMaxScroll(hwnd) - g_propsScroll; break;
            default: break;
        }
        if (delta != 0) {
            g_propsScroll += delta;
            PropsApplyScroll(hwnd);
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        // Scrolling a long stat list with the wheel over the panel is far
        // quicker than dragging the (very short, content-capped) scrollbar.
        g_propsScroll -= GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 24;
        PropsApplyScroll(hwnd);
        break;
    }
    case WM_SIZE: {
        // The user resized the panel, so the page size changed: re-publish the
        // range and re-clamp. PropsFitWindow's own SetWindowPos lands here too.
        PropsUpdateScroll(hwnd);
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_PP_CLOSE) { ShowPropertiesPanel(false); break; }
        if (id == ID_PP_EDITDEF) {
            // Hand off to the OS editor (defs stay read-only in the panel)
            if (!g_editorPanels.propDefPath.empty())
                ShellExecuteA(hwnd, "open", g_editorPanels.propDefPath.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        if (id == ID_PP_MESHRELOAD) {
            // Carries the target so a reload cannot be applied to whatever is
            // selected by the time the frame drains the bus.
            ed::EventBus::instance().post(ed::Ev::ReloadMesh,
                ed::SelRef{ToBusKind((SelType)g_editorPanels.propsTargetType),
                           g_editorPanels.propsTargetIndex});
            break;
        }
        if (id == ID_PP_PORTALDELETE) {
            // propsTargetIndex is already the GetPortals() index. Main.cpp's handler
            // calls HistoryPush() itself, so this must not push undo here.
            if (g_editorPanels.propsTargetIndex >= 0)
                ed::EventBus::instance().post(ed::Ev::DeletePortal,
                    ed::SelRef{ed::SelKind::Portal, g_editorPanels.propsTargetIndex});
            break;
        }
        // Texture pickers for mesh / particle-emitter properties â€” pick from the
        // Texture Manager ("Use Active Tex") or the OS dialog ("Browse Tex...")
        // instead of hand-copying a path, then apply immediately.
        if (id == ID_PP_MESHANIMFILE_BROWSE) {
            std::string path;
            if (ChooseAnimFile(path)) {
                std::wstring w(path.begin(), path.end());
                SetWindowTextW(GetDlgItem(hwnd, ID_PP_MESHANIMFILE), w.c_str());
                g_editorPanels.propMeshAnimFile = path;
                PostApplyProperties();
            }
            break;
        }
        if (id == ID_PP_CONVERT_ANIMATED) {
            // Capture the panel's own target, not whatever is selected by the time
            // the frame drains - the button lives on this panel for this entity.
            ed::EventBus::instance().post(ed::Ev::ConvertToAnimated,
                ed::SelRef{ToBusKind((SelType)g_editorPanels.propsTargetType),
                           g_editorPanels.propsTargetIndex});
            break;
        }
        if (id == ID_PP_MESHTEX_BROWSE || id == ID_PP_EMITTERTEX_BROWSE ||
            id == ID_PP_MESHTEX_ACTIVE || id == ID_PP_EMITTERTEX_ACTIVE) {
            bool mesh = (id == ID_PP_MESHTEX_BROWSE || id == ID_PP_MESHTEX_ACTIVE);
            bool active = (id == ID_PP_MESHTEX_ACTIVE || id == ID_PP_EMITTERTEX_ACTIVE);
            std::string path = active ? g_editorPanels.activeTexturePath : std::string();
            if (!active && !ChooseImageFile(path)) break;
            if (path.empty()) break;
            int field = mesh ? ID_PP_MESHTEX : ID_PP_EMITTER_TEX;
            std::wstring w(path.begin(), path.end());
            SetWindowTextW(GetDlgItem(hwnd, field), w.c_str());
            if (mesh) g_editorPanels.propMeshTex = path;
            else      g_editorPanels.propEmitterTex = path;
            PostApplyProperties();
            break;
        }
        // --- SelType::MAP controls ---------------------------------------------
        // Game Mode change: refresh the resolved-ruleset summary, otherwise it
        // keeps describing the mode the panel was opened with. (The removed Zone
        // Properties copy of this label was created once and never updated,
        // despite a comment claiming it refreshed on combo change.)
        if (id == ID_PP_MAP_GAMETYPE) {
            if (HWND h = GetDlgItem(hwnd, ID_PP_MAP_GAMETYPE)) {
                int s = (int)SendMessage(h, CB_GETCURSEL, 0, 0);
                if (s >= 0) g_editorPanels.propMapGameType = s;
            }
            RefreshMapRulesetSummary(hwnd);
            break;
        }
        if (id == ID_PP_MAP_SKYBOX_BROWSE || id == ID_PP_MAP_SKYBOX_ACTIVE) {
            bool useActive = (id == ID_PP_MAP_SKYBOX_ACTIVE);
            std::string path = useActive ? g_editorPanels.activeTexturePath : std::string();
            if (useActive && path.empty()) {
                OZ_WARN("Map properties: no active texture in the Texture Manager");
                break;
            }
            if (!useActive && !ChooseSkyboxFile(path)) break;
            if (path.empty()) break;
            // Normalised to a repo-relative GameData/ path by ChooseImageFile; trim
            // the field to the same so what is shown is what will be written.
            std::wstring w(path.begin(), path.end());
            SetWindowTextW(GetDlgItem(hwnd, ID_PP_MAP_SKYBOX), w.c_str());
            g_editorPanels.propMapSkybox = path;
            // Take effect immediately, as the legacy Browse handler did: the skybox
            // is read live by the viewport pass, so waiting for Apply would look
            // broken even though the value is stored.
            LevelMetadata meta = GetLevelMetadata();
            meta.skyboxTexturePath = path;
            SetLevelMetadata(meta);
            break;
        }
        // Colour picker buttons are created as (idR + 10000) by addColorRow.
        if (id >= 10000 && id < 11000) {
            const int base = id - 10000;
            HWND hr = GetDlgItem(hwnd, base);
            HWND hg = GetDlgItem(hwnd, base + 1);
            HWND hb = GetDlgItem(hwnd, base + 2);
            if (!hr || !hg || !hb) break;
            wchar_t wbuf[32];
            GetWindowTextW(hr, wbuf, 32); int r = _wtoi(wbuf);
            GetWindowTextW(hg, wbuf, 32); int g = _wtoi(wbuf);
            GetWindowTextW(hb, wbuf, 32); int b = _wtoi(wbuf);
            if (!ChooseColorRGB(r, g, b)) break;
            swprintf(wbuf, 32, L"%d", r); SetWindowTextW(hr, wbuf);
            swprintf(wbuf, 32, L"%d", g); SetWindowTextW(hg, wbuf);
            swprintf(wbuf, 32, L"%d", b); SetWindowTextW(hb, wbuf);
            break;
        }
        // Preview / Browse for a sound row. Each row gets its OWN pair of control IDs
        // rather than sharing two and hit-testing the cursor: a shared ID needs
        // the row geometry recomputed here, and any drift from the layout would
        // silently act on the wrong weapon's sound.
        // The range test covered ONLY the Preview IDs. ID_PP_STAT_BROWSE_0 is 587, so the
        // whole Browse range fell outside it and the ChooseSoundFile path below was
        // unreachable — every "Browse..." on a sound stat row was a dead button.
        // Two tests, both anchored on the base they belong to, with the row index
        // derived from whichever range matched.
        const bool isStatPreview = (id >= ID_PP_STAT_PREVIEW_0 &&
                                    id <  ID_PP_STAT_PREVIEW_0 + ID_PP_STAT_MAX);
        const bool isStatBrowse  = (id >= ID_PP_STAT_BROWSE_0 &&
                                    id <  ID_PP_STAT_BROWSE_0 + ID_PP_STAT_MAX);
        if (isStatPreview || isStatBrowse) {
            const int index = isStatPreview ? (id - ID_PP_STAT_PREVIEW_0)
                                            : (id - ID_PP_STAT_BROWSE_0);
            if (index < 0 || index >= (int)g_editorPanels.propDefEditable.size()) break;
            const auto& row = g_editorPanels.propDefEditable[(size_t)index];

            if (isStatPreview) {
                const std::string path = readStatRowText(hwnd, row.controlId);
                if (path.empty()) break;
                g_editorPanels.propDefPreviewSound = path;
                break;
            }
            std::string chosen;
            if (!ChooseSoundFile(chosen)) break;
            std::wstring w(chosen.begin(), chosen.end());
            SetWindowTextW(GetDlgItem(hwnd, row.controlId), w.c_str());
            break;
        }
        if (id == ID_PP_APPLY) {
            // Read all edit fields and set action flags
            auto readFloat = [hwnd](int id, float def) -> float {
                HWND hCtrl = GetDlgItem(hwnd, id);
                if (!hCtrl) return def;
                wchar_t buf[64];
                GetWindowTextW(hCtrl, buf, 64);
                return (float)wcstod(buf, nullptr);
            };
            auto readString = [hwnd](int id, const std::string& def) -> std::string {
                HWND hCtrl = GetDlgItem(hwnd, id);
                if (!hCtrl) return def;
                wchar_t buf[256];
                GetWindowTextW(hCtrl, buf, 256);
                char out[256] = {0};
                WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, 256, nullptr, nullptr);
                return std::string(out);
            };
            g_editorPanels.propPosX = readFloat(ID_PP_POSX, 0);
            g_editorPanels.propPosY = readFloat(ID_PP_POSY, 0);
            g_editorPanels.propPosZ = readFloat(ID_PP_POSZ, 0);
            g_editorPanels.propRotation = readFloat(ID_PP_ROT, 0);
            g_editorPanels.propSizeX = readFloat(ID_PP_SX, 1);
            g_editorPanels.propSizeY = readFloat(ID_PP_SY, 1);
            g_editorPanels.propSizeZ = readFloat(ID_PP_SZ, 1);
            g_editorPanels.propTexScaleU = readFloat(ID_PP_TEX_SCALE_U, 1.0f);
            g_editorPanels.propTexScaleV = readFloat(ID_PP_TEX_SCALE_V, 1.0f);
            g_editorPanels.propTexOffsetU = readFloat(ID_PP_TEX_OFF_U, 0.0f);
            g_editorPanels.propTexOffsetV = readFloat(ID_PP_TEX_OFF_V, 0.0f);
            // Def-aligned instance overrides
            g_editorPanels.propHealth = readFloat(ID_PP_HEALTH, g_editorPanels.propHealth);
            g_editorPanels.propSpeed = readFloat(ID_PP_SPEED, g_editorPanels.propSpeed);
            g_editorPanels.propRespawnTime = readFloat(ID_PP_RESPAWN, g_editorPanels.propRespawnTime);
    g_editorPanels.propZoneIntensity = readFloat(ID_PP_ZONEINT, g_editorPanels.propZoneIntensity);
    g_editorPanels.propZoneName = readString(ID_PP_ZONENAME, g_editorPanels.propZoneName);
    g_editorPanels.propZoneGravity = readFloat(ID_PP_ZONEGRAV, g_editorPanels.propZoneGravity);
    g_editorPanels.propZoneJump = readFloat(ID_PP_ZONEJUMP, g_editorPanels.propZoneJump);
    g_editorPanels.propZoneTerminal = readFloat(ID_PP_ZONETERM, g_editorPanels.propZoneTerminal);
    g_editorPanels.propZoneWaterGravity = readFloat(ID_PP_ZONEWGRAV, g_editorPanels.propZoneWaterGravity);
    g_editorPanels.propZoneWaterDrag = readFloat(ID_PP_ZONEWDRAG, g_editorPanels.propZoneWaterDrag);
    g_editorPanels.propZoneSwimUp = readFloat(ID_PP_ZONESWIM, g_editorPanels.propZoneSwimUp);
    g_editorPanels.propZoneLadderSpeed = readFloat(ID_PP_ZONELADDER, g_editorPanels.propZoneLadderSpeed);
    g_editorPanels.propZoneFlyMult = readFloat(ID_PP_ZONEFLYMULT, g_editorPanels.propZoneFlyMult);
            // Zone environment overrides
            {
                auto& P = g_editorPanels;
                if (HWND h = GetDlgItem(hwnd, ID_PP_ZENV_FOG_ON))
                    P.propZoneApplyFog = SendMessage(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
                P.propZoneFogR       = (int)readFloat(ID_PP_ZENV_FOG_R, (float)P.propZoneFogR);
                P.propZoneFogG       = (int)readFloat(ID_PP_ZENV_FOG_G, (float)P.propZoneFogG);
                P.propZoneFogB       = (int)readFloat(ID_PP_ZENV_FOG_B, (float)P.propZoneFogB);
                P.propZoneFogDensity = readFloat(ID_PP_ZENV_FOG_DENS, P.propZoneFogDensity);
                P.propZoneFogStart   = readFloat(ID_PP_ZENV_FOG_START, P.propZoneFogStart);
                P.propZoneFogEnd     = readFloat(ID_PP_ZENV_FOG_END, P.propZoneFogEnd);
                if (HWND h = GetDlgItem(hwnd, ID_PP_ZENV_AMB_ON))
                    P.propZoneApplyAmbient = SendMessage(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
                P.propZoneAmbR         = (int)readFloat(ID_PP_ZENV_AMB_R, (float)P.propZoneAmbR);
                P.propZoneAmbG         = (int)readFloat(ID_PP_ZENV_AMB_G, (float)P.propZoneAmbG);
                P.propZoneAmbB         = (int)readFloat(ID_PP_ZENV_AMB_B, (float)P.propZoneAmbB);
                P.propZoneAmbIntensity = readFloat(ID_PP_ZENV_AMB_INT, P.propZoneAmbIntensity);
                P.propZoneReverbMix    = readFloat(ID_PP_ZENV_REVERB_MIX, P.propZoneReverbMix);
                P.propZoneReverbDecay  = readFloat(ID_PP_ZENV_REVERB_DEC, P.propZoneReverbDecay);
                auto cl = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
                P.propZoneFogR = cl(P.propZoneFogR);
                P.propZoneFogG = cl(P.propZoneFogG);
                P.propZoneFogB = cl(P.propZoneFogB);
                P.propZoneAmbR = cl(P.propZoneAmbR);
                P.propZoneAmbG = cl(P.propZoneAmbG);
                P.propZoneAmbB = cl(P.propZoneAmbB);
                // A fog range with end <= start divides by (end - start) in the
                // shader, producing inf/NaN. Nudge instead of exporting garbage.
                if (P.propZoneFogEnd <= P.propZoneFogStart)
                    P.propZoneFogEnd = P.propZoneFogStart + 1.0f;
                if (P.propZoneFogDensity < 0.0f) P.propZoneFogDensity = 0.0f;
                if (P.propZoneAmbIntensity < 0.0f) P.propZoneAmbIntensity = 0.0f;
                if (P.propZoneReverbMix < 0.0f) P.propZoneReverbMix = 0.0f;
                if (P.propZoneReverbDecay < 0.0f) P.propZoneReverbDecay = 0.0f;
            }
            g_editorPanels.propPortalWorld = readString(ID_PP_PORTALWORLD, g_editorPanels.propPortalWorld);
            g_editorPanels.propPortalSpawn[0] = readFloat(ID_PP_PSPAWNX, g_editorPanels.propPortalSpawn[0]);
            g_editorPanels.propPortalSpawn[1] = readFloat(ID_PP_PSPAWNY, g_editorPanels.propPortalSpawn[1]);
            g_editorPanels.propPortalSpawn[2] = readFloat(ID_PP_PSPAWNZ, g_editorPanels.propPortalSpawn[2]);
            if (HWND hz = GetDlgItem(hwnd, ID_PP_ZONETYPE)) {
                int s = (int)SendMessage(hz, CB_GETCURSEL, 0, 0);
                if (s >= 0 && s <= 4) g_editorPanels.propZoneType = s;
            }
            if (HWND hb = GetDlgItem(hwnd, ID_PP_PBIDIR))
                g_editorPanels.propPortalBidir =
                    SendMessage(hb, BM_GETCHECK, 0, 0) == BST_CHECKED;

            // --- SelType::MAP (the level) --------------------------------------
            // Every reader keeps its current value as the default, so a missing
            // control (wrong selection type) leaves the field untouched rather
            // than zeroing it.
            {
                auto& P = g_editorPanels;
                if (HWND h = GetDlgItem(hwnd, ID_PP_MAP_GAMETYPE)) {
                    int s = (int)SendMessage(h, CB_GETCURSEL, 0, 0);
                    if (s >= 0) P.propMapGameType = s;
                }
                P.propMapMaxPlayers       = (int)readFloat(ID_PP_MAP_MAXPLAYERS, (float)P.propMapMaxPlayers);
                P.propMapRespawnTime      = readFloat(ID_PP_MAP_RESPAWN, P.propMapRespawnTime);
                if (HWND h = GetDlgItem(hwnd, ID_PP_MAP_TIMELIMIT_ON))
                    P.propMapTimeLimitEnabled = SendMessage(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
                P.propMapTimeLimitMinutes = readFloat(ID_PP_MAP_TIMELIMIT, P.propMapTimeLimitMinutes);
                P.propMapScoreLimit       = (int)readFloat(ID_PP_MAP_SCORELIMIT, (float)P.propMapScoreLimit);
                if (HWND h = GetDlgItem(hwnd, ID_PP_MAP_FRIENDLY))
                    P.propMapFriendlyFire = SendMessage(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
                P.propMapSkybox     = readString(ID_PP_MAP_SKYBOX, P.propMapSkybox);
                P.propMapSkyboxSide = readString(ID_PP_MAP_SKYBOX_SIDE, P.propMapSkyboxSide);
                if (HWND h = GetDlgItem(hwnd, ID_PP_MAP_PTYPE)) {
                    int s = (int)SendMessage(h, CB_GETCURSEL, 0, 0);
                    if (s >= 0 && s <= 4) P.propMapParticleType = s;
                }
                P.propMapParticleDensity = readFloat(ID_PP_MAP_PDENSITY, P.propMapParticleDensity);
                P.propMapParticleSpeed   = readFloat(ID_PP_MAP_PSPEED, P.propMapParticleSpeed);
                P.propMapParticleR = (int)readFloat(ID_PP_MAP_PR, (float)P.propMapParticleR);
                P.propMapParticleG = (int)readFloat(ID_PP_MAP_PG, (float)P.propMapParticleG);
                P.propMapParticleB = (int)readFloat(ID_PP_MAP_PB, (float)P.propMapParticleB);
                P.propMapParticleWindX = readFloat(ID_PP_MAP_PWINDX, P.propMapParticleWindX);
                P.propMapParticleWindZ = readFloat(ID_PP_MAP_PWINDZ, P.propMapParticleWindZ);
                // Clamp the channels here rather than in the exporter: the colour
                // also feeds the level preview, and a negative value would reach
                // SetShaderValue as an int cast to float.
                auto cl = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
                P.propMapParticleR = cl(P.propMapParticleR);
                P.propMapParticleG = cl(P.propMapParticleG);
                P.propMapParticleB = cl(P.propMapParticleB);
                if (P.propMapMaxPlayers < 1) P.propMapMaxPlayers = 1;
                if (P.propMapScoreLimit < 0) P.propMapScoreLimit = 0;
                if (P.propMapRespawnTime < 0.0f) P.propMapRespawnTime = 0.0f;
                if (P.propMapParticleDensity < 0.0f) P.propMapParticleDensity = 0.0f;
                if (P.propMapParticleSpeed < 0.0f) P.propMapParticleSpeed = 0.0f;
            }

            // Collect editable .ozls stat edits. Only rows whose text actually
            // CHANGED are queued: a row that was never authored starts empty,
            // and writing those empties would erase nothing while still
            // producing a spurious file edit on every Apply.
            //
            // Gated on writability. A read-only stat row is created with NO
            // control ID, so readStatRowText() would return "" for it, compare
            // unequal to its authored value, and queue an erase — the exact
            // "Apply stripped the def" failure. For a packaged def there is no
            // file to write anyway.
            g_editorPanels.propDefPendingEdits.clear();
            if (g_editorPanels.propDefWritable) {
                for (const auto& row : g_editorPanels.propDefEditable) {
                    const std::string now = readStatRowText(hwnd, row.controlId);
                    // Trim trailing spaces so a stray keystroke does not register
                    // as a change.
                    std::string trimmed = now;
                    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
                        trimmed.pop_back();
                    if (trimmed == row.value) continue;
                    g_editorPanels.propDefPendingEdits.push_back({row.key, trimmed});
                }
            }
            // GameEngine.Mesh object edits
            g_editorPanels.propScale = readFloat(ID_PP_SCALE, g_editorPanels.propScale);
            g_editorPanels.propMeshPath = readString(ID_PP_MESHPATH, g_editorPanels.propMeshPath);
            g_editorPanels.propMeshTex = readString(ID_PP_MESHTEX, g_editorPanels.propMeshTex);
            g_editorPanels.propAnimClip = readString(ID_PP_ANIMCLIP, g_editorPanels.propAnimClip);
            g_editorPanels.propMeshAnimFile = readString(ID_PP_MESHANIMFILE, g_editorPanels.propMeshAnimFile);
            g_editorPanels.propMeshAnimSpeed = readFloat(ID_PP_MESHANIMSPEED, g_editorPanels.propMeshAnimSpeed);
            // GameEngine.ParticleEmitter edits
            g_editorPanels.propEmitterType = readString(ID_PP_EMITTER_TYPE, g_editorPanels.propEmitterType);
            g_editorPanels.propEmitterTex = readString(ID_PP_EMITTER_TEX, g_editorPanels.propEmitterTex);
            g_editorPanels.propEmitterRate = readFloat(ID_PP_EMITTER_RATE, g_editorPanels.propEmitterRate);
            g_editorPanels.propEmitterLife = readFloat(ID_PP_EMITTER_LIFE, g_editorPanels.propEmitterLife);
            g_editorPanels.propEmitterSpeed = readFloat(ID_PP_EMITTER_SPEED, g_editorPanels.propEmitterSpeed);
            g_editorPanels.propEmitterSize = readFloat(ID_PP_EMITTER_SIZE, g_editorPanels.propEmitterSize);
            g_editorPanels.propEmitterSpread = readFloat(ID_PP_EMITTER_SPREAD, g_editorPanels.propEmitterSpread);
            g_editorPanels.propEmitterR = (int)readFloat(ID_PP_EMITTER_R, (float)g_editorPanels.propEmitterR);
            g_editorPanels.propEmitterG = (int)readFloat(ID_PP_EMITTER_G, (float)g_editorPanels.propEmitterG);
            g_editorPanels.propEmitterB = (int)readFloat(ID_PP_EMITTER_B, (float)g_editorPanels.propEmitterB);
            g_editorPanels.propEmitterSizeEnd = readFloat(ID_PP_EMITTER_SIZEEND, g_editorPanels.propEmitterSizeEnd);
            g_editorPanels.propEmitterGravity = readFloat(ID_PP_EMITTER_GRAVITY, g_editorPanels.propEmitterGravity);
            g_editorPanels.propEmitterRadius = readFloat(ID_PP_EMITTER_RADIUS, g_editorPanels.propEmitterRadius);
            g_editorPanels.propEmitterREnd = (int)readFloat(ID_PP_EMITTER_REND, (float)g_editorPanels.propEmitterREnd);
            g_editorPanels.propEmitterGEnd = (int)readFloat(ID_PP_EMITTER_GEND, (float)g_editorPanels.propEmitterGEnd);
            g_editorPanels.propEmitterBEnd = (int)readFloat(ID_PP_EMITTER_BEND, (float)g_editorPanels.propEmitterBEnd);
            {
                HWND hChk = GetDlgItem(hwnd, ID_PP_EMITTER_BILLBOARD);
                if (hChk)
                    g_editorPanels.propEmitterBillboard =
                        (SendMessage(hChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
            }
            // GameEngine.PathNode edits
            g_editorPanels.propPathName = readString(ID_PP_PATHNAME, g_editorPanels.propPathName);
            g_editorPanels.propPathRadius = readFloat(ID_PP_PATHRADIUS, g_editorPanels.propPathRadius);
            g_editorPanels.propPathNext = readString(ID_PP_PATHNEXT, g_editorPanels.propPathNext);
            if (HWND hb = GetDlgItem(hwnd, ID_PP_PATHLOOP))
                g_editorPanels.propPathLoop =
                    SendMessage(hb, BM_GETCHECK, 0, 0) == BST_CHECKED;
            // GameEngine.Mesh wind flag
            if (HWND hb = GetDlgItem(hwnd, ID_PP_MESHWIND))
                g_editorPanels.propMeshWind =
                    SendMessage(hb, BM_GETCHECK, 0, 0) == BST_CHECKED;
            // WindZone edits
            g_editorPanels.propWindSizeX = readFloat(ID_PP_WIND_SX, g_editorPanels.propWindSizeX);
            g_editorPanels.propWindSizeY = readFloat(ID_PP_WIND_SY, g_editorPanels.propWindSizeY);
            g_editorPanels.propWindSizeZ = readFloat(ID_PP_WIND_SZ, g_editorPanels.propWindSizeZ);
            g_editorPanels.propWindDirX = readFloat(ID_PP_WIND_DIRX, g_editorPanels.propWindDirX);
            g_editorPanels.propWindDirY = readFloat(ID_PP_WIND_DIRY, g_editorPanels.propWindDirY);
            g_editorPanels.propWindDirZ = readFloat(ID_PP_WIND_DIRZ, g_editorPanels.propWindDirZ);
            g_editorPanels.propWindStrength = readFloat(ID_PP_WIND_STRENGTH, g_editorPanels.propWindStrength);
            g_editorPanels.propWindFrequency = readFloat(ID_PP_WIND_FREQ, g_editorPanels.propWindFrequency);
            // GameEngine.Light edits. Defaults fall back to the current panel
            // values so a partially-visible section (directional hides the
            // target rows) cannot zero a field that was never shown.
            g_editorPanels.propLightName = readString(ID_PP_LIGHT_NAME, g_editorPanels.propLightName);
            g_editorPanels.propLightR = (int)readFloat(ID_PP_LIGHT_R, (float)g_editorPanels.propLightR);
            g_editorPanels.propLightG = (int)readFloat(ID_PP_LIGHT_G, (float)g_editorPanels.propLightG);
            g_editorPanels.propLightB = (int)readFloat(ID_PP_LIGHT_B, (float)g_editorPanels.propLightB);
            g_editorPanels.propLightIntensity = readFloat(ID_PP_LIGHT_INTENS, g_editorPanels.propLightIntensity);
            g_editorPanels.propLightRadius = readFloat(ID_PP_LIGHT_RADIUS, g_editorPanels.propLightRadius);
            g_editorPanels.propLightInnerAngle = readFloat(ID_PP_LIGHT_INNER, g_editorPanels.propLightInnerAngle);
            g_editorPanels.propLightOuterAngle = readFloat(ID_PP_LIGHT_OUTER, g_editorPanels.propLightOuterAngle);
            g_editorPanels.propLightTarget[0] = readFloat(ID_PP_LIGHT_TARGETX, g_editorPanels.propLightTarget[0]);
            g_editorPanels.propLightTarget[1] = readFloat(ID_PP_LIGHT_TARGETY, g_editorPanels.propLightTarget[1]);
            g_editorPanels.propLightTarget[2] = readFloat(ID_PP_LIGHT_TARGETZ, g_editorPanels.propLightTarget[2]);
            if (HWND hz = GetDlgItem(hwnd, ID_PP_LIGHT_TYPE)) {
                int s = (int)SendMessage(hz, CB_GETCURSEL, 0, 0);
                if (s >= 0 && s <= 2) g_editorPanels.propLightType = s;
            }
            if (HWND hz = GetDlgItem(hwnd, ID_PP_LIGHT_EFFECT)) {
                int s = (int)SendMessage(hz, CB_GETCURSEL, 0, 0);
                if (s >= 0 && s <= 4) g_editorPanels.propLightEffect = s;
            }
            if (HWND hb = GetDlgItem(hwnd, ID_PP_LIGHT_FLARE))
                g_editorPanels.propLightFlare =
                    SendMessage(hb, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (HWND hb = GetDlgItem(hwnd, ID_PP_LIGHT_CORONA))
                g_editorPanels.propLightCorona =
                    SendMessage(hb, BM_GETCHECK, 0, 0) == BST_CHECKED;
            PostApplyProperties();
        }
        break;
    }
    case WM_CLOSE: ShowPropertiesPanel(false); break;
    case WM_DESTROY: g_editorPanels.hPropsPanel = nullptr; break;
    default: return DefWindowProc(hwnd, msg, w, l);
    }
    return 0;
}


// Fill the structured def rows from the registry by def name (+ a PawnDefs/*.cfg
// fallback for pawns without an .ozls def).
//
// File scope rather than a lambda inside ShowPropertiesPanel, because the Script
// Manager's Properties button needs it too - that is the only route to a def
// with no world instance (Player.ozls). Two copies would drift.
static void FillDefBlock(const std::string& defName, const std::string& fallbackPath) {
    using DefField = EditorPanelState::DefField;
    auto& P = g_editorPanels;
    P.propDefPath.clear(); P.propDefTitle.clear(); P.propDefSource.clear();
    P.propDefPawnFields.clear(); P.propDefFields.clear(); P.propDefActions.clear();
    P.propDefEditable.clear(); P.propDefPendingEdits.clear();
    P.propDefPreviewSound.clear();

    // A def that came from a package has no editable source file, so the panel
    // shows its stats read-only instead of accepting edits it cannot save. (Same
    // reasoning as the Script Manager's "edit the source and repack" message.)
    P.propDefWritable = false;

    auto addPawnDefFields = [&](const std::string& name) {
        for (const auto& pd : PawnSystem::Instance().GetDefs()) {
            if (pd.name != name) continue;
            P.propDefPawnFields.push_back({"speed", FormatStat(pd.speed)});
            P.propDefPawnFields.push_back({"aggroRange", FormatStat(pd.aggroRange)});
            P.propDefPawnFields.push_back({"attackRange", FormatStat(pd.attackRange)});
            P.propDefPawnFields.push_back({"damage", FormatStat(pd.damage)});
            P.propDefPawnFields.push_back({"maxHealth", std::to_string(pd.maxHealth)});
            if (!pd.sprite_path.empty()) P.propDefPawnFields.push_back({"sprite", pd.sprite_path});
            if (!pd.scream_path.empty()) P.propDefPawnFields.push_back({"scream", pd.scream_path});
            return true;
        }
        return false;
    };

    const EntityDef* def = LightningEntityRegistry::Instance().Find(defName);
    if (def) {
        P.propDefPath = def->sourcePath;
        P.propDefTitle = def->name + "  [" + EntityTypeName(def->type) + "]";
        P.propDefSource = def->sourcePath;
        if (!def->mesh.empty())    P.propDefFields.push_back({"mesh", def->mesh});
        if (!def->texture.empty()) P.propDefFields.push_back({"texture", def->texture});
        if (!def->icon.empty())    P.propDefFields.push_back({"icon", def->icon});
        if (!def->skybox.empty())  P.propDefFields.push_back({"skybox", def->skybox});
        if (!def->music.empty())   P.propDefFields.push_back({"music", def->music});

        std::vector<std::pair<std::string, float>> sv(def->stats.floats.begin(), def->stats.floats.end());
        std::sort(sv.begin(), sv.end());
        for (auto& [k, v] : sv) P.propDefFields.push_back({k, FormatStat(v)});
        std::vector<std::pair<std::string, std::string>> ss(def->stats.strings.begin(), def->stats.strings.end());
        std::sort(ss.begin(), ss.end());
        for (auto& [k, v] : ss) P.propDefFields.push_back({k, "\"" + v + "\""});
        for (const auto& a : def->actions)
            P.propDefActions.push_back({a.name, std::to_string(a.scriptLines.size()) + " lines"});
        if (def->type == EntityType::PAWN) addPawnDefFields(def->name);

        // Build the editable row set from the SCHEMA rather than from the
        // authored keys, so a weapon gets every documented stat including ones it
        // has never set - otherwise the panel could only ever edit what already
        // exists. Unauthored rows render blank, and Main.cpp only writes a key the
        // user actually typed into.
        P.propDefWritable = !P.propDefPath.empty() && fs::exists(P.propDefPath);

        int schemaCount = 0;
        const StatSpec* schema = StatSchemaFor(def->type, &schemaCount);
        // Player.ozls is declared `: upgrade`, so the player stat list is chosen
        // by name rather than by an entity type token.
        if (def->name == "Player") {
            schema = kPlayerStats;
            schemaCount = (int)(sizeof(kPlayerStats) / sizeof(StatSpec));
        }
        // Fill the rows whenever there IS a schema, regardless of writability. It used to
        // be gated on `&& P.propDefWritable`, which meant a packaged def left
        // propDefEditable empty — and since the panel draws its Edit section from
        // that vector, the "read-only: def is packaged" branch could never be
        // reached and a packaged def's stats were not shown at all.
        if (schema) {
            int row = 0;
            for (int i = 0; i < schemaCount && row < ID_PP_STAT_MAX; i++) {
                const StatSpec& sp = schema[i];
                std::string value;
                auto fit = def->stats.floats.find(sp.key);
                if (fit != def->stats.floats.end()) {
                    value = FormatStat(fit->second);
                } else {
                    auto sit = def->stats.strings.find(sp.key);
                    if (sit != def->stats.strings.end()) {
                        value = sit->second;
                    } else {
                        auto vit = def->stats.vec3s.find(sp.key);
                        if (vit != def->stats.vec3s.end())
                            value = "(" + FormatStat(vit->second[0]) + ", " +
                                    FormatStat(vit->second[1]) + ", " +
                                    FormatStat(vit->second[2]) + ")";
                    }
                }
                EditorPanelState::DefStatRow r;
                r.key = sp.key;
                r.value = value;
                r.label = sp.label ? sp.label : sp.key;
                r.group = sp.group ? sp.group : "";
                r.controlId = ID_PP_STAT_FIELD_0 + row;
                r.isFloat = (sp.kind == 0);
                r.isSoundPath = (sp.kind == 3);
                P.propDefEditable.push_back(r);
                row++;
            }

            // Drop read-only rows whose key the Edit section already shows.
            // The dump's job is to surface keys the schema does NOT know about
            // (so nothing hand-authored is hidden); repeating every schema key
            // there just doubled the panel's height and pushed Apply off the
            // bottom of the screen. Nothing is lost - a schema key is still
            // shown, just in the editable row below.
            std::unordered_set<std::string> shown;
            shown.reserve(P.propDefEditable.size() * 2);
            for (const auto& r : P.propDefEditable) shown.insert(r.key);
            P.propDefFields.erase(
                std::remove_if(P.propDefFields.begin(), P.propDefFields.end(),
                               [&](const EditorPanelState::DefField& f) {
                                   return shown.count(f.key) != 0;
                               }),
                P.propDefFields.end());
        }
        return;
    }

    // No .ozls def - PawnDefs/*.cfg-only pawn fallback.
    if (addPawnDefFields(defName)) {
        std::string path = fallbackPath.empty()
            ? ("GameData/Global/PawnDefs/" + defName + ".cfg") : fallbackPath;
        if (!fs::exists(path)) path.clear();
        P.propDefPath = path;
        P.propDefTitle = defName + "  [pawn - PawnDefs only]";
        P.propDefSource = path;
    }
}

void ShowPropertiesPanel(bool show) {
    g_editorPanels.showPropsPanel = show;
    if (show && g_editorPanels.hPropsPanel) {
        // Pre-populate apply values from the selection
        g_editorPanels.propPosX = g_editorPanels.propsTargetPos[0];
        g_editorPanels.propPosY = g_editorPanels.propsTargetPos[1];
        g_editorPanels.propPosZ = g_editorPanels.propsTargetPos[2];
        g_editorPanels.propRotation = g_editorPanels.propsTargetRotation;
        g_editorPanels.propScale = g_editorPanels.propsTargetScale;
        g_editorPanels.propDefPath.clear();
        g_editorPanels.propDefTitle.clear();
        g_editorPanels.propDefSource.clear();
        g_editorPanels.propDefPawnFields.clear();
        g_editorPanels.propDefFields.clear();
        g_editorPanels.propDefActions.clear();
        g_editorPanels.propDefEditable.clear();
        g_editorPanels.propDefPendingEdits.clear();
        g_editorPanels.propDefPreviewSound.clear();
        g_editorPanels.propDefWritable = false;

        if (g_editorPanels.propsTargetType == sel::NPC) {
            if (Pawn* p = PawnSystem::Instance().Get(g_editorPanels.propsTargetIndex)) {
                g_editorPanels.propHealth = (float)p->health;
                g_editorPanels.propSpeed = p->speed;
                FillDefBlock(p->defName, "");
            }
        } else if (g_editorPanels.propsTargetType == sel::PICKUP) {
            for (auto& pk : PawnSystem::Instance().GetPickups()) {
                if ((int)pk.id == g_editorPanels.propsTargetIndex) {
                    g_editorPanels.propRespawnTime = pk.respawnTime;
                    FillDefBlock(pk.typeName, "");
                    break;
                }
            }
        } else if (g_editorPanels.propsTargetType == sel::LIGHT) {
            if (LightNode* l = PawnSystem::Instance().GetLight(g_editorPanels.propsTargetIndex)) {
                g_editorPanels.propLightName = l->name;
                g_editorPanels.propLightR = l->color.r;
                g_editorPanels.propLightG = l->color.g;
                g_editorPanels.propLightB = l->color.b;
                g_editorPanels.propLightIntensity = l->intensity;
                g_editorPanels.propLightRadius = l->radius;
                g_editorPanels.propLightType = (int)l->type;
                g_editorPanels.propLightEffect = (int)l->effect;
                // Recompute the editable degrees from the stored cosines.
                g_editorPanels.propLightInnerAngle =
                    acosf(fminf(fmaxf(l->innerCone, -1.0f), 1.0f)) * RAD2DEG;
                g_editorPanels.propLightOuterAngle =
                    acosf(fminf(fmaxf(l->outerCone, -1.0f), 1.0f)) * RAD2DEG;
                g_editorPanels.propLightFlare = l->flare;
                g_editorPanels.propLightCorona = l->corona;
g_editorPanels.propLightTarget[0] = l->target.x;
                g_editorPanels.propLightTarget[1] = l->target.y;
                g_editorPanels.propLightTarget[2] = l->target.z;
                // A light's `name=` resolves an :light .ozls def exactly as a
                // zone's name= resolves its skyzone def. Without this the Light
                // properties panel showed no def section at all.
                if (!l->name.empty()) FillDefBlock(l->name, "");
            }
        } else if (g_editorPanels.propsTargetType == sel::ZONE) {
            for (auto& z : ZoneManager::Instance().GetZones()) {
                if ((int)z.id == g_editorPanels.propsTargetIndex) {
            g_editorPanels.propZoneType = (int)z.zoneType;
            if (g_editorPanels.propZoneType > 4) g_editorPanels.propZoneType = 0;
            g_editorPanels.propZoneIntensity = z.intensity;
            g_editorPanels.propZoneName = z.name;
            g_editorPanels.propZoneGravity = z.physics.gravity;
            g_editorPanels.propZoneJump = z.physics.jumpSpeed;
            g_editorPanels.propZoneTerminal = z.physics.terminalVelocity;
            g_editorPanels.propZoneWaterGravity = z.physics.waterGravity;
            g_editorPanels.propZoneWaterDrag = z.physics.waterDrag;
            g_editorPanels.propZoneSwimUp = z.physics.swimUpSpeed;
            g_editorPanels.propZoneLadderSpeed = z.physics.ladderSpeed;
            g_editorPanels.propZoneFlyMult = z.physics.flySpeedMult;
            // Environment overrides: the values the runtime actually merges and
            // applies on zone entry (Core.hpp's zone env block). Nothing in the
            // editor wrote these before, so a level's per-zone fog had to be
            // hand-authored in the .ozone.
            {
                const auto& eo = z.envOverrides;
                auto& P = g_editorPanels;
                P.propZoneApplyFog   = eo.applyFog;
                P.propZoneFogR       = eo.fogR;
                P.propZoneFogG       = eo.fogG;
                P.propZoneFogB       = eo.fogB;
                P.propZoneFogDensity = eo.fogDensity;
                P.propZoneFogStart   = eo.fogStart;
                P.propZoneFogEnd     = eo.fogEnd;
                P.propZoneApplyAmbient = eo.applyAmbient;
                P.propZoneAmbR       = eo.ambR;
                P.propZoneAmbG       = eo.ambG;
                P.propZoneAmbB       = eo.ambB;
                P.propZoneAmbIntensity = eo.ambIntensity;
                P.propZoneReverbMix  = eo.reverbMix;
                P.propZoneReverbDecay = eo.reverbDecay;
            }
if (!z.name.empty()) FillDefBlock(z.name, "");
                    break;
                }
            }
        } else if (g_editorPanels.propsTargetType == sel::SPAWN) {
            // `playerstart x y z yaw` carries no name, so the def is resolved by
            // a fixed name instead of a world lookup - the player a spawn points
            // at is always the "Player" def (GameData/Global/Objects/
            // Player.ozls). This branch did not exist, so a PlayerStart showed
            // no def section at all and its .ozls stats (health, mana,
            // jump_sound, ...) were unreachable from the editor.
            for (const auto& s : PawnSystem::Instance().GetPlayerStarts()) {
                if ((int)s.id != g_editorPanels.propsTargetIndex) continue;
                FillDefBlock("Player", "");
                break;
            }
        } else if (g_editorPanels.propsTargetType == sel::PORTAL) {
            auto& portals = ZoneManager::Instance().GetPortals();
            if (g_editorPanels.propsTargetIndex >= 0 &&
                g_editorPanels.propsTargetIndex < (int)portals.size()) {
                auto& p = portals[g_editorPanels.propsTargetIndex];
                g_editorPanels.propPortalWorld = p.targetWorld;
                g_editorPanels.propPortalSpawn[0] = p.targetSpawn.x;
                g_editorPanels.propPortalSpawn[1] = p.targetSpawn.y;
                g_editorPanels.propPortalSpawn[2] = p.targetSpawn.z;
                g_editorPanels.propPortalBidir = p.bidirectional;
            }
        } else if (g_editorPanels.propsTargetType == sel::MESH) {
            if (MeshObjectNode* m = PawnSystem::Instance().GetMeshObject(g_editorPanels.propsTargetIndex)) {
                g_editorPanels.propMeshPath = m->meshPath;
                g_editorPanels.propMeshTex = m->texturePath;
                g_editorPanels.propAnimClip = m->animClip;
                g_editorPanels.propMeshAnimFile = m->animFile;
                g_editorPanels.propMeshAnimSpeed = m->animSpeed;
                g_editorPanels.propScale = m->scale;
                g_editorPanels.propMeshWind = m->windAffected;
            }
        } else if (g_editorPanels.propsTargetType == sel::PARTICLE) {
            if (ParticleEmitterNode* e = PawnSystem::Instance().GetParticleEmitter(g_editorPanels.propsTargetIndex)) {
                g_editorPanels.propEmitterType = e->type;
                g_editorPanels.propEmitterTex = e->texturePath;
                g_editorPanels.propEmitterRate = e->rate;
                g_editorPanels.propEmitterLife = e->lifetime;
                g_editorPanels.propEmitterSpeed = e->speed;
                g_editorPanels.propEmitterSize = e->sizeStart;
                g_editorPanels.propEmitterSpread = e->spread;
                g_editorPanels.propEmitterR = e->colorStart.r;
                g_editorPanels.propEmitterG = e->colorStart.g;
                g_editorPanels.propEmitterB = e->colorStart.b;
                g_editorPanels.propEmitterSizeEnd = e->sizeEnd;
                g_editorPanels.propEmitterREnd = e->colorEnd.r;
                g_editorPanels.propEmitterGEnd = e->colorEnd.g;
                g_editorPanels.propEmitterBEnd = e->colorEnd.b;
                g_editorPanels.propEmitterGravity = e->gravity;
                g_editorPanels.propEmitterRadius = e->radius;
                g_editorPanels.propEmitterBillboard = e->billboard;
            }
        } else if (g_editorPanels.propsTargetType == sel::PATHNODE) {
            if (PathNode* pn = PawnSystem::Instance().GetPathNode(g_editorPanels.propsTargetIndex)) {
                g_editorPanels.propPathName = pn->name;
                g_editorPanels.propPathRadius = pn->radius;
                g_editorPanels.propPathNext.clear();
                for (size_t i = 0; i < pn->next.size(); i++) {
                    if (i) g_editorPanels.propPathNext += ",";
                    g_editorPanels.propPathNext += pn->next[i];
                }
                g_editorPanels.propPathLoop = pn->loop;
            }
        } else if (g_editorPanels.propsTargetType == sel::WINDZONE) {
            if (WindZoneNode* z = PawnSystem::Instance().GetWindZone(g_editorPanels.propsTargetIndex)) {
                g_editorPanels.propWindSizeX = z->bounds.max.x - z->bounds.min.x;
                g_editorPanels.propWindSizeY = z->bounds.max.y - z->bounds.min.y;
                g_editorPanels.propWindSizeZ = z->bounds.max.z - z->bounds.min.z;
                g_editorPanels.propWindDirX = z->direction.x;
                g_editorPanels.propWindDirY = z->direction.y;
                g_editorPanels.propWindDirZ = z->direction.z;
                g_editorPanels.propWindStrength = z->strength;
                g_editorPanels.propWindFrequency = z->frequency;
            }
        } else if (g_editorPanels.propsTargetType == sel::MAP) {
// The level itself: seed every propMap* field from LevelMetadata, which
            // is the single owner of this state. It used to be mirrored into a
            // ZoneProperties global for the Zone window, which is why the two
            // panels could drift; both the global and the window are gone.
            LevelMetadata meta = GetLevelMetadata();
            auto& P = g_editorPanels;
            P.propMapGameType         = (int)meta.gameType;
            P.propMapMaxPlayers       = meta.maxPlayers;
            P.propMapRespawnTime      = meta.respawnTime;
            P.propMapTimeLimitEnabled = meta.timeLimitEnabled;
            P.propMapTimeLimitMinutes = meta.timeLimitMinutes;
            P.propMapScoreLimit       = meta.scoreLimit;
            P.propMapFriendlyFire     = meta.friendlyFire;
            P.propMapSkybox           = meta.skyboxTexturePath;
            P.propMapSkyboxSide       = meta.skyboxSidePath;
            P.propMapParticleType     = (int)meta.particleType;
            P.propMapParticleDensity  = meta.particleDensity;
            P.propMapParticleSpeed    = meta.particleSpeed;
            P.propMapParticleR        = meta.particleColorR;
            P.propMapParticleG        = meta.particleColorG;
            P.propMapParticleB        = meta.particleColorB;
            P.propMapParticleWindX    = meta.particleWindX;
            P.propMapParticleWindZ    = meta.particleWindZ;
        }

        // For brush/zone, derive size from position data if needed
        if (g_editorPanels.propsTargetType == sel::BRUSH) {
            int idx = g_editorPanels.propsTargetIndex;
            // propsTargetIndex for a BRUSH is a RENDERABLE index (set from
            // g_sel.index). If it is out of range the selection is stale, and the
            // honest thing is to leave the previous values alone.
            //
            // There used to be an `else` here that read the CSG collision-volume
            // list with the SAME idx — a renderable index indexing the
            // collision-volume output. That is the index-space confusion
            // AGENTS.md records as deleted with RaycastTestBrushes: the two
            // lists do not correspond once any `sub` is present, so it silently
            // showed a DIFFERENT brush's size and UVs. PropsPanel must never
            // touch that list again (tests/Surface.test.cpp asserts the absence).
            if (idx >= 0 && idx < OzoneLoader::Instance().Count()) {
                OzoneRenderable* r = OzoneLoader::Instance().Get(idx);
                if (r && r->loaded) {
                    BoundingBox mb = GetMeshBoundingBox(r->model.meshes[0]);
                    g_editorPanels.propSizeX = mb.max.x - mb.min.x;
                    g_editorPanels.propSizeY = mb.max.y - mb.min.y;
                    g_editorPanels.propSizeZ = mb.max.z - mb.min.z;
                    g_editorPanels.propTexScaleU = r->texScaleU;
                    g_editorPanels.propTexScaleV = r->texScaleV;
                    g_editorPanels.propTexOffsetU = r->texOffsetU;
                    g_editorPanels.propTexOffsetV = r->texOffsetV;
                }
            }
        } else if (g_editorPanels.propsTargetType == sel::ZONE) {
            auto& zones = ZoneManager::Instance().GetZones();
            for (auto& z : zones) {
                if ((int)z.id == g_editorPanels.propsTargetIndex) {
                    g_editorPanels.propSizeX = z.bounds.max.x - z.bounds.min.x;
                    g_editorPanels.propSizeY = z.bounds.max.y - z.bounds.min.y;
                    g_editorPanels.propSizeZ = z.bounds.max.z - z.bounds.min.z;
                    break;
                }
            }
        } else if (g_editorPanels.propsTargetType == sel::PORTAL) {
            auto& portals = ZoneManager::Instance().GetPortals();
            if (g_editorPanels.propsTargetIndex >= 0 &&
                g_editorPanels.propsTargetIndex < (int)portals.size()) {
                auto& p = portals[g_editorPanels.propsTargetIndex];
                g_editorPanels.propSizeX = p.bounds.max.x - p.bounds.min.x;
                g_editorPanels.propSizeY = p.bounds.max.y - p.bounds.min.y;
                g_editorPanels.propSizeZ = p.bounds.max.z - p.bounds.min.z;
            }
        }

ShowWindow((HWND)g_editorPanels.hPropsPanel, SW_SHOW);
        SetForegroundWindow((HWND)g_editorPanels.hPropsPanel);
        // Rebuild controls for the current entity type
        SendMessage((HWND)g_editorPanels.hPropsPanel, WM_USER + 50, 0, 0);
    } else if (g_editorPanels.hPropsPanel) {
        ShowWindow((HWND)g_editorPanels.hPropsPanel, SW_HIDE);
    }
}

// Open the Entity Properties panel on a DEF rather than a world instance.
//
// Needed for defs with no instance in the open world - Player.ozls is the
// motivating case, since the player's sound stats have to be editable and the
// player is never placed in a level. propsTargetType is left at -1 so the
// panel's per-SelType position/rotation/size rows are skipped: they belong to a
// world instance, and there is none here. The def section is what matters.
void ShowDefPropertiesFor(const std::string& defName) {
    const EntityDef* def = LightningEntityRegistry::Instance().Find(defName);
    if (!def) return;

    g_editorPanels.propsTargetType = sel::DEF_ONLY;
    g_editorPanels.propsTargetIndex = -1;
    g_editorPanels.propsTargetName = defName;

    // Fill the def block directly rather than via ShowPropertiesPanel, which
    // derives it from the current world selection and would find nothing.
    FillDefBlock(defName, "");
    ShowPropertiesPanel(true);
}

