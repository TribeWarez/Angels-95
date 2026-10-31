// AngelServ – Dedicated server for AngelTech / OzWorld
// Provides UDP game server + HTTP map API + LAN discovery.
// Build: g++ -O3 --std=c++20 -fPIC -lpthread -lm
// Usage: AngelServ [--port P] [--http-port P] [--dir GameData]

#include "../Network/Network.hpp"
#include "../World/OzoneParser.hpp"
#include "GameState.hpp"
#include "../Game/GameManager.hpp"
#include "../Game/GameBoxedTypes.hpp"
#include "../Game/GameOpcodes.hpp"
#include "../Script/LightningEntityRegistry.hpp"
#include "../Log.hpp"
#include "../IniConfig.hpp"
#include "../Version.hpp"
#include "Master/MasterClient.hpp"
#include "ServerInternal.hpp"

#include <csignal>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>
#include <thread>
#include <chrono>
#include <mutex>

// ---------------------------------------------------------------------------
// Globals / Config
// ---------------------------------------------------------------------------
bool g_running = true;
net::NetworkServer* g_game_server = nullptr;
std::string g_gamedata_dir = "GameData";
int g_http_port = 8080;
std::mutex g_print_mutex;
std::vector<std::string> g_world_list;
std::string g_auth_token;   // HTTP API token (--auth-token / OZ_AUTH_TOKEN)
std::string g_admin_token;  // admin COMMAND token (--admin-token / OZ_ADMIN_TOKEN)
std::string g_server_name = "Angels95 Server";
std::chrono::steady_clock::time_point g_server_start_time;

GameState g_game_state;
net::NetworkDiscovery* g_discovery = nullptr; // set in main(); keeps LAN announce counts current

// Master-server uplink (internet discovery). Empty lists = master disabled.
std::vector<std::string> g_master_udp;   // --master host[:port]
std::vector<std::string> g_master_http;  // --master-http http://host:port
std::string g_public_ip;                 // --public-ip (NAT)
std::string g_bind_ip;                   // --bind (default = all interfaces)
bool g_campaign = false;                 // --campaign (mission arc; default OFF)
std::string g_mission;                   // --mission <name> (implies --campaign)
std::string g_world_pin;                 // --world <name> (plain co-op spawn world)
// The world the named mission resolves to, filled during startup validation.
// Only used to report the map; empty = no --mission.
std::string g_mission_world;

std::string current_map_name() {
    // With --mission the players start (and stay, until the objectives
    // complete) in the named mission's world, NOT in world_list[0]. Naming the
    // alphabetically-first world in the server browser and in HTTP /map would
    // tell joining players the wrong map.
    if (!g_mission_world.empty()) return g_mission_world;
    if (!g_world_pin.empty()) return g_world_pin;
    return g_world_list.empty() ? std::string("unknown") : g_world_list[0];
}

// Send a chat message to a single player (system notices / command replies)
void send_syschat(const net::NetworkPlayer& player, const std::string& text) {
    net::ChatData cd{};
    strncpy(cd.text, text.c_str(), sizeof(cd.text) - 1);
    net::NetworkMessage msg{};
    msg.magic = net::MAGIC;
    msg.type = static_cast<uint32_t>(net::MessageType::CHAT);
    msg.size = sizeof(cd);
    msg.sequence = 0;
    msg.timestamp = static_cast<uint32_t>(time(nullptr));
    memcpy(msg.payload, &cd, sizeof(cd));
    g_game_server->send_message(player, msg);
}

static void signal_handler(int) { g_running = false; }


// ---------------------------------------------------------------------------
// Server state management
// ---------------------------------------------------------------------------
// Build the PICKUP_RESPAWN payload for one pickup. Single source of truth for
// the three places that send it (join snapshot, respawn broadcast, re-sync
// answer to a rejected collect) — they drifted once already, and a drifted
// sender silently fails the client's size check and drops the message.
static net::PickupRespawnData make_pickup_respawn(const ServerPickup& p, int world_index) {
    net::PickupRespawnData prd{};
    prd.pickup_id = p.id;
    prd.world_index = world_index;
    prd.position = {p.position.x, p.position.y, p.position.z};
    prd.type = static_cast<int>(p.type);
    prd.value = p.value;
    // Zero-init matters: this path used to leave weapon_def_name uninitialised
    // and ship stack garbage on the wire.
    strncpy(prd.weapon_def_name, p.weapon_def_name, sizeof(prd.weapon_def_name) - 1);
    strncpy(prd.typeName, p.typeName, sizeof(prd.typeName) - 1);
    // Optional tail: tells the client whether this pickup is still on the floor.
    // The snapshot sends inactive entries too (see for_each_pickup) so a pickup
    // someone else consumed is HIDDEN instead of staying drawn and uncollectable.
    prd.active = p.active ? 1 : 0;
    return prd;
}

static void send_pickup_respawn_msg(const net::NetworkPlayer& player,
                                    int world_index, const ServerPickup& p) {
    net::PickupRespawnData prd = make_pickup_respawn(p, world_index);
    net::NetworkMessage msg{};
    msg.magic = net::MAGIC;
    msg.type = static_cast<uint32_t>(net::MessageType::PICKUP_RESPAWN);
    msg.size = sizeof(prd);
    msg.sequence = 0;
    msg.timestamp = static_cast<uint32_t>(time(nullptr));
    memcpy(msg.payload, &prd, sizeof(prd));
    g_game_server->send_message(const_cast<net::NetworkPlayer&>(player), msg);
}

// Magazine size the server records when a player collects a weapon pickup.
//
// The server has NO weapon-def loader: `magazine` lives in the client's `.ozls`
// and the network layer only ever carries the def NAME. This table is therefore
// the server's copy of the defs' authored magazines, and it silently drifts if a
// weapon is retuned on the client. It used to be two strcmps inline at the
// collect site (automag 12, selenite_blade 0, everything else 12), so a rifle
// collected in multiplayer was tracked as a 12-round magazine while the client
// fired 30 - every reload disagreed with the server's accounting.
//
// A melee weapon has no magazine (0). Unknown defs keep the old 12.
static int DefaultMagazineForWeapon(const char* defName) {
    struct Entry { const char* name; int magazine; };
    static const Entry kWeapons[] = {
        { "pistol_01",     15 },   // Pistol_01.ozls
        { "rifle_01",      30 },   // Rifle_01.ozls
        { "smg_01",        32 },   // SMG_01.ozls
        { "shotgun_01",     6 },   // Shotgun_01.ozls
        { "automag",       12 },   // automag.ozls
        { "flux_carbine",  24 },   // flux_carbine.ozls
        { "selenite_blade", 0 },   // melee
        { "etheral_waver",  0 },   // melee
    };
    if (!defName) return 12;
    for (const Entry& e : kWeapons) {
        if (strcmp(defName, e.name) == 0) return e.magazine;
    }
    return 12;
}

// Re-send EVERY pickup in one world to a single player, active or not. This is
// the self-heal path: pickups are otherwise only sent on join and on respawn, so
// a single lost UDP datagram left a pickup drawn on the floor that could never be
// collected again for the rest of the session.
//
// Scoped to ONE world on purpose — the join snapshot walked every loaded world
// (all shipped worlds are authored near the origin), which is a lot of traffic
// for state the client cannot use. Each player gets the world it is standing in.
//
// Inactive entries must be included, not skipped: the client draws its own copy
// of every `pickup` line in World.ozone, so a pickup another player already took
// is only removed from the client's screen if the server tells it the id is
// consumed. Sending active ones only is why a rifle someone else had collected
// stayed drawn and could never be picked up (the collect was refused NOT_FOUND
// on every walk-over, and each refusal re-sent the same active-only snapshot).
static void send_pickup_snapshot(const net::NetworkPlayer& player, int world_index) {
    int synced = 0, inactive = 0;
    g_game_state.for_each_pickup(world_index, [&](WorldState& ws, ServerPickup& p) {
        (void)ws;
        send_pickup_respawn_msg(player, world_index, p);
        synced++;
        if (!p.active) inactive++;
    });
    if (synced)
        OZ_INFO("Re-synced %d pickup(s) in world %d to player %s (%d inactive)",
                synced, world_index, player.name, inactive);
}

// World-list JSON for a joining player, clamped to the network payload size so
// a long world list can never overflow the fixed-size message buffer.
// `player_world` is the world the joiner was PLACED in (add_player ->
// current_mission_world_index()); naming it active_world keeps the client's
// scene handshake and the WORLD_CHANGE sent right after this message in
// agreement — they used to disagree under --campaign (active_world was
// hardcoded to world 0), and UDP has no ordering guarantee, so whichever
// arrived last decided which world the client rendered.
static void send_join_world_list(const net::NetworkPlayer& player, int player_world) {
    constexpr size_t kMaxPayload = net::MAX_MESSAGE_SIZE;
    std::string active_world;
    if (!g_world_list.empty()) {
        const size_t worldSlot = (player_world >= 0 &&
                                  player_world < (int)g_world_list.size())
                                     ? (size_t)player_world : 0;
        active_world = g_world_list[worldSlot];
    }
    // json_escape() already emits the surrounding JSON quotes, so do NOT add
    // another pair here (that produced "active_world":""Name"" and broke the
    // client's scene parse, leaving it on its default world).
    std::string world_list = "{\"type\":\"world_list\",\"active_world\":" + json_escape(active_world) + ",\"worlds\":[";
    for (size_t i = 0; i < g_world_list.size(); ++i) {
        std::string entry = json_escape(g_world_list[i]);
        if (world_list.size() + entry.size() + 2 > kMaxPayload) break; // room for ",]"
        if (i) world_list += ',';
        world_list += entry;
    }
    world_list += "]}";
    if (world_list.size() > kMaxPayload) world_list.resize(kMaxPayload);

    net::NetworkMessage msg{};
    msg.magic = net::MAGIC;
    msg.type = static_cast<uint32_t>(net::MessageType::SCENE_UPDATE);
    msg.size = static_cast<uint32_t>(world_list.size());
    msg.sequence = 0;
    msg.timestamp = static_cast<uint32_t>(time(nullptr));
    memcpy(msg.payload, world_list.data(), world_list.size());
    g_game_server->send_message(player, msg);
}

