// AngelServ – Dedicated server for OmegaTech / OzWorld
// Provides UDP game server + HTTP map API + LAN discovery.
// Build: g++ -O3 --std=c++20 -fPIC -lpthread -lm
// Usage: AngelServ [--port P] [--http-port P] [--dir GameData]

#include "../Network/Network.hpp"
#include "../World/OzoneParser.hpp"
#include "GameState.hpp"
#include "../Game/GameManager.hpp"
#include "../Script/LightningEntityRegistry.hpp"
#include "../Log.hpp"
#include "../IniConfig.hpp"
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

std::string current_map_name() {
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

// Re-send every ACTIVE pickup in one world to a single player. This is the
// self-heal path: pickups are otherwise only sent on join and on respawn, so a
// single lost UDP datagram left a pickup drawn on the floor that could never be
// collected again for the rest of the session.
//
// Scoped to ONE world on purpose — the join snapshot walked every loaded world
// (all shipped worlds are authored near the origin), which is a lot of traffic
// for state the client cannot use. Each player gets the world it is standing in.
static void send_pickup_snapshot(const net::NetworkPlayer& player, int world_index) {
    int synced = 0;
    g_game_state.for_each_active_pickup(world_index, [&](WorldState& ws, ServerPickup& p) {
        (void)ws;
        send_pickup_respawn_msg(player, world_index, p);
        synced++;
    });
    if (synced)
        OZ_INFO("Re-synced %d pickup(s) in world %d to player %s",
                synced, world_index, player.name);
}

// World-list JSON for a joining player, clamped to the network payload size so
// a long world list can never overflow the fixed-size message buffer.
// Includes the server's active world (world 0) so the client can switch to it.
static void send_join_world_list(const net::NetworkPlayer& player) {
    constexpr size_t kMaxPayload = net::MAX_MESSAGE_SIZE;
    std::string active_world = g_world_list.empty() ? std::string() : g_world_list[0];
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
static void send_world_change(net::NetworkPlayer& player, int world_index, bool accepted) {
    net::WorldChangeData wcd{};
    wcd.world_index = world_index;
    wcd.accepted = accepted ? 1 : 0;
    wcd.spawnX = 0.0f;
    wcd.spawnY = 0.0f;
    wcd.spawnZ = 0.0f;
    // Authoritative spawn (first playerstart) so the client arrives on the floor
    // instead of the origin.
    if (world_index >= 0 && world_index < g_game_state.world_count()) {
        const auto& ws = g_game_state.worlds()[(size_t)world_index];
        if (ws.has_spawn) {
            wcd.spawnX = ws.spawn_x;
            wcd.spawnY = ws.spawn_y;
            wcd.spawnZ = ws.spawn_z;
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

static void on_player_join(net::NetworkPlayer& player) {
    OZ_INFO("Player %s (id=%u) joined from %s:%u",
            player.name, player.id, player.ip_address, player.port);
    // Idempotent: re-auth/re-ack may replay this for an existing player.
    // player.name is already sanitised and de-duplicated by the network layer,
    // so it is safe to use as a persistent identity key.
    g_game_state.add_player(player.id, player.name,
                            player.profile.team, player.profile.requestedTeam);
    if (g_discovery)
        g_discovery->set_player_count((uint32_t)g_game_state.player_count(), net::MAX_PLAYERS);

    send_join_world_list(player);

    // Sync active pickups. One world per player (the one they are in) rather
    // than every loaded world — see send_pickup_snapshot for why, and the
    // periodic re-sync in the main loop for the lost-datagram case.
    ServerPlayer* sp = g_game_state.get_player(player.id);
    const int wi = sp ? sp->world_index : 0;

    // Tell the client its authoritative world assignment before the pickup snapshot
    // that is scoped to it. Order matters: the client derives its own world index
    // from this message, and a pickup for world N arriving before the client knows
    // it is in world N is filtered out by the pickup draw path.
    send_world_change(player, wi, /*accepted=*/true);

    send_pickup_snapshot(player, wi);
}

static void on_player_leave(net::NetworkPlayer& player) {
    printf("Player %s left\n", player.name);
    g_game_state.remove_player(player.id);
    if (g_discovery)
        g_discovery->set_player_count((uint32_t)g_game_state.player_count(), net::MAX_PLAYERS);

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
                                    // Get magazine size from weapon def (default 12 for automag)
                                    int mag = 12;
                                    if (strcmp(weapon_def_out, "automag") == 0) mag = 12;
                                    else if (strcmp(weapon_def_out, "selenite_blade") == 0) mag = 0;
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
            // Spawn server-side projectile
            ServerPlayer* sp = g_game_state.get_player(sender.id);
            if (sp) {
                WorldState* ws = g_game_state.get_world(sp->world_index);
                if (ws) {
                    NetVec3 origin = {wfd.origin_x, wfd.origin_y, wfd.origin_z};
                    NetVec3 dir = {wfd.dir_x, wfd.dir_y, wfd.dir_z};
                    float speed = 20.0f;
                    float lifetime = 2.0f;
                    float damage = (float)wfd.power;
                    g_game_state.spawn_projectile(*ws, sender.id, origin, dir, speed, damage, lifetime);
                }
            }
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
            if (msg.size < sizeof(net::SceneUpdateData)) break;
            OZ_INFO("SCENE_UPDATE from player %u (data_size=%u) — relaying",
                    sender.id, msg.size);
            g_game_server->broadcast_message(msg);
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
            // Broadcast hurt event to all players (for HUD/audio feedback)
            net::NetworkMessage relay;
            relay.magic = net::MAGIC;
            relay.type = static_cast<uint32_t>(net::MessageType::PLAYER_HURT);
            relay.size = sizeof(phd);
            relay.sequence = 0;
            relay.timestamp = static_cast<uint32_t>(time(nullptr));
            memcpy(relay.payload, &phd, sizeof(phd));
            g_game_server->broadcast_message(relay);
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
        else if (strcmp(argv[i], "--help") == 0) {
            printf("AngelServ -- OzWorld/OmegaTech dedicated server\n");
            printf("Usage: AngelServ [--port P] [--http-port P] [--dir GameData]"
                   " [--bind IP|HOST] [--auth-token T] [--admin-token T]\n");
            printf("                  [--server-name NAME] [--master host[:port]]"
                   " [--master-http URL] [--public-ip IP]\n");
            printf("  --bind         Interface for UDP+HTTP (default 0.0.0.0 = all).\n");
            printf("  --auth-token   Require Bearer auth for the HTTP API (env: OZ_AUTH_TOKEN)\n");
            printf("  --admin-token  Enable admin COMMANDs (list/say/kick; env: OZ_ADMIN_TOKEN)\n");
            printf("  --server-name  Display name announced to masters (default: Angels95 Server)\n");
            printf("  --master       Master UDP heartbeat target, repeatable (default port 27900)\n");
            printf("  --master-http  Master HTTP heartbeat URL, repeatable (plain http:// only)\n");
            printf("  --public-ip    Public IP to announce when behind NAT (master can override)\n");
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

    OZ_INFO("AngelServ build b58 starting");
    OZ_INFO("Game port: UDP %d",  game_port);
    OZ_INFO("HTTP port: %d",     http_port);
    printf("Data dir:  %s\n",     g_gamedata_dir.c_str());
    if (g_auth_token.empty())
        OZ_WARN("HTTP API is OPEN (no --auth-token): /map /worlds /status /players are public");
    if (g_admin_token.empty())
        OZ_WARN("Admin COMMANDs disabled (no --admin-token)");

    // Scan worlds
    scan_worlds(g_gamedata_dir + "/Worlds");

    // Start UDP game server
    net::NetworkServer game_server;
    g_game_server = &game_server;

    if (!game_server.init(static_cast<uint16_t>(game_port))) {
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
    discovery.init("Angels95", "0.2.1", static_cast<uint16_t>(game_port));
    discovery.start();
    g_discovery = &discovery;

    // Internet discovery: announce to configured master servers.
    master::MasterUplink master_uplink;
    if (!g_master_udp.empty() || !g_master_http.empty()) {
        master_uplink.SetMasters(g_master_udp, g_master_http, g_public_ip);
        master_uplink.SetStatus(g_server_name, static_cast<uint16_t>(game_port),
                                static_cast<uint16_t>(http_port), current_map_name(),
                                0, net::MAX_PLAYERS, false);
        master_uplink.Start();
        OZ_INFO("Master uplink: %zu UDP / %zu HTTP target(s)",
                g_master_udp.size(), g_master_http.size());
    } else {
        OZ_WARN("No --master/--master-http configured: server will not appear in internet lists");
    }

    // Initialize game state with worlds. The game hierarchy (GameData/Game) is
    // loaded first so init_worlds can seed each world's COOP objectives from its
    // `: mission` def.
    LightningEntityRegistry::Instance().Init();
    oz::game::GameManager::Instance().Init();
    g_game_state.init_worlds(g_gamedata_dir, g_world_list);
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
        discovery.update();

        // Refresh the master heartbeat payload ~every 300ms.
        static int master_status_ticks = 0;
        if (++master_status_ticks >= 300) {
            master_status_ticks = 0;
            master_uplink.SetStatus(g_server_name, static_cast<uint16_t>(game_port),
                                    static_cast<uint16_t>(http_port), current_map_name(),
                                    static_cast<uint32_t>(g_game_state.player_count()),
                                    net::MAX_PLAYERS, false);
        }

        auto now = std::chrono::steady_clock::now();
        acc += std::chrono::duration<double>(now - last_frame).count();
        last_frame = now;

        while (acc >= kTickInterval) {
            acc -= kTickInterval;
            g_game_state.tick(static_cast<float>(kTickInterval));

            // Coordinated COOP mission transition: when a world's objectives are
            // all complete, move every player in that world to the mission's
            // `next` world (GameManager graph). Fires once per world.
            {
                auto& gm = oz::game::GameManager::Instance();
                if (gm.Loaded()) {
                    for (auto& ws : g_game_state.worlds()) {
                        if (ws.mission_transitioned || ws.objectives.empty()) continue;
                        bool allDone = true;
                        for (const auto& o : ws.objectives)
                            if (o.target > 0 && o.progress < o.target) { allDone = false; break; }
                        if (!allDone) continue;
                        ws.mission_transitioned = true;
                        const std::string nextWorld = gm.NextMissionWorld(ws.mission);
                        if (nextWorld.empty()) continue;
                        int nextIdx = -1;
                        for (int i = 0; i < g_game_state.world_count(); i++)
                            if (g_game_state.worlds()[(size_t)i].name == nextWorld) { nextIdx = i; break; }
                        if (nextIdx < 0) continue;
                        for (const auto& np : g_game_server->players()) {
                            ServerPlayer* sp = g_game_state.get_player(np.id);
                            if (!sp || sp->world_index != ws.world_index) continue;
                            sp->world_index = nextIdx;
                            send_world_change(const_cast<net::NetworkPlayer&>(np), nextIdx, true);
                        }
                        OZ_INFO("COOP: world '%s' objectives complete -> advancing to '%s'",
                                ws.name.c_str(), nextWorld.c_str());
                    }
                }
            }

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
                    for (auto& p : g_game_state.players()) {
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
                        g_game_server->broadcast_message(msg);
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
                            g_game_server->broadcast_message(msg);
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
                        g_game_server->broadcast_message(bmsg);
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
                            g_game_server->broadcast_message(bmsg);
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