// Build an NPC state snapshot (includes the pawn def type name so clients
// can spawn the correct sprite/scream instead of hardcoding "Walker").
static net::NpcStateUpdateData make_npc_state(const WorldState& ws, int npc_index,
                                              int partition_index, const ServerNPC& n) {
    net::NpcStateUpdateData nsud{};
    nsud.world_index = ws.world_index;
    nsud.npc_index = npc_index;
    nsud.partition_index = partition_index;
    nsud.position = n.position;
    nsud.yaw = n.yaw;
    nsud.state = static_cast<int>(n.state);
    nsud.health = n.health;
    nsud.active = n.active;
    strncpy(nsud.npc_type, n.typeName.c_str(), sizeof(nsud.npc_type) - 1);
    nsud.npc_type[sizeof(nsud.npc_type) - 1] = '\0';
    return nsud;
}

// Tell a client which world the server put it in, and where to stand.
//
// Sent on join AND as the answer to REQUEST_WORLD, because the client's request is
// advisory: the server validates the index against its own loaded worlds, so the
// client must not assume its request took. `accepted` distinguishes the two cases.
// Cosmetic character/colour snapshot. The id is already sanitised before this
// is called; re-clamp the colour here so a corrupt ServerPlayer cannot write
// arbitrary bytes into a client palette lookup.
static net::NetworkMessage make_player_profile_msg(const ServerPlayer& sp) {
    net::PlayerProfileData pd{};
    pd.player_id = sp.id;
    std::strncpy(pd.character, sp.character.c_str(), sizeof(pd.character) - 1);
    pd.color = (uint8_t)std::clamp(sp.color, 0, 7);

    net::NetworkMessage msg{};
    msg.magic = net::MAGIC;
    msg.type = static_cast<uint32_t>(net::MessageType::PLAYER_PROFILE);
    msg.size = sizeof(pd);
    msg.sequence = 0;
    msg.timestamp = static_cast<uint32_t>(time(nullptr));
    std::memcpy(msg.payload, &pd, sizeof(pd));
    return msg;
}

static void broadcast_player_profile(const ServerPlayer& sp) {
    if (!g_game_server) return;
    auto msg = make_player_profile_msg(sp);
    g_game_server->broadcast_message(msg);
}

static void send_player_profile_to(const net::NetworkPlayer& target, const ServerPlayer& sp) {
    if (!g_game_server) return;
    auto msg = make_player_profile_msg(sp);
    g_game_server->send_message(target, msg);
}

static void send_world_change(net::NetworkPlayer& player, int world_index, bool accepted) {    net::WorldChangeData wcd{};
    wcd.world_index = world_index;
    wcd.accepted = accepted ? 1 : 0;
    wcd.spawnX = 0.0f;
    wcd.spawnY = 0.0f;
    wcd.spawnZ = 0.0f;
    // Authoritative spawn (first playerstart) so the client arrives on the floor
    // instead of the origin, plus the mission this world implements (co-op
    // narrative beats; see WorldChangeData::mission).
    if (world_index >= 0 && world_index < g_game_state.world_count()) {
        const auto& ws = g_game_state.worlds()[(size_t)world_index];
        if (ws.has_spawn) {
            wcd.spawnX = ws.spawn_x;
            wcd.spawnY = ws.spawn_y;
            wcd.spawnZ = ws.spawn_z;
        }
        if (!ws.mission.empty()) {
            if (!net::MissionNameFits(ws.mission))
                OZ_WARN("WORLD_CHANGE: mission name '%s' is %zu chars - the wire field "
                        "holds %zu; the client's beat will use a truncated name",
                        ws.mission.c_str(), ws.mission.size(), net::kMissionNameMax - 1);
            strncpy(wcd.mission, ws.mission.c_str(), sizeof(wcd.mission) - 1);
        }
    }

    net::NetworkMessage msg;
    msg.magic = net::MAGIC;
    msg.type = static_cast<uint32_t>(net::MessageType::WORLD_CHANGE);
    msg.size = sizeof(wcd);
    msg.sequence = 0;
    msg.timestamp = static_cast<uint32_t>(time(nullptr));
    memcpy(msg.payload, &wcd, sizeof(wcd));
    g_game_server->send_message(player, msg);
    OZ_INFO("WORLD_CHANGE -> player %u: world=%d accepted=%d",
            player.id, world_index, wcd.accepted);
}

// PLAYER_STATE -> the ONE player it names.
//
// Directed on purpose. The old PLAYER_HURT relay was broadcast, and the client
// applies it as "you were hit", so one player's damage rewrote everyone's health
// and played the hurt/death sound on all of them.
//
// `teleport` is set only on a respawn: the server has already moved its own copy
// of the player to the world spawn, so the client must follow. A damage update
// must never yank the player back to spawn.
static void send_player_state(const ServerPlayer& sp, bool teleport, int damage) {
    if (!g_game_server) return;
    net::PlayerStateData ps{};
    ps.player_id   = sp.id;
    ps.health      = (int32_t)sp.health;
    ps.max_health  = (int32_t)sp.max_health;
    ps.damage      = (int32_t)damage;
    ps.dead        = sp.dead ? 1u : 0u;
    if (sp.dead && sp.respawn_at > 0.0) {
        const double left = sp.respawn_at - (double)time(nullptr);
        ps.respawn_seconds = left > 0.0 ? (float)left : 0.0f;
    }
    if (teleport) {
        const WorldState* ws = g_game_state.get_world(sp.world_index);
        if (ws && ws->has_spawn) {
            ps.teleport = 1;
            ps.x = ws->spawn_x;
            ps.y = ws->spawn_y;
            ps.z = ws->spawn_z;
        }
    }
    net::NetworkMessage msg{};
    msg.magic = net::MAGIC;
    msg.type = static_cast<uint32_t>(net::MessageType::PLAYER_STATE);
    msg.size = sizeof(ps);
    msg.sequence = 0;
    msg.timestamp = static_cast<uint32_t>(time(nullptr));
    memcpy(msg.payload, &ps, sizeof(ps));
    for (const auto& np : g_game_server->players()) {
        if (np.id != sp.id) continue;
        g_game_server->send_message(const_cast<net::NetworkPlayer&>(np), msg);
        break;
    }
}

static void on_player_join(net::NetworkPlayer& player) {
    OZ_INFO("Player %s (id=%u) joined from %s:%u",
            player.name, player.id, player.ip_address, player.port);
    // Idempotent: re-auth/re-ack may replay this for an existing player.
    // player.name is already sanitised and de-duplicated by the network layer,
    // so it is safe to use as a persistent identity key.
    g_game_state.add_player(player.id, player.name,
                            player.profile.team, player.profile.requestedTeam);

    // add_player refuses at the world's maxPlayers (which is 8 on the shipped
    // levels, NOT net::MAX_PLAYERS=32). The return used to be discarded, so a
    // player over the cap stayed connected at the network layer with no
    // ServerPlayer: a ghost that received broadcasts but whose own updates
    // were all dropped. Reject with a visible message and close the session.
    ServerPlayer* joined = g_game_state.get_player(player.id);
    if (!joined) {
        OZ_WARN("Rejecting %s (id=%u): server full (%d/%u)",
                player.name, player.id, g_game_state.player_count(),
                g_game_state.max_players());
        send_syschat(player, "[server] Server is full.");
        g_game_server->kick_player(player.id);
        return;
    }
    if (g_discovery)
        g_discovery->set_player_count((uint32_t)g_game_state.player_count(),
                                      g_game_state.max_players());

    // Placement already happened in add_player; both messages below must name
    // THAT world — the scene handshake's active_world drives the client's
    // first LoadWorld, WORLD_CHANGE is its authoritative view.
    ServerPlayer* sp = joined;
    const int wi = sp ? sp->world_index : 0;

    // Cosmetic profile: the auth payload's modelPath is a roster id (legacy
    // clients may still send a path). Sanitize it before it reaches a client;
    // the server never resolves it. Broadcast this player to everyone and send
    // every existing player's profile to this one, so late joiners see the same
    // characters as early joiners.
    if (sp) {
        sp->character = net::SanitizeCharacterId(player.profile.modelPath);
        sp->color = net::ClampRequestedTeam(player.profile.requestedTeam);
        broadcast_player_profile(*sp);
        for (const auto& other : g_game_server->players()) {
            if (other.id == player.id) continue;
            if (const ServerPlayer* osp = g_game_state.get_player(other.id))
                send_player_profile_to(player, *osp);
        }
    }

    send_join_world_list(player, wi);

    // Sync active pickups. One world per player (the one they are in) rather
    // than every loaded world — see send_pickup_snapshot for why, and the
    // periodic re-sync in the main loop for the lost-datagram case.

    // Tell the client its authoritative world assignment before the pickup snapshot
    // that is scoped to it. Order matters: the client derives its own world index
    // from this message, and a pickup for world N arriving before the client knows
    // it is in world N is filtered out by the pickup draw path.
    send_world_change(player, wi, /*accepted=*/true);

    send_pickup_snapshot(player, wi);

    // The client used to start every session assuming 100 health. Health is
    // restored from the save (and may legitimately be below max), so state it.
    if (sp) send_player_state(*sp, /*teleport=*/false, /*damage=*/0);
}

static void on_player_leave(net::NetworkPlayer& player) {
    printf("Player %s left\n", player.name);
    g_game_state.remove_player(player.id);
    if (g_discovery)
        g_discovery->set_player_count((uint32_t)g_game_state.player_count(),
                                      g_game_state.max_players());

    // Broadcast departure to remaining clients
    net::NetworkMessage leave;
    leave.magic = net::MAGIC;
    leave.type = static_cast<uint32_t>(net::MessageType::PLAYER_LEAVE);
    leave.size = sizeof(player.id);
    leave.sequence = 0;
    leave.timestamp = static_cast<uint32_t>(time(nullptr));
    memcpy(leave.payload, &player.id, sizeof(player.id));
    g_game_server->broadcast_message(leave);
}

static void on_server_message(const net::NetworkMessage& msg,
                               const net::NetworkPlayer& sender) {
    auto type = static_cast<net::MessageType>(msg.type);
    switch (type) {
        case net::MessageType::PLAYER_UPDATE: {
            if (msg.size < sizeof(net::PlayerUpdateData)) return;
            net::PlayerUpdateData pud;
            memcpy(&pud, msg.payload, sizeof(pud));

            // All validation (finite, teleport clamp) lives in GameState so
            // every position source shares the same choke point.
            if (!g_game_state.update_player_position(
                    sender.id, pud.position.x, pud.position.y, pud.position.z,
                    pud.yaw, pud.pitch)) {
                OZ_WARN("PLAYER_UPDATE from %u rejected", sender.id);
                break;
            }

            // Relay to all other players (with server-authoritative health)
            ServerPlayer* sp = g_game_state.get_player(sender.id);
            pud.player_id = sender.id;
            if (sp) pud.health = sp->health;
            net::NetworkMessage relay;
            relay.magic = net::MAGIC;
            relay.type = static_cast<uint32_t>(net::MessageType::PLAYER_UPDATE);
            relay.size = sizeof(pud);
            relay.sequence = 0;
            relay.timestamp = static_cast<uint32_t>(time(nullptr));
            memcpy(relay.payload, &pud, sizeof(pud));
            for (const auto& p : g_game_server->players()) {
                if (p.id != sender.id && p.connected) {
                    g_game_server->send_message(p, relay);
                }
            }
            break;
        }
        case net::MessageType::PICKUP_COLLECT: {
            if (msg.size < sizeof(net::PickupCollectData)) return;
            net::PickupCollectData pcd;
            memcpy(&pcd, msg.payload, sizeof(pcd));
            PickupType ptype;
            int pvalue;
            char weapon_def_name[64] = {0};
            PickupReject why = PickupReject::NONE;
            if (!g_game_state.collect_pickup(sender.id, pcd.pickup_id, pcd.world_index,
                                             &ptype, &pvalue, weapon_def_name,
                                             sizeof(weapon_def_name), &why)) {
                // Log the reason and the numbers. OUT_OF_RANGE is a legitimate
                // thing for a laggy client to send; NOT_FOUND / WRONG_WORLD mean
                // the two sides disagree about state and the re-sync below is what
                // repairs it.
                OZ_WARN("Pickup collect refused for %s (id=%d world=%d): %s",
                        sender.name, pcd.pickup_id, pcd.world_index,
                        pickup_reject_str(why));
                // Answer a refusal with the current truth for the player's own
                // world. Previously nothing came back at all, so a client whose
                // list had drifted stayed wrong for the rest of the session.
                ServerPlayer* rp = g_game_state.get_player(sender.id);
                if (rp) send_pickup_snapshot(sender, rp->world_index);
                break;
            }
            {
                ServerPlayer* pl = g_game_state.get_player(sender.id);
                if (pl) {
                    net::XpUpdateData xud;
                    xud.player_id = sender.id;
                    xud.xp = pl->xp;
                    xud.level = pl->level;
                    xud.xp_to_next = pl->xp_to_next;
                    net::NetworkMessage xmsg;
                    xmsg.magic = net::MAGIC;
                    xmsg.type = static_cast<uint32_t>(net::MessageType::XP_UPDATE);
                    xmsg.size = sizeof(xud);
                    xmsg.sequence = 0;
                    xmsg.timestamp = static_cast<uint32_t>(time(nullptr));
                    memcpy(xmsg.payload, &xud, sizeof(xud));
                    net::NetworkPlayer tmp = sender;
                    g_game_server->send_message(tmp, xmsg);
                }
                // Shared pickup->item mapping (see Pawn/PickupItems.hpp).
                const PickupGrant grant = ResolvePickupItemId(ptype, pvalue);
                int item_id = grant.itemId;
                int quantity = grant.quantity;
                char weapon_def_out[64] = {0};
                if (ptype == PickupType::WEAPON) {
                        // Use weapon def name from server pickup, or default
                        if (weapon_def_name[0] != '\0') {
                            strncpy(weapon_def_out, weapon_def_name, sizeof(weapon_def_out) - 1);
                        } else {
                            strcpy(weapon_def_out, "automag"); // default
                        }
                        // Store weapon in player's weapon registry (first free slot)
                        ServerPlayer* pl2 = g_game_state.get_player(sender.id);
                        if (pl2) {
                            for (int i = 0; i < SERVER_WEAPON_SLOTS; i++) {
                                if (pl2->weapon_def[i][0] == '\0') {
                                    strncpy(pl2->weapon_def[i], weapon_def_out, 63);
                                    pl2->weapon_def[i][63] = '\0';
                                    // Server-side magazine accounting; see
                                    // DefaultMagazineForWeapon for why this is a
                                    // table rather than a def lookup.
                                    int mag = DefaultMagazineForWeapon(weapon_def_out);
                                    pl2->weapon_magazine[i] = mag;
                                    pl2->weapon_ammo[i] = mag;
                                    break;
                                }
                            }
                        }
                }
                net::PickupCollectedData pcd_out;
                pcd_out.player_id = sender.id;
                pcd_out.pickup_id = pcd.pickup_id;
                // Echo the world the pickup actually came from (already
                // validated against player->world_index in collect_pickup).
                pcd_out.world_index = pcd.world_index;
                pcd_out.item_id = item_id;
                pcd_out.quantity = quantity;
                strncpy(pcd_out.weapon_def_name, weapon_def_out, sizeof(pcd_out.weapon_def_name) - 1);
                net::NetworkMessage imsg;
                imsg.magic = net::MAGIC;
                imsg.type = static_cast<uint32_t>(net::MessageType::PICKUP_COLLECTED);
                imsg.size = sizeof(pcd_out);
                imsg.sequence = 0;
                imsg.timestamp = static_cast<uint32_t>(time(nullptr));
                memcpy(imsg.payload, &pcd_out, sizeof(pcd_out));
                // Collector gets item; everyone gets deactivate via same message
                g_game_server->broadcast_message(imsg);
            }
            break;
        }
        case net::MessageType::PICKUP_RESYNC: {
            // No payload: the server decides which world from its own player
            // record rather than trusting a client-supplied index.
            ServerPlayer* sp = g_game_state.get_player(sender.id);
            if (!sp) break;
            OZ_INFO("Player %s requested a pickup re-sync (world %d)",
                    sender.name, sp->world_index);
            send_pickup_snapshot(sender, sp->world_index);
            break;
        }
        case net::MessageType::CHAT: {
            g_game_server->broadcast_message(msg);
            break;
        }
        case net::MessageType::PLAYER_ACTION: {
            if (msg.size < sizeof(net::WeaponFireData)) return;
            net::WeaponFireData wfd;
            memcpy(&wfd, msg.payload, sizeof(wfd));
            wfd.player_id = sender.id;

            ServerPlayer* sp = g_game_state.get_player(sender.id);
            if (!sp) return;
            // A dead player cannot fire (the client is client-authoritative about
            // its own input, so this is the only place that can refuse).
            if (sp->dead) break;
            WorldState* ws = g_game_state.get_world(sp->world_index);
            if (!ws) break;

            // Origin/direction/power are UNTRUSTED. The sibling MELEE_HIT and
            // NPC_DAMAGE handlers cap damage and re-validate against the server's
            // own positions; this path used to take all three verbatim, so a
            // client could spawn a projectile anywhere aimed at anyone with
            // arbitrary damage.
            const float vals[6] = {wfd.origin_x, wfd.origin_y, wfd.origin_z,
                                   wfd.dir_x, wfd.dir_y, wfd.dir_z};
            bool finite = true;
            for (float v : vals) if (!std::isfinite(v)) finite = false;
            if (!finite) {
                OZ_WARN("PLAYER_ACTION from %u: non-finite origin/direction dropped",
                        sender.id);
                break;
            }
            float dx = wfd.dir_x, dy = wfd.dir_y, dz = wfd.dir_z;
            const float len = sqrtf(dx*dx + dy*dy + dz*dz);
            if (!(len > 1e-4f)) {
                // A zero direction parks the projectile on the player, where it
                // damages whoever is standing there.
                OZ_WARN("PLAYER_ACTION from %u: zero-length direction dropped", sender.id);
                break;
            }
            NetVec3 origin = {wfd.origin_x, wfd.origin_y, wfd.origin_z};
            NetVec3 dir = {dx / len, dy / len, dz / len};

            const int dmg = net::ClampActionDamage(wfd.power);
            if (dmg != wfd.power)
                OZ_WARN("PLAYER_ACTION from %u: damage %d clamped to %d",
                        sender.id, wfd.power, dmg);
            wfd.power = dmg;          // observers see the clamped value too
            wfd.dir_x = dir.x; wfd.dir_y = dir.y; wfd.dir_z = dir.z;

            const float speed = 20.0f;
            const float lifetime = 2.0f;
            g_game_state.spawn_projectile(*ws, sender.id, origin, dir, speed,
                                          (float)dmg, lifetime);

            // Relay weapon fire to other players for visual rendering
            net::NetworkMessage relay;
            relay.magic = net::MAGIC;
            relay.type = static_cast<uint32_t>(net::MessageType::PLAYER_ACTION);
            relay.size = sizeof(wfd);
            relay.sequence = 0;
            relay.timestamp = static_cast<uint32_t>(time(nullptr));
            memcpy(relay.payload, &wfd, sizeof(wfd));
            for (const auto& p : g_game_server->players()) {
                if (p.id != sender.id && p.connected) {
                    g_game_server->send_message(p, relay);
                }
            }
            break;
        }
        case net::MessageType::WEAPON_AMMO: {
            if (msg.size < sizeof(net::WeaponAmmoData)) return;
            net::WeaponAmmoData wad;
            memcpy(&wad, msg.payload, sizeof(wad));
            // Bounds-check client-reported ammo (anti-cheat basic clamp).
            if (wad.ammo < 0) wad.ammo = 0;
            else if (wad.ammo > 999) wad.ammo = 999;
            if (wad.magazine < 0) wad.magazine = 0;
            else if (wad.magazine > 999) wad.magazine = 999;
            wad.player_id = sender.id;
            ServerPlayer* sp = g_game_state.get_player(sender.id);
            if (sp && wad.slot >= 0 && wad.slot < 8) {
                if (wad.action == 0) { // fire
                    sp->weapon_ammo[wad.slot] = wad.ammo;
                } else if (wad.action == 1) { // reload
                    sp->weapon_ammo[wad.slot] = wad.ammo;
                } else if (wad.action == 2) { // full sync
                    if (wad.slot < 8) {
                        sp->weapon_ammo[wad.slot] = wad.ammo;
                        sp->weapon_magazine[wad.slot] = wad.magazine;
                    }
                }
                // Relay ammo update to other players
                net::NetworkMessage relay;
                relay.magic = net::MAGIC;
                relay.type = static_cast<uint32_t>(net::MessageType::WEAPON_AMMO);
                relay.size = sizeof(wad);
                relay.sequence = 0;
                relay.timestamp = static_cast<uint32_t>(time(nullptr));
                memcpy(relay.payload, &wad, sizeof(wad));
                for (const auto& p : g_game_server->players()) {
                    if (p.id != sender.id && p.connected) {
                        g_game_server->send_message(p, relay);
                    }
                }
            }
            break;
        }
        case net::MessageType::FILE_TRANSFER: {
            // Rejected by design (no chunked reassembly / trust model yet) —
            // but tell the sender instead of silently dropping. Notice is
            // rate-limited per sender so a peer can't amplify traffic.
            static std::unordered_map<uint32_t, double> s_last_notice;
            double now = time(nullptr);
            if (now - s_last_notice[sender.id] > 10.0) {
                s_last_notice[sender.id] = now;
                send_syschat(sender, "[server] file transfer unsupported");
            }
            OZ_INFO("FILE_TRANSFER from player %u (size=%u) — rejected (unsupported)",
                    sender.id, msg.size);
            break;
        }
        case net::MessageType::SCENE_UPDATE: {
            // Server -> client ONLY. This relayed a client's payload verbatim to
            // every OTHER client, and the receiving side feeds `active_world`
            // into LoadWorld - which treats a name containing ".ozone" as a
            // direct filesystem path. Any peer could therefore make every player
            // try to load an arbitrary local file as a world. The server authors
            // its own scene (send_join_world_list); it never needs to relay one.
            // Rate-limited so a peer cannot spam the log.
            static std::unordered_map<uint32_t, double> s_lastSceneWarn;
            const double sceneNow = time(nullptr);
            if (sceneNow - s_lastSceneWarn[sender.id] > 10.0) {
                s_lastSceneWarn[sender.id] = sceneNow;
                OZ_WARN("SCENE_UPDATE from player %u rejected (server-authoritative)",
                        sender.id);
            }
            break;
        }
        case net::MessageType::NPC_STATE_UPDATE: {
            // Server->client ONLY. The server is authoritative over NPCs: it derives
            // their state in tick_npcs and broadcasts it (see the spawn broadcast at
            // the bottom of this function, and the two make_npc_state relays after
            // NPC_DAMAGE and MELEE_HIT).
            //
            // This case used to apply the client's values to GameState and re-broadcast
            // them verbatim, with no ownership, range, world or rate check — unlike
            // both neighbouring handlers, NPC_DAMAGE (reach + rate limit) and MELEE_HIT
            // (reach + stamina + world match). Any client could therefore set
            // health = 0 and active = false on any npc_index in any world and have the
            // server broadcast it, i.e. teleport or kill any NPC on demand.
            //
            // No legitimate client sends this: the shipped client's only occurrence of
            // the type is the RECEIVE case in Client.cpp's handle_message, and its NPCs
            // are networkControlled so they never report state. So the whole handler
            // was attack surface with no caller. Dropping it is the fix; the enum value
            // stays, because the server->client direction is the protocol's contract.
            //
            // A future legitimate client->server NPC report belongs on a NEW message
            // type with real validation, not by relaxing this one.
            OZ_WARN("NPC_STATE_UPDATE from player %u — server sends these, ignoring "
                    "incoming (client-claimed NPC state is never authoritative)",
                    sender.id);
            break;
        }
        case net::MessageType::REQUEST_WORLD: {
            if (msg.size < sizeof(net::RequestWorldData)) break;
            net::RequestWorldData rwd;
            memcpy(&rwd, msg.payload, sizeof(rwd));

            ServerPlayer* sp = g_game_state.get_player(sender.id);
            if (!sp) break;

            // UNTRUSTED INDEX. The client names a world by position in the server's
            // own list, so the only safe thing to do with that number is bounds-check
            // it against the server's world_count() before it reaches anything that
            // indexes m_worlds. get_world() would return nullptr and the move would
            // silently no-op, but a refused request must be REPORTED, not silent —
            // otherwise the client waits forever for a world it never got.
            const int want = rwd.world_index;
            const int worldCount = g_game_state.world_count();
            if (want < 0 || want >= worldCount) {
                OZ_WARN("REQUEST_WORLD from player %u — index %d out of range "
                        "(server has %d world(s)); staying in world %d",
                        sender.id, want, worldCount, sp->world_index);
                // Answer with where the player ACTUALLY is, so the client can
                // reconcile rather than assume it is lost.
                send_world_change(const_cast<net::NetworkPlayer&>(sender),
                                  sp->world_index, /*accepted=*/false);
                break;
            }

            // Already there: still answer, so a client that re-requests after a lost
            // datagram converges instead of waiting.
            if (sp->world_index == want) {
                send_world_change(const_cast<net::NetworkPlayer&>(sender), want,
                                  /*accepted=*/true);
                break;
            }

            // Campaign completion IS this door. Leaving a mission world through
            // the portal that leads to its `next` world is what finishes the
            // mission, so the authored arcs run to their own landmarks (the
            // shrine, the tower top, the rig) instead of being cut short by an
            // objective counter.
            //
            // The whole party moves with the player who opened it: the arc is
            // coordinated, and leaving teammates behind in a completed world
            // would split the session. `mission_skipped` worlds are behind a
            // --mission pin and are never completed.
            if (g_game_state.campaign_enabled()) {
                WorldState* from = g_game_state.get_world(sp->world_index);
                WorldState* to   = g_game_state.get_world(want);
                if (from && to && g_game_state.complete_mission_at_exit(*from, *to)) {
                    OZ_INFO("COOP: mission '%s' complete at its exit -> '%s'",
                            from->mission.c_str(), to->name.c_str());
                    for (const auto& np : g_game_server->players()) {
                        if (np.id == sender.id) continue;
                        ServerPlayer* other = g_game_state.get_player(np.id);
                        if (!other || other->world_index != sp->world_index) continue;
                        other->world_index = want;
                        other->position = NetVec3{ 0.0f, 0.0f, 0.0f };
                        send_world_change(const_cast<net::NetworkPlayer&>(np), want,
                                          /*accepted=*/true);
                        send_pickup_snapshot(np, want);
                    }
                }
            }

            const int previous = sp->world_index;
            sp->world_index = want;

            // A client's own position is meaningless in the world it just left, and
            // carrying it across would drop the player inside or outside geometry.
            // Reset to the origin-ish default the rest of the server assumes.
            // (ServerPlayer has no velocity field — velocity is client-side and
            // arrives with the next PLAYER_UPDATE.)
            sp->position = NetVec3{ 0.0f, 0.0f, 0.0f };

            OZ_INFO("Player %u moved world %d -> %d", sender.id, previous, want);
            send_world_change(const_cast<net::NetworkPlayer&>(sender), want,
                              /*accepted=*/true);
            // The pickup snapshot is scoped to the player's world, so it must be
            // re-sent after the move — otherwise the new world arrives empty and
            // PickupPawns has nothing to draw until the 15 s resync.
            send_pickup_snapshot(const_cast<net::NetworkPlayer&>(sender), want);
            break;
        }
        case net::MessageType::XP_UPDATE: {
            OZ_WARN("XP_UPDATE from player %u — server sends these, ignoring incoming",
                    sender.id);
            break;
        }
        case net::MessageType::PLAYER_HURT: {
            if (msg.size < sizeof(net::PlayerHurtData)) break;
            net::PlayerHurtData phd;
            memcpy(&phd, msg.payload, sizeof(phd));
            // Only the affected client may report its own world-hazard damage.
            if (phd.player_id != sender.id) {
                OZ_WARN("PLAYER_HURT from %u targeting %u — dropped",
                        sender.id, phd.player_id);
                break;
            }
            if (phd.damage < 0 || phd.damage > 100 ||
                !std::isfinite(phd.remaining_health) ||
                phd.remaining_health < 0.0f) {
                OZ_WARN("PLAYER_HURT from %u: out-of-range values dropped",
                        sender.id);
                break;
            }
            ServerPlayer* victim = g_game_state.get_player(phd.player_id);
            if (victim) {
                // Monotonic down: a client may never raise its own health.
                if (phd.remaining_health > victim->health) {
                    OZ_WARN("PLAYER_HURT from %u: health increase attempt dropped",
                            sender.id);
                    break;
                }
                victim->health = phd.remaining_health;
                OZ_INFO("PLAYER_HURT: victim=%u damage=%d health=%.0f",
                        phd.player_id, phd.damage, phd.remaining_health);
            }
            // Relay to the VICTIM ONLY. This was a broadcast, and the client
            // applies PLAYER_HURT as "you were hit" - so one player's hazard
            // damage overwrote every other player's health and played the
            // hurt/death sound on all of them. The victim is the sender (checked
            // above), so send back to it.
            net::NetworkMessage relay{};
            relay.magic = net::MAGIC;
            relay.type = static_cast<uint32_t>(net::MessageType::PLAYER_HURT);
            relay.size = sizeof(phd);
            relay.sequence = 0;
            relay.timestamp = static_cast<uint32_t>(time(nullptr));
            memcpy(relay.payload, &phd, sizeof(phd));
            g_game_server->send_message(const_cast<net::NetworkPlayer&>(sender), relay);
            break;
        }
        case net::MessageType::PLAYER_KILL: {
            if (msg.size < sizeof(net::PlayerKillData)) break;
            net::PlayerKillData pkd;
            memcpy(&pkd, msg.payload, sizeof(pkd));
            // The server is authoritative over kills. A client may only
            // report a self-inflicted kill.
            if (pkd.killer_id != sender.id || pkd.victim_id != sender.id) {
                OZ_WARN("PLAYER_KILL from %u claiming killer=%u victim=%u — dropped",
                        sender.id, pkd.killer_id, pkd.victim_id);
                break;
            }
            OZ_INFO("PLAYER_KILL: killer=%u victim=%u", pkd.killer_id, pkd.victim_id);
            ServerPlayer* victim = g_game_state.get_player(pkd.victim_id);
            if (victim) {
                victim->health = 0.0f;
            }
            // Broadcast kill to all players
            net::NetworkMessage relay;
            relay.magic = net::MAGIC;
            relay.type = static_cast<uint32_t>(net::MessageType::PLAYER_KILL);
            relay.size = sizeof(pkd);
            relay.sequence = 0;
            relay.timestamp = static_cast<uint32_t>(time(nullptr));
            memcpy(relay.payload, &pkd, sizeof(pkd));
            g_game_server->broadcast_message(relay);
            break;
        }
        case net::MessageType::PICKUP_COLLECTED: {
            // Server broadcasts its own PICKUP_COLLECTED on a successful
            // PICKUP_COLLECT; never relay client-spoofed grants.
            OZ_WARN("PICKUP_COLLECTED from player %u ignored (server-authoritative)",
                    sender.id);
            break;
        }
        case net::MessageType::NPC_DAMAGE: {
            if (msg.size < sizeof(net::NpcDamageData)) break;
            net::NpcDamageData ndd;
            memcpy(&ndd, msg.payload, sizeof(ndd));
            ndd.player_id = sender.id;

            // The attacker must exist before anything else: the world match is
            // checked against where the player actually is, not against the
            // client-claimed index. NPC_DAMAGE used to validate neither, so any
            // client could damage a same-numbered NPC in a world it was not in
            // (all worlds are authored near the origin) with no world check.
            ServerPlayer* attacker = g_game_state.get_player(sender.id);
            if (!attacker) break;
            if (ndd.world_index != attacker->world_index) {
                OZ_WARN("NPC_DAMAGE from player %u — world %d != %d", sender.id,
                        ndd.world_index, attacker->world_index);
                break;
            }

            // Find and damage the NPC
            WorldState* ws = g_game_state.get_world(attacker->world_index);
            if (!ws) break;
            ServerNPC* npc = nullptr;
            if (ndd.partition_index >= 0) {
                WorldPartition* part = g_game_state.get_partition(*ws, ndd.partition_index);
                if (part && ndd.npc_index >= 0 && ndd.npc_index < (int)part->npcs.size())
                    npc = &part->npcs[ndd.npc_index];
            } else {
                if (ndd.npc_index >= 0 && ndd.npc_index < (int)ws->global_npcs.size())
                    npc = &ws->global_npcs[ndd.npc_index];
            }
            if (!npc || !npc->active) break;
            // Validate distance (2x attack range for leeway)
            float dx = attacker->position.x - npc->position.x;
            float dy = attacker->position.y - npc->position.y;
            float dz = attacker->position.z - npc->position.z;
            float dist = sqrtf(dx*dx + dy*dy + dz*dz);
            if (dist > net::NPC_MAX_RANGE) break; // max weapon range
            // Rate-limit: max 5 damage ticks/second (2 server ticks at 10fps)
            if (attacker->last_damage_tick > 0 &&
                g_game_state.tick_count() - attacker->last_damage_tick < 2) break;
            attacker->last_damage_tick = g_game_state.tick_count();
            // Cap client-reported damage so a spoofed report cannot one-shot.
            // There was no cap at all here while MELEE_HIT had one.
            int dmg = ndd.damage;
            if (dmg < 0) dmg = 0;
            if (dmg > net::NPC_MAX_DAMAGE) dmg = net::NPC_MAX_DAMAGE;
            g_game_state.damage_npc(*npc, dmg, sender.id,
                                    oz::gametype::DamageSource::PLAYER, ws->world_index);
            // Broadcast updated NPC state
            net::NpcStateUpdateData nsud = make_npc_state(*ws, ndd.npc_index, ndd.partition_index, *npc);
            net::NetworkMessage relay;
            relay.magic = net::MAGIC;
            relay.type = static_cast<uint32_t>(net::MessageType::NPC_STATE_UPDATE);
            relay.size = sizeof(nsud);
            relay.sequence = 0;
            relay.timestamp = static_cast<uint32_t>(time(nullptr));
            memcpy(relay.payload, &nsud, sizeof(nsud));
            g_game_server->broadcast_message(relay);
            break;
        }
        case net::MessageType::MELEE_HIT: {
            if (msg.size < sizeof(net::MeleeHitData)) break;
            net::MeleeHitData mhd;
            memcpy(&mhd, msg.payload, sizeof(mhd));
            mhd.player_id = sender.id;
            ServerPlayer* attacker = g_game_state.get_player(sender.id);
            if (!attacker) break;
            // Reject a hit reported against a world the attacker is not in.
            if (mhd.world_index != attacker->world_index) {
                OZ_WARN("MELEE_HIT from player %u — world %d != %d", sender.id,
                        mhd.world_index, attacker->world_index);
                break;
            }
            WorldState* ws = g_game_state.get_world(mhd.world_index);
            if (!ws) break;
            ServerNPC* npc = nullptr;
            if (mhd.partition_index >= 0) {
                WorldPartition* part = g_game_state.get_partition(*ws, mhd.partition_index);
                if (part && mhd.npc_index >= 0 && mhd.npc_index < (int)part->npcs.size())
                    npc = &part->npcs[mhd.npc_index];
            } else {
                if (mhd.npc_index >= 0 && mhd.npc_index < (int)ws->global_npcs.size())
                    npc = &ws->global_npcs[mhd.npc_index];
            }
            if (!npc || !npc->active) break;

            // Authoritative range check against the NPC's real position, using
            // the reported reach but capped so a client cannot claim a huge
            // reach to hit across the map. The reported origin/dir are not
            // trusted for this: distance from the server's own player position
            // is the only thing that matters.
            float reach = mhd.reach;
            if (reach <= 0.0f || reach > net::MELEE_MAX_REACH) reach = net::MELEE_MAX_REACH;
            float ddx = attacker->position.x - npc->position.x;
            float ddy = attacker->position.y - npc->position.y;
            float ddz = attacker->position.z - npc->position.z;
            float dist = sqrtf(ddx*ddx + ddy*ddy + ddz*ddz);
            if (dist > reach) break;

            // Server-side stamina gate + throttle. Both ends spend on a swing,
            // so a client that skips the cost is refused here.
            float cost = mhd.stamina_cost;
            if (cost < 0.0f) cost = 0.0f;
            if (cost > attacker->stamina) {
                OZ_WARN("MELEE_HIT from player %u — stamina %.1f < cost %.1f",
                        sender.id, attacker->stamina, cost);
                break;
            }
            attacker->stamina -= cost;
            if (attacker->last_melee_tick > 0 &&
                g_game_state.tick_count() - attacker->last_melee_tick < net::MELEE_MIN_TICKS) break;
            attacker->last_melee_tick = g_game_state.tick_count();

            // Cap client-reported damage so a spoofed swing cannot one-shot.
            int dmg = mhd.damage;
            if (dmg < 0) dmg = 0;
            if (dmg > net::MELEE_MAX_DAMAGE) dmg = net::MELEE_MAX_DAMAGE;
            g_game_state.damage_npc(*npc, dmg, sender.id,
                                    oz::gametype::DamageSource::PLAYER, ws->world_index);

            net::NpcStateUpdateData nsud = make_npc_state(*ws, mhd.npc_index, mhd.partition_index, *npc);
            net::NetworkMessage relay;
            relay.magic = net::MAGIC;
            relay.type = static_cast<uint32_t>(net::MessageType::NPC_STATE_UPDATE);
            relay.size = sizeof(nsud);
            relay.sequence = 0;
            relay.timestamp = static_cast<uint32_t>(time(nullptr));
            memcpy(relay.payload, &nsud, sizeof(nsud));
            g_game_server->broadcast_message(relay);
            break;
        }
        case net::MessageType::COMMAND: {
            if (msg.size < sizeof(net::CommandData)) break;
            net::CommandData cd;
            memcpy(&cd, msg.payload, sizeof(cd));
            cd.cmd[sizeof(cd.cmd) - 1] = '\0';
            cd.args[sizeof(cd.args) - 1] = '\0';

            // Admin auth: args must start with "<admin_token> ". Fail-closed:
            // no --admin-token configured → every command rejected.
            if (g_admin_token.empty()) {
                send_syschat(sender, "[server] admin commands disabled (no --admin-token)");
                break;
            }
            size_t tokLen = g_admin_token.size();
            if (strncmp(cd.args, g_admin_token.c_str(), tokLen) != 0 ||
                (cd.args[tokLen] != ' ' && cd.args[tokLen] != '\0')) {
                OZ_WARN("COMMAND from player %u — bad admin token", sender.id);
                send_syschat(sender, "[server] authentication failed");
                break;
            }
            std::string payload(cd.args + tokLen);
            size_t argStart = payload.find_first_not_of(' ');
            payload = (argStart == std::string::npos) ? "" : payload.substr(argStart);

            std::string cmd = cd.cmd;
            for (auto& c : cmd) c = (char)tolower((unsigned char)c);

            if (cmd == "list") {
                std::string reply = "[" + std::to_string(g_game_server->player_count()) + " online:";
                for (const auto& p : g_game_server->players())
                    if (p.connected) reply += " " + std::string(p.name) + "(id=" + std::to_string(p.id) + ")";
                reply += "]";
                send_syschat(sender, reply);
            } else if (cmd == "say") {
                if (payload.empty()) { send_syschat(sender, "[server] usage: say <message>"); break; }
                std::string broadcast = "[server] " + payload;
                net::ChatData bcd{};
                strncpy(bcd.text, broadcast.c_str(), sizeof(bcd.text) - 1);
                net::NetworkMessage bmsg{};
                bmsg.magic = net::MAGIC;
                bmsg.type = static_cast<uint32_t>(net::MessageType::CHAT);
                bmsg.size = sizeof(bcd);
                bmsg.sequence = 0;
                bmsg.timestamp = static_cast<uint32_t>(time(nullptr));
                memcpy(bmsg.payload, &bcd, sizeof(bcd));
                g_game_server->broadcast_message(bmsg);
            } else if (cmd == "kick") {
                if (payload.empty()) { send_syschat(sender, "[server] usage: kick <player_id|name>"); break; }
                uint32_t targetId = 0;
                std::string targetName;
                // Numeric id or name match
                if (payload.find_first_not_of("0123456789") == std::string::npos) {
                    targetId = (uint32_t)std::stoul(payload);
                } else {
                    targetName = payload;
                    for (const auto& p : g_game_server->players()) {
                        if (p.connected && payload == p.name) { targetId = p.id; break; }
                    }
                }
                // Resolve the name before kicking (for the reply)
                std::string kickedName;
                for (const auto& p : g_game_server->players())
                    if (p.connected && p.id == targetId) { kickedName = p.name; break; }
                if (targetId != 0 && g_game_server->kick_player(targetId)) {
                    send_syschat(sender, "[server] kicked " + (kickedName.empty() ? payload : kickedName));
                } else {
                    send_syschat(sender, "[server] player not found: " + payload);
                }
            } else {
                send_syschat(sender, "[server] unknown command '" + cmd + "' (list|say|kick)");
            }
            break;
        }
        case net::MessageType::GAME_STATE: {
            OZ_WARN("GAME_STATE from player %u ignored — server owns authoritative state",
                    sender.id);
            break;
        }
        default:
            OZ_WARN("Unhandled message type %s (%u) from player %u",
                    net::message_type_string(type), (unsigned)type, sender.id);
            break;
    }
}

// ---------------------------------------------------------------------------
// Environment setup
// ---------------------------------------------------------------------------
static void scan_worlds(const std::string& dir) {
#ifdef _WIN32
    std::string pattern = dir + "/*";
    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        OZ_ERROR("scan: couldnt open %s", dir.c_str());
        return;
    }
    do {
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (ffd.cFileName[0] == '.') continue;
            std::string ozone = dir + "/" + ffd.cFileName + "/World.ozone";
            struct stat ss;
            if (stat(ozone.c_str(), &ss) == 0) {
                g_world_list.push_back(ffd.cFileName);
                OZ_INFO("Found world: %s", ffd.cFileName);
            }
        }
    } while (FindNextFileA(hFind, &ffd) != 0);
    FindClose(hFind);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) {
        OZ_ERROR("scan: couldnt open %s", dir.c_str());
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string path = dir + "/" + entry->d_name;
        struct stat s;
        if (stat(path.c_str(), &s) == 0 && S_ISDIR(s.st_mode)) {
            std::string ozone = path + "/World.ozone";
            struct stat ss;
            if (stat(ozone.c_str(), &ss) == 0) {
                g_world_list.push_back(entry->d_name);
                OZ_INFO("Found world: %s", entry->d_name);
            }
        }
    }
    closedir(d);
#endif
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    int game_port = 27015;
    int http_port = 8080;

    // Optional persisted config (useful on a VPS so the systemd unit stays short):
    //   System/OzServer.ini  [Server] port/http-port/dir/bind/server-name
    //                        [Auth]  auth-token/admin-token/public-ip
    //                        [MasterServers] Master, Master1.. (UDP or http:// URLs)
    // CLI flags and environment variables override these.
    {
        IniConfig cfg;
        if (cfg.Load("System/OzServer.ini")) {
            game_port = cfg.GetInt("Server", "port", game_port);
            http_port = cfg.GetInt("Server", "http-port", http_port);
            std::string d = cfg.Get("Server", "dir", "");
            if (!d.empty()) g_gamedata_dir = d;
            std::string b = cfg.Get("Server", "bind", "");
            if (!b.empty()) g_bind_ip = b;
            std::string sn = cfg.Get("Server", "server-name", "");
            if (!sn.empty()) g_server_name = sn;
            std::string at = cfg.Get("Auth", "auth-token", "");
            if (!at.empty()) g_auth_token = at;
            std::string ad = cfg.Get("Auth", "admin-token", "");
            if (!ad.empty()) g_admin_token = ad;
            std::string pi = cfg.Get("Auth", "public-ip", "");
            if (!pi.empty()) g_public_ip = pi;

            const char* S = "MasterServers";
            auto push = [](const std::string& v, std::vector<std::string>& udp,
                           std::vector<std::string>& http) {
                size_t start = 0;
                while (start <= v.size()) {
                    size_t comma = v.find(',', start);
                    std::string tok = (comma == std::string::npos)
                        ? v.substr(start) : v.substr(start, comma - start);
                    size_t a = tok.find_first_not_of(" \t");
                    size_t b = tok.find_last_not_of(" \t");
                    if (a != std::string::npos) tok = tok.substr(a, b - a + 1);
                    if (!tok.empty()) {
                        if (tok.rfind("http://", 0) == 0 || tok.rfind("https://", 0) == 0)
                            http.push_back(tok);
                        else
                            udp.push_back(tok);
                    }
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
            };
            push(cfg.Get(S, "Master"), g_master_udp, g_master_http);
            for (int i = 1; i <= 32; ++i) {
                char key[16];
                snprintf(key, sizeof(key), "Master%d", i);
                push(cfg.Get(S, key), g_master_udp, g_master_http);
            }
        }
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            game_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--http-port") == 0 && i + 1 < argc)
            http_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc)
            g_gamedata_dir = argv[++i];
        else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc)
            g_bind_ip = argv[++i];
        else if (strcmp(argv[i], "--auth-token") == 0 && i + 1 < argc)
            g_auth_token = argv[++i];
        else if (strcmp(argv[i], "--admin-token") == 0 && i + 1 < argc)
            g_admin_token = argv[++i];
        else if (strcmp(argv[i], "--server-name") == 0 && i + 1 < argc)
            g_server_name = argv[++i];
        else if (strcmp(argv[i], "--master") == 0 && i + 1 < argc)
            g_master_udp.push_back(argv[++i]);
        else if (strcmp(argv[i], "--master-http") == 0 && i + 1 < argc)
            g_master_http.push_back(argv[++i]);
        else if (strcmp(argv[i], "--public-ip") == 0 && i + 1 < argc)
            g_public_ip = argv[++i];
        else if (strcmp(argv[i], "--campaign") == 0)
            g_campaign = true;
        else if (strcmp(argv[i], "--world") == 0 && i + 1 < argc)
            g_world_pin = argv[++i];
        else if (strcmp(argv[i], "--mission") == 0 && i + 1 < argc) {
            g_mission = argv[++i];
            // A named mission is a campaign concept by definition: without the
            // arc flag no world carries objectives, so the pin would be inert.
            // Implying it matches the client, which sends both together.
            g_campaign = true;
        }
        else if (strcmp(argv[i], "--version") == 0) {
            printf("AngelServ %s (protocol %u)\n", oz::kBuildVersion,
                   (unsigned)net::PROTOCOL_VERSION);
            return 0;
        }
        else if (strcmp(argv[i], "--help") == 0) {
            printf("AngelServ %s (protocol %u) -- OzWorld/AngelTech dedicated server\n",
                   oz::kBuildVersion, (unsigned)net::PROTOCOL_VERSION);
            printf("Usage: AngelServ [--port P] [--http-port P] [--dir GameData]"
                   " [--bind IP|HOST] [--auth-token T] [--admin-token T]\n");
            printf("                  [--server-name NAME] [--master host[:port]]"
                   " [--master-http URL] [--public-ip IP] [--campaign]\n");
            printf("                  [--mission NAME] [--world NAME]\n");
            printf("  --bind         Interface for UDP+HTTP (default 0.0.0.0 = all).\n");
            printf("  --auth-token   Require Bearer auth for the HTTP API (env: OZ_AUTH_TOKEN)\n");
            printf("  --admin-token  Enable admin COMMANDs (list/say/kick; env: OZ_ADMIN_TOKEN)\n");
            printf("  --server-name  Display name announced to masters (default: Angels95 Server)\n");
            printf("  --master       Master UDP heartbeat target, repeatable (default port 27900)\n");
            printf("  --master-http  Master HTTP heartbeat URL, repeatable (plain http:// only)\n");
            printf("  --public-ip    Public IP to announce when behind NAT (master can override)\n");
            printf("  --campaign     Run the campaign arc: seed mission objectives, join at the\n");
            printf("                 mission frontier, advance worlds when objectives complete\n");
            printf("                 (default OFF = plain co-op: no objectives, join in world 0)\n");
            printf("  --mission NAME Start the arc at this `: mission` def's world (implies\n");
            printf("                 --campaign). Unknown name = startup error, never a\n");
            printf("                 silent fallback to world 0.\n");
            printf("  --world NAME   Plain co-op spawn world (default: world 0). Ignored\n");
            printf("                 with --campaign (the mission frontier owns placement).\n");
            printf("  Config file:   System/OzServer.ini ([Server]/[Auth]/[MasterServers]; CLI overrides)\n");
            return 0;
        }
    }
    // Env fallback (CLI/config win)
    if (g_auth_token.empty())  { const char* e = getenv("OZ_AUTH_TOKEN");  if (e) g_auth_token = e; }
    if (g_admin_token.empty()) { const char* e = getenv("OZ_ADMIN_TOKEN"); if (e) g_admin_token = e; }

    // Resolve --bind (literal IPv4, hostname, or 0.0.0.0). Empty → all interfaces.
    std::string resolved_bind;
    if (!g_bind_ip.empty() && g_bind_ip != "0.0.0.0") {
        resolved_bind = master::ResolveBindAddress(g_bind_ip);
        if (resolved_bind.empty()) {
            OZ_WARN("Cannot resolve --bind '%s' — falling back to 0.0.0.0", g_bind_ip.c_str());
        }
    }
    g_bind_ip = resolved_bind; // "" = all interfaces; literal IPv4 otherwise

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Written ONCE, here, before any thread exists: the HTTP listener thread
    // reads it on every /status request (`now() - g_server_start_time`). It
    // used to be declared and never assigned, so the default-constructed
    // epoch was subtracted and /status reported uptime as seconds-since-1970
    // (~56 years). Do not move this write after the thread spawns.
    g_server_start_time = std::chrono::steady_clock::now();

    OZ_INFO("AngelServ %s starting (proto %u)", oz::kBuildVersion,
            (unsigned)net::PROTOCOL_VERSION);
    OZ_INFO("Game port: UDP %d",  game_port);
    OZ_INFO("HTTP port: %d",     http_port);
    printf("Data dir:  %s\n",     g_gamedata_dir.c_str());
    if (g_auth_token.empty())
        OZ_WARN("HTTP API is OPEN (no --auth-token): /map /worlds /status /players are public");
    if (g_admin_token.empty())
        OZ_WARN("Admin COMMANDs disabled (no --admin-token)");

    // Campaign arc is OPT-IN. Applied BEFORE init_worlds (it gates objective
    // seeding); current_mission_world_index() and the coordinated transition
    // read the same flag later. OFF is classic co-op: one world, no objectives.
    g_game_state.set_campaign_enabled(g_campaign);
    OZ_INFO("Campaign mode: %s", g_campaign
                ? (g_mission.empty()
                       ? "ON (--campaign): mission objectives seeded, joiners start at the "
                         "mission frontier, worlds advance on completion"
                       : "ON (--campaign --mission): objectives seeded, the arc STARTS at the "
                         "named mission, worlds advance on completion")
                : "OFF (default): no objectives, joiners start in world 0; pass "
                  "--campaign to run the mission arc");
    if (!g_mission.empty())
        OZ_INFO("Requested starting mission: %s", g_mission.c_str());

    // Scan worlds
    scan_worlds(g_gamedata_dir + "/Worlds");

    // The scan order is the FILESYSTEM's, and "world 0" is what every plain
    // co-op joiner is placed in (add_player -> world_index 0). Windows'
    // FindFirstFile returns names in index order, but Linux/readdir returns
    // directory-hash order, so a server built from the same archive started in a
    // different world depending on the host — the deployed fleet kept landing in
    // Dessert_Dreams while a local Windows build landed in Aethelgard_Ruins, and
    // since the client's scene handshake follows `active_world`, every join was
    // yanked into that arbitrary world. Sort once, here, so the list is a
    // stable, documented order on every platform and the map a server reports
    // (current_map_name) is the map players actually get.
    std::sort(g_world_list.begin(), g_world_list.end());

    // Load the game hierarchy and the world state BEFORE binding anything. The
    // hierarchy is needed by GameManager (and therefore by --mission
    // validation), and failing here must not leave a started server and a live
    // HTTP thread behind — an early `return` from main with a joinable
    // std::thread aborts the process instead of exiting cleanly. It also closes
    // the old window where the HTTP thread could read g_game_state while
    // init_worlds was still filling it.
    LightningEntityRegistry::Instance().Init();
    oz::game::GameManager::Instance().Init();
    oz::game::RegisterGameBoxedTypes();
    oz::game::RegisterGameOpcodes();

    // --mission must be validated AFTER GameManager::Init (only then are the
    // `: mission` defs resolvable) and BEFORE init_worlds (it decides the
    // starting frontier). An unknown name is a startup ERROR: silently starting
    // a campaign host on a different level than the operator asked for is worse
    // than not starting, because the players have no way to tell.
    if (!g_mission.empty()) {
        auto& gm = oz::game::GameManager::Instance();
        const std::string missionWorld =
            gm.Loaded() ? gm.MissionWorld(g_mission) : std::string();
        if (missionWorld.empty()) {
            fprintf(stderr,
                    "AngelServ: --mission '%s' is unknown: no `: mission` def maps it to a "
                    "world (game hierarchy %s). Refusing to start.\n",
                    g_mission.c_str(), gm.Loaded() ? "loaded" : "FAILED TO LOAD");
            return 1;
        }
        g_mission_world = missionWorld;
        OZ_INFO("--mission '%s' -> world '%s'", g_mission.c_str(), missionWorld.c_str());
        g_game_state.set_starting_mission(g_mission);
    }

    g_game_state.init_worlds(g_gamedata_dir, g_world_list);

    // The only server -> client damage notification. GameState owns when a player
    // is hurt or respawned; this turns that into the directed PLAYER_STATE the
    // victim can actually act on. Registered here because it needs BOTH the game
    // state and the network server, and must not fire before either exists.
    g_game_state.set_on_player_state([](const ServerPlayer& sp,
                                        GameState::PlayerStateEvent ev, int damage) {
        send_player_state(sp, ev == GameState::PlayerStateEvent::Respawn, damage);
    });

    // --world is a plain-co-op placement pin. The campaign frontier owns join
    // placement, so combining them would be a silent contradiction.
    if (!g_world_pin.empty()) {
        if (g_campaign) {
            OZ_WARN("--world '%s' ignored: --campaign places joiners at the mission frontier",
                    g_world_pin.c_str());
            g_world_pin.clear();
        } else if (!g_game_state.set_default_world(g_world_pin)) {
            fprintf(stderr,
                    "AngelServ: --world '%s' is not one of the loaded worlds. Refusing to start.\n",
                    g_world_pin.c_str());
            return 1;
        } else {
            OZ_INFO("--world '%s' -> spawn world %d", g_world_pin.c_str(),
                    g_game_state.default_world_index());
        }
    }

    // Start UDP game server
    net::NetworkServer game_server;
    g_game_server = &game_server;

    if (!game_server.init(static_cast<uint16_t>(game_port), net::MAX_PLAYERS,
                          g_bind_ip.c_str())) {
        fprintf(stderr, "Failed to create game server\n");
        return 1;
    }

    game_server.set_callbacks({
        .on_player_join = on_player_join,
        .on_player_leave = on_player_leave,
        .on_message_received = on_server_message
    });

    if (!game_server.start()) {
        fprintf(stderr, "Failed to start game server\n");
        return 1;
    }

    // Start HTTP server thread
    std::thread http_thread(http_server_thread, http_port);

    // LAN discovery
    net::NetworkDiscovery discovery;
    discovery.init(g_server_name.c_str(), oz::kBuildVersion,
                   static_cast<uint16_t>(game_port));
    discovery.set_server_info(static_cast<uint16_t>(http_port), current_map_name().c_str(), false);
    discovery.start();
    g_discovery = &discovery;

    // Internet discovery: announce to configured master servers.
    master::MasterUplink master_uplink;
    if (!g_master_udp.empty() || !g_master_http.empty()) {
        master_uplink.SetMasters(g_master_udp, g_master_http, g_public_ip);
        master_uplink.SetStatus(g_server_name, static_cast<uint16_t>(game_port),
                                static_cast<uint16_t>(http_port), current_map_name(),
                                0, g_game_state.max_players(), false,
                                net::PROTOCOL_VERSION);
        master_uplink.Start();
        OZ_INFO("Master uplink: %zu UDP / %zu HTTP target(s)",
                g_master_udp.size(), g_master_http.size());
    } else {
        OZ_WARN("No --master/--master-http configured: server will not appear in internet lists");
    }

    {
        std::lock_guard<std::mutex> lock(g_print_mutex);
        printf("AngelServ ready\n");
    }

    // Main loop — fixed-timestep game tick (10 Hz), network drained each pass.
    int tick = 0;
    constexpr double kTickInterval = 0.1; // 10 Hz
    constexpr double kMaxCatchUp = 0.5;   // drop excess catch-up ticks under load
    double acc = 0.0;
    auto last_frame = std::chrono::steady_clock::now();
    while (g_running) {
        game_server.update();
        discovery.set_server_info(static_cast<uint16_t>(http_port), current_map_name().c_str(), false);
        discovery.update();

        // Refresh the master heartbeat payload ~every 300ms.
        static int master_status_ticks = 0;
        if (++master_status_ticks >= 300) {
            master_status_ticks = 0;
            master_uplink.SetStatus(g_server_name, static_cast<uint16_t>(game_port),
                                    static_cast<uint16_t>(http_port), current_map_name(),
                                    static_cast<uint32_t>(g_game_state.player_count()),
                                    g_game_state.max_players(), false,
                                    net::PROTOCOL_VERSION);
        }

        auto now = std::chrono::steady_clock::now();
        acc += std::chrono::duration<double>(now - last_frame).count();
        last_frame = now;

        while (acc >= kTickInterval) {
            acc -= kTickInterval;
            g_game_state.tick(static_cast<float>(kTickInterval));

            // Mission completion is SPATIAL, not a countdown: a campaign world
            // is marked complete when a player leaves it through the door that
            // leads to the mission's `next` world (the REQUEST_WORLD handler
            // below). This loop used to advance the whole server the instant
            // every objective was ticked, which fired after ~5 of a map's
            // 12/18/19 NPCs and teleported players out of the middle of the
            // level: the arcs the maps are built around (the shrine, the tower
            // climb, the annex) were never reached and the authored exit
            // portals were decoys.
            //
            // Objectives still advance (damage_npc / collect_pickup) and are
            // broadcast as OBJECTIVE_STATE. They are the route's progress
            // read-out; they do not end the mission.

            // Broadcast pickups that just respawned to all clients
            {
                auto respawned = g_game_state.consume_respawned_pickups();
                for (auto& rp : respawned) {
                    WorldState* ws = g_game_state.get_world(rp.world_index);
                    if (!ws) continue;
                    ServerPickup* pickup = nullptr;
                    for (auto& part : ws->partitions) {
                        for (auto& p : part.pickups) {
                            if (p.id == rp.pickup_id) { pickup = &p; break; }
                        }
                        // Was missing: the outer loop kept scanning every later
                        // partition and the *last* match won.
                        if (pickup) break;
                    }
                    if (!pickup)
                        for (auto& p : ws->global_pickups)
                            if (p.id == rp.pickup_id) { pickup = &p; break; }
                    if (!pickup || !pickup->active) continue;
                    net::PickupRespawnData prd = make_pickup_respawn(*pickup, rp.world_index);
                    net::NetworkMessage msg{};
                    msg.magic = net::MAGIC;
                    msg.type = static_cast<uint32_t>(net::MessageType::PICKUP_RESPAWN);
                    msg.size = sizeof(prd);
                    msg.sequence = 0;
                    msg.timestamp = static_cast<uint32_t>(time(nullptr));
                    memcpy(msg.payload, &prd, sizeof(prd));
                    g_game_server->broadcast_message(msg);
                }
            }

            // Periodic pickup re-sync, per player, for that player's own world.
            //
            // Pickups are otherwise only sent on join and on respawn, and UDP
            // gives no delivery guarantee: one lost PICKUP_RESPAWN left the
            // client drawing a pickup the server had no record of, permanently
            // uncollectable, with no error anywhere. 15 s is slow enough to be
            // negligible traffic (~15 x 160 bytes per player) and fast enough
            // that a lost packet costs a rounding error rather than a session.
            {
                static double lastPickupResync = 0.0;
                constexpr double kPickupResyncInterval = 15.0;
                double tnow = static_cast<double>(time(nullptr));
                if (tnow - lastPickupResync >= kPickupResyncInterval) {
                    lastPickupResync = tnow;
                    for (const auto& p : g_game_server->players()) {
                        if (!p.connected) continue;
                        const ServerPlayer* sp = g_game_state.get_player(p.id);
                        if (!sp) continue;
                        send_pickup_snapshot(p, sp->world_index);
                    }
                }
            }

            // Broadcast score state (throttled to ~1 Hz, or immediately on change)
            {
                static double lastScoreBroadcast = 0.0;
                double now = static_cast<double>(time(nullptr));
                bool matchOver = g_game_state.match_over();
                if (now - lastScoreBroadcast >= 1.0 || matchOver) {
                    for (const auto& p : g_game_state.players()) {
                        if (!p.connected) continue;
                        net::ScoreStateData ss{};
                        ss.player_id = p.id;
                        ss.score = p.score;
                        ss.team = p.team;
                        ss.winningTeam = matchOver ? g_game_state.winning_team() : -1;
                        ss.matchOver = matchOver ? 1 : 0;
                        net::NetworkMessage msg{};
                        msg.magic = net::MAGIC;
                        msg.type = static_cast<uint32_t>(net::MessageType::SCORE_STATE);
                        msg.size = sizeof(ss);
                        msg.sequence = 0;
                        msg.timestamp = static_cast<uint32_t>(now);
                        memcpy(msg.payload, &ss, sizeof(ss));
                        // Look up the wire-level player to send to.
                        for (const auto& np : g_game_server->players()) {
                            if (np.id != p.id) continue;
                            g_game_server->send_message(const_cast<net::NetworkPlayer&>(np), msg);
                            break;
                        }
                    }

                    // COOP objectives: one message per objective, per world. The
                    // client filters by world_index; periodic send self-heals a
                    // lost datagram (same reasoning as SCORE_STATE).
                    for (const auto& ws : g_game_state.worlds()) {
                        for (const auto& o : ws.objectives) {
                            net::ObjectiveStateData os{};
                            os.world_index = ws.world_index;
                            os.progress = o.progress;
                            os.target = o.target;
                            strncpy(os.id, o.id.c_str(), sizeof(os.id) - 1);
                            net::NetworkMessage msg{};
                            msg.magic = net::MAGIC;
                            msg.type = static_cast<uint32_t>(net::MessageType::OBJECTIVE_STATE);
                            msg.size = sizeof(os);
                            msg.sequence = 0;
                            msg.timestamp = static_cast<uint32_t>(now);
                            memcpy(msg.payload, &os, sizeof(os));
                            for (const auto& np : g_game_server->players()) {
                                ServerPlayer* sp = g_game_state.get_player(np.id);
                                if (sp && sp->world_index != ws.world_index) continue;
                                net::NetworkPlayer npp = np;
                                g_game_server->send_message(npp, msg);
                            }
                        }
                    }
                    if (matchOver) {
                        std::string text = g_game_state.winning_team() >= 0
                            ? "Match ended: Team " + std::to_string(g_game_state.winning_team()) + " wins!"
                            : "Match ended: Draw";
                        net::ChatData cd;
                        strncpy(cd.text, text.c_str(), sizeof(cd.text) - 1);
                        net::NetworkMessage msg{};
                        msg.magic = net::MAGIC;
                        msg.type = static_cast<uint32_t>(net::MessageType::CHAT);
                        msg.size = sizeof(cd);
                        msg.sequence = 0;
                        msg.timestamp = static_cast<uint32_t>(now);
                        memcpy(msg.payload, &cd, sizeof(cd));
                        g_game_server->broadcast_message(msg);
                    }
                    lastScoreBroadcast = now;
                }
            }

            // Broadcast NPC state to all players every 4 ticks (2.5/sec)
            if (tick % 4 == 0) {
                for (const auto& world : g_game_state.worlds()) {
                    // Global NPCs
                    for (size_t i = 0; i < world.global_npcs.size(); i++) {
                        const auto& n = world.global_npcs[i];
                        // Inactive (dead) NPCs are broadcast too so clients
                        // hide them and late joiners learn the state.
                        net::NpcStateUpdateData nsud = make_npc_state(world, static_cast<int>(i), -1, n);
                        net::NetworkMessage bmsg;
                        bmsg.magic = net::MAGIC;
                        bmsg.type = static_cast<uint32_t>(net::MessageType::NPC_STATE_UPDATE);
                        bmsg.size = sizeof(nsud);
                        bmsg.sequence = 0;
                        bmsg.timestamp = static_cast<uint32_t>(time(nullptr));
                        memcpy(bmsg.payload, &nsud, sizeof(nsud));
                        for (const auto& np : g_game_server->players()) {
                            ServerPlayer* sp = g_game_state.get_player(np.id);
                            if (sp && sp->world_index != world.world_index) continue;
                            g_game_server->send_message(np, bmsg);
                        }
                    }
                    // Partition NPCs
                    for (const auto& part : world.partitions) {
                        for (size_t i = 0; i < part.npcs.size(); i++) {
                            const auto& n = part.npcs[i];
                            // Inactive (dead) NPCs are broadcast too (see above).
                            net::NpcStateUpdateData nsud = make_npc_state(world, static_cast<int>(i), part.id, n);
                            net::NetworkMessage bmsg;
                            bmsg.magic = net::MAGIC;
                            bmsg.type = static_cast<uint32_t>(net::MessageType::NPC_STATE_UPDATE);
                            bmsg.size = sizeof(nsud);
                            bmsg.sequence = 0;
                            bmsg.timestamp = static_cast<uint32_t>(time(nullptr));
                            memcpy(bmsg.payload, &nsud, sizeof(nsud));
                            for (const auto& np : g_game_server->players()) {
                                ServerPlayer* sp = g_game_state.get_player(np.id);
                                if (sp && sp->world_index != world.world_index) continue;
                                g_game_server->send_message(np, bmsg);
                            }
                        }
                    }
                }
            }

            if (++tick % 30 == 0 && g_game_server->player_count() > 0) {
                auto& players = g_game_server->players();
                printf("Players online: %u\n", g_game_server->player_count());
                for (const auto& p : players) {
                    if (p.connected)
                        printf("  %s [%s:%d]\n", p.name, p.ip_address, p.port);
                }
            }
        }
        if (acc > kMaxCatchUp) acc = kMaxCatchUp; // spiral-of-death guard

        // Nap to avoid busy-spinning; sub-tick latency is preserved since the
        // receive loop (game_server.update) still drains every iteration.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    printf("Shutting down...\n");
    g_discovery = nullptr;
    discovery.stop();
    g_game_server->stop();   // fires on_player_leave for everyone
    master_uplink.Stop();
    g_game_state.save_all_worlds();  // persist world + player state
    http_thread.join();

    return 0;
}
