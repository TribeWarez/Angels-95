#include "Client.hpp"
#include "../Log.hpp"
#include "../Pawn/PickupItems.hpp"
#include "../PlayerProfile.hpp"
#include <cstring>
#include <cstdio>
#include <mutex>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <algorithm>

static double now_seconds() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

static uint32_t id_to_color_packed(uint32_t id) {
    uint32_t c = id * 1234567u;
    return (c & 0xFF) | ((c >> 8) & 0xFF00) | ((c >> 16) & 0xFF0000) | 0xFF000000u;
}

bool OmegaClient::connect(const char* ip, uint16_t port) {
    // Present the active player profile to the server (protocol v2). Read here
    // rather than from the caller so the profile always reflects whatever the
    // title menu last committed.
    {
        const PlayerProfile& p = PlayerProfileManager::Instance().Active();
        m_client.set_profile(p.displayName, p.modelPath, p.voiceSet, p.team);
    }

    net::ClientCallbacks cbs;
    cbs.on_connected = [this]() { this->on_connected(); };
    cbs.on_disconnected = [this]() { this->on_disconnected(); };
    cbs.on_message_received = [this](const net::NetworkMessage& msg) {
        this->handle_message(msg);
    };
    m_client.set_callbacks(std::move(cbs));

    bool ok = m_client.connect(ip, port);
    if (!ok) {
        OZ_ERROR("OmegaClient: failed to connect to %s:%u", ip, port);
    }
    return ok;
}

void OmegaClient::disconnect() {
    // The accepted profile belongs to the session that just ended.
    m_client.clear_server_profile();
    m_client.disconnect();
}

// The identity the server actually assigned. Differs from the local profile
// whenever the server sanitised or de-duplicated the name.
const net::ServerProfile& OmegaClient::server_profile() const {
    return m_client.server_profile();
}
bool OmegaClient::has_server_profile() const {
    return m_client.has_server_profile();
}

void OmegaClient::update(float cam_x, float cam_y, float cam_z,
                         float cam_yaw, float cam_pitch, uint8_t stance) {
    m_client.update();

    if (!m_client.is_connected()) return;

    // Send player position update (with full state data for server sync)
    net::PlayerUpdateData pud;
    pud.player_id = 0;
    pud.position = {cam_x, cam_y, cam_z};
    pud.yaw = cam_yaw;
    pud.pitch = cam_pitch;
    pud.health = 100.0f;
    pud.mana = 0.0f;
    pud.psychic_energy = 0.0f;
    pud.level = m_level;
    pud.xp = m_xp;
    memset(pud.inventory, 0, sizeof(pud.inventory));
    pud.stance = stance;

    net::NetworkMessage msg;
    msg.magic = net::MAGIC;
    msg.type = static_cast<uint32_t>(net::MessageType::PLAYER_UPDATE);
    msg.size = sizeof(pud);
    msg.sequence = 0;
    msg.timestamp = static_cast<uint32_t>(time(nullptr));
    std::memcpy(msg.payload, &pud, sizeof(pud));

    m_client.send_message(msg);

    // Interpolate NPC render transforms between server snapshots (2.5 Hz).
    static constexpr double kNpcInterpInterval = 0.4; // 1 / 2.5 Hz
    double now = now_seconds();
    for (auto& n : m_npcs) {
        if (!n.has_snapshot || !n.active) continue;
        double t = (now - n.snapshot_time) / kNpcInterpInterval;
        if (t < 0.0) t = 0.0;
        else if (t > 1.0) t = 1.0;
        float f = static_cast<float>(t);
        n.position.x = n.prev.x + (n.target.x - n.prev.x) * f;
        n.position.y = n.prev.y + (n.target.y - n.prev.y) * f;
        n.position.z = n.prev.z + (n.target.z - n.prev.z) * f;
        float dyaw = n.yaw_target - n.yaw_prev;
        while (dyaw > 3.14159265f) dyaw -= 2.0f * 3.14159265f;
        while (dyaw < -3.14159265f) dyaw += 2.0f * 3.14159265f;
        n.yaw = n.yaw_prev + dyaw * f;
    }
}

void OmegaClient::send_chat(const char* text) {
    if (!m_client.is_connected()) return;

    net::ChatData cd;
    std::strncpy(cd.text, text, sizeof(cd.text) - 1);
    cd.text[sizeof(cd.text) - 1] = '\0';

    net::NetworkMessage msg;
    msg.magic = net::MAGIC;
    msg.type = static_cast<uint32_t>(net::MessageType::CHAT);
    msg.size = static_cast<uint32_t>(std::strlen(cd.text) + 1);
    msg.sequence = 0;
    msg.timestamp = static_cast<uint32_t>(time(nullptr));
    std::memcpy(msg.payload, cd.text, msg.size);

    m_client.send_message(msg);
}

void OmegaClient::send_pickup_collect(int pickup_id, int world_index, const char* weapon_def_name) {
    if (!m_client.is_connected()) return;

    net::PickupCollectData pcd;
    pcd.player_id = 0; // server knows player id
    pcd.pickup_id = pickup_id;
    pcd.world_index = world_index;
    memset(pcd.weapon_def_name, 0, sizeof(pcd.weapon_def_name));
    if (weapon_def_name) {
        strncpy(pcd.weapon_def_name, weapon_def_name, sizeof(pcd.weapon_def_name) - 1);
    }
    {
        const std::lock_guard<std::mutex> lock(m_msg_mutex);
        // Track this request so the PICKUP_COLLECTED reply is applied. Bounded
        // so a server that never replies cannot grow this without limit.
        if (m_pending_collects.size() >= 32) m_pending_collects.erase(m_pending_collects.begin());
        m_pending_collects.emplace_back(pickup_id, world_index);
    }

    net::NetworkMessage msg;
    msg.magic = net::MAGIC;
    msg.type = static_cast<uint32_t>(net::MessageType::PICKUP_COLLECT);
    msg.size = sizeof(pcd);
    msg.sequence = 0;
    msg.timestamp = static_cast<uint32_t>(time(nullptr));
    std::memcpy(msg.payload, &pcd, sizeof(pcd));

    m_client.send_message(msg);
}

void OmegaClient::send_weapon_fire(float ox, float oy, float oz,
                                   float dx, float dy, float dz,
                                   int weapon_type, int power) {
    if (!m_client.is_connected()) return;

    net::WeaponFireData wfd;
    wfd.player_id = 0;
    wfd.origin_x = ox; wfd.origin_y = oy; wfd.origin_z = oz;
    wfd.dir_x = dx; wfd.dir_y = dy; wfd.dir_z = dz;
    wfd.weapon_type = weapon_type;
    wfd.power = power;

    net::NetworkMessage msg;
    msg.magic = net::MAGIC;
    msg.type = static_cast<uint32_t>(net::MessageType::PLAYER_ACTION);
    msg.size = sizeof(wfd);
    msg.sequence = 0;
    msg.timestamp = static_cast<uint32_t>(time(nullptr));
    std::memcpy(msg.payload, &wfd, sizeof(wfd));

    m_client.send_message(msg);
}

void OmegaClient::send_melee_hit(int world_index, int npc_index, int partition_index,
                                 int damage, float reach, float stamina_cost,
                                 float ox, float oy, float oz,
                                 float dx, float dy, float dz) {
    if (!m_client.is_connected()) return;
    net::MeleeHitData mhd;
    mhd.player_id = 0;  // server stamps the real sender
    mhd.world_index = world_index;
    mhd.npc_index = npc_index;
    mhd.partition_index = partition_index;
    mhd.damage = damage;
    mhd.reach = reach;
    mhd.stamina_cost = stamina_cost;
    mhd.origin_x = ox; mhd.origin_y = oy; mhd.origin_z = oz;
    mhd.dir_x = dx;    mhd.dir_y = dy;    mhd.dir_z = dz;
    net::NetworkMessage msg;
    msg.magic = net::MAGIC;
    msg.type = static_cast<uint32_t>(net::MessageType::MELEE_HIT);
    msg.size = sizeof(mhd);
    msg.sequence = 0;
    msg.timestamp = static_cast<uint32_t>(time(nullptr));
    std::memcpy(msg.payload, &mhd, sizeof(mhd));
    m_client.send_message(msg);
}

void OmegaClient::send_npc_damage(int world_index, int npc_index, int partition_index, int damage) {
    if (!m_client.is_connected()) return;
    net::NpcDamageData ndd;
    ndd.player_id = 0;
    ndd.world_index = world_index;
    ndd.npc_index = npc_index;
    ndd.partition_index = partition_index;
    ndd.damage = damage;
    net::NetworkMessage msg;
    msg.magic = net::MAGIC;
    msg.type = static_cast<uint32_t>(net::MessageType::NPC_DAMAGE);
    msg.size = sizeof(ndd);
    msg.sequence = 0;
    msg.timestamp = static_cast<uint32_t>(time(nullptr));
    std::memcpy(msg.payload, &ndd, sizeof(ndd));
    m_client.send_message(msg);
}

void OmegaClient::send_weapon_ammo(int slot, int ammo, int magazine, int action) {
    if (!m_client.is_connected()) return;

    net::WeaponAmmoData wad;
    wad.player_id = 0;
    wad.slot = slot;
    wad.ammo = ammo;
    wad.magazine = magazine;
    wad.action = action; // 0=fire, 1=reload, 2=sync

    net::NetworkMessage msg;
    msg.magic = net::MAGIC;
    msg.type = static_cast<uint32_t>(net::MessageType::WEAPON_AMMO);
    msg.size = sizeof(wad);
    msg.sequence = 0;
    msg.timestamp = static_cast<uint32_t>(time(nullptr));
    std::memcpy(msg.payload, &wad, sizeof(wad));

    m_client.send_message(msg);
}

void OmegaClient::handle_message(const net::NetworkMessage& msg) {
    auto type = static_cast<net::MessageType>(msg.type);
    // Set by the pickup handlers, acted on AFTER the switch. Each case scopes
    // its own lock_guard, so by the time we get here the mutex is free and the
    // callback can safely call back into the client (it takes the lock again to
    // build its snapshot).
    bool pickups_changed = false;

    switch (type) {
        case net::MessageType::CHAT: {
            if (msg.size == 0) break;
            const std::lock_guard<std::mutex> lock(m_msg_mutex);
            // Bound the read to the declared size and clamp to the chat buffer;
            // stop at the first NUL so an unterminated payload can't over-read.
            const char* p = reinterpret_cast<const char*>(msg.payload);
            size_t n = std::min<size_t>(msg.size, sizeof(net::ChatData) - 1);
            size_t len = 0;
            while (len < n && p[len] != '\0') ++len;
            m_chat_msg.assign(p, len);
            if (m_on_chat_received) m_on_chat_received(m_chat_msg);
            break;
        }
        case net::MessageType::SCENE_UPDATE: {
            if (msg.size == 0) break;
            const std::lock_guard<std::mutex> lock(m_msg_mutex);
            // msg.size is attacker-controlled; never read past the payload buf.
            size_t n = std::min<size_t>(msg.size, net::MAX_MESSAGE_SIZE);
            m_pending_scene.assign(reinterpret_cast<const char*>(msg.payload), n);
            OZ_INFO("Client: SCENE_UPDATE received (%zu bytes)", n);
            if (m_on_scene_received) m_on_scene_received(m_pending_scene);
            break;
        }
        case net::MessageType::NPC_STATE_UPDATE: {
            if (msg.size < sizeof(net::NpcStateUpdateData)) return;
            const std::lock_guard<std::mutex> lock(m_msg_mutex);
            net::NpcStateUpdateData nsud;
            memcpy(&nsud, msg.payload, sizeof(nsud));
            double snaptime = now_seconds();
            bool found = false;
            for (auto& n : m_npcs) {
                if (n.npc_index == nsud.npc_index &&
                    n.world_index == nsud.world_index &&
                    n.partition_index == nsud.partition_index) {
                    n.prev = n.position;      // last interpolated point -> continuity
                    n.yaw_prev = n.yaw;
                    n.target = nsud.position;
                    n.yaw_target = nsud.yaw;
                    n.snapshot_time = snaptime;
                    n.has_snapshot = true;
                    n.state = nsud.state;
                    n.health = nsud.health;
                    n.active = nsud.active;
                    if (nsud.npc_type[0]) strncpy(n.npc_type, nsud.npc_type, sizeof(n.npc_type) - 1);
                    n.npc_type[sizeof(n.npc_type) - 1] = '\0';
                    found = true;
                    break;
                }
            }
            if (!found) {
                ClientNPC cn;
                cn.world_index = nsud.world_index;
                cn.npc_index = nsud.npc_index;
                cn.partition_index = nsud.partition_index;
                cn.prev = nsud.position;
                cn.target = nsud.position;
                cn.position = nsud.position;
                cn.yaw = nsud.yaw;
                cn.yaw_prev = nsud.yaw;
                cn.yaw_target = nsud.yaw;
                cn.snapshot_time = snaptime;
                cn.has_snapshot = true;
                cn.state = nsud.state;
                cn.health = nsud.health;
                cn.active = nsud.active;
                strncpy(cn.npc_type, nsud.npc_type, sizeof(cn.npc_type) - 1);
                cn.npc_type[sizeof(cn.npc_type) - 1] = '\0';
                m_npcs.push_back(cn);
            }
            break;
        }
        case net::MessageType::PICKUP_RESPAWN: {
            // Accept the base layout and treat a missing typeName as empty: the
            // tail is optional (see kPickupRespawnSizeBase), so requiring
            // sizeof() here would silently drop every message from a server that
            // predates the field.
            if (msg.size < net::kPickupRespawnSizeBase) return;
            const std::lock_guard<std::mutex> lock(m_msg_mutex);
            net::PickupRespawnData prd;
            memset(&prd, 0, sizeof(prd));
            memcpy(&prd, msg.payload, std::min<size_t>(msg.size, sizeof(prd)));
            // Stop at the first NUL so a truncated/unterminated tail cannot run
            // off the struct (same rule as the CHAT payload read).
            prd.typeName[sizeof(prd.typeName) - 1] = '\0';
            bool found = false;
            for (auto& p : m_pickups) {
                if (p.id == prd.pickup_id && p.world_index == prd.world_index) {
                    p.active = true;
                    p.position = prd.position;
                    p.type = prd.type;
                    p.value = prd.value;
                    strncpy(p.weapon_def_name, prd.weapon_def_name, sizeof(p.weapon_def_name) - 1);
                    p.weapon_def_name[sizeof(p.weapon_def_name) - 1] = '\0';
                    strncpy(p.typeName, prd.typeName, sizeof(p.typeName) - 1);
                    p.typeName[sizeof(p.typeName) - 1] = '\0';
                    found = true;
                    break;
                }
            }
            if (!found) {
                ClientPickup cp;
                cp.id = prd.pickup_id;
                cp.world_index = prd.world_index;
                cp.position = prd.position;
                cp.type = prd.type;
                cp.value = prd.value;
                cp.active = true;
                strncpy(cp.weapon_def_name, prd.weapon_def_name, sizeof(cp.weapon_def_name) - 1);
                cp.weapon_def_name[sizeof(cp.weapon_def_name) - 1] = '\0';
                strncpy(cp.typeName, prd.typeName, sizeof(cp.typeName) - 1);
                cp.typeName[sizeof(cp.typeName) - 1] = '\0';
                m_pickups.push_back(cp);
            }
            pickups_changed = true;
            break;
        }
        case net::MessageType::PICKUP_COLLECTED: {
            if (msg.size < sizeof(net::PickupCollectedData)) return;
            const std::lock_guard<std::mutex> lock(m_msg_mutex);
            net::PickupCollectedData pcd;
            memset(&pcd, 0, sizeof(pcd));
            memcpy(&pcd, msg.payload, std::min<size_t>(msg.size, sizeof(pcd)));
            pcd.weapon_def_name[sizeof(pcd.weapon_def_name) - 1] = '\0';
            for (auto& p : m_pickups) {
                if (p.id == pcd.pickup_id && p.world_index == pcd.world_index) {
                    p.active = false;
                    break;
                }
            }
            // Grant item only if we requested this collect. Pending ids are a
            // set, not a single slot: two rapid collects (E then walk-over) used
            // to overwrite each other and silently drop the first grant even
            // though the server had already applied the effect.
            bool granted = false;
            for (auto it = m_pending_collects.begin(); it != m_pending_collects.end(); ++it) {
                if (it->first == pcd.pickup_id && it->second == pcd.world_index) {
                    granted = true;
                    m_pending_collects.erase(it);
                    break;
                }
            }
            if (pcd.item_id > 0 && granted) {
                if (pcd.item_id == item_id::WEAPON && pcd.weapon_def_name[0] != '\0') {
                    // Weapon pickup - notify with weapon def name
                    if (m_on_weapon_collected) m_on_weapon_collected(pcd.weapon_def_name);
                } else if (m_on_item_collected) {
                    m_on_item_collected(pcd.item_id, pcd.quantity);
                }
            }
            pickups_changed = true;
            break;
        }
        case net::MessageType::XP_UPDATE: {
            if (msg.size < sizeof(net::XpUpdateData)) return;
            const std::lock_guard<std::mutex> lock(m_msg_mutex);
            net::XpUpdateData xud;
            memcpy(&xud, msg.payload, sizeof(xud));
            m_xp = xud.xp;
            m_level = xud.level;
            m_xp_to_next = xud.xp_to_next;
            break;
        }
        case net::MessageType::PLAYER_LEAVE: {
            if (msg.size < sizeof(uint32_t)) return;
            const std::lock_guard<std::mutex> lock(m_msg_mutex);
            uint32_t leave_id;
            memcpy(&leave_id, msg.payload, sizeof(leave_id));
            m_remote_players.erase(
                std::remove_if(m_remote_players.begin(), m_remote_players.end(),
                    [leave_id](const RemotePlayer& rp) { return rp.player_id == leave_id; }),
                m_remote_players.end());
            break;
        }
        case net::MessageType::PLAYER_HURT: {
            if (msg.size < sizeof(net::PlayerHurtData)) return;
            const std::lock_guard<std::mutex> lock(m_msg_mutex);
            net::PlayerHurtData pud;
            memcpy(&pud, msg.payload, sizeof(pud));
            if (m_on_player_hurt)
                m_on_player_hurt(pud.damage, pud.remaining_health);
            break;
        }
        case net::MessageType::SCORE_STATE: {
            if (msg.size < sizeof(net::ScoreStateData)) break;
            const std::lock_guard<std::mutex> lock(m_msg_mutex);
            net::ScoreStateData ss;
            memcpy(&ss, msg.payload, sizeof(ss));
            m_scores[ss.player_id] = ss;
            if (ss.matchOver) {
                m_matchOver = true;
                m_matchOverAt = now_seconds();
            }
            break;
        }
        case net::MessageType::WORLD_CHANGE: {
            // Gate on kWorldChangeSizeBase, never sizeof(): a server predating the
            // spawn tail sends a shorter message, and requiring the full size made a
            // client drop every one of them — the same trap as
            // PickupRespawnData::typeName. The world assignment is the part that
            // matters, so it is the part that must survive.
            if (msg.size < net::kWorldChangeSizeBase) return;
            const std::lock_guard<std::mutex> lock(m_msg_mutex);
            net::WorldChangeData wcd;
            memcpy(&wcd, msg.payload, sizeof(wcd));

            // The server is authoritative: `world_index` is where we ARE, whether or
            // not our request was granted. A refusal still carries the real world so
            // the client can reconcile instead of waiting for a reply that already
            // arrived saying no.
            m_serverWorldIndex = wcd.world_index;
            m_worldChangeAccepted = (wcd.accepted != 0);
            m_lastWorldChangeAt = now_seconds();
            // The spawn tail is optional; only trust it if it actually arrived.
            if (msg.size >= sizeof(net::WorldChangeData)) {
                m_worldSpawn = net::NetVec3{ wcd.spawnX, wcd.spawnY, wcd.spawnZ };
                m_hasWorldSpawn = true;
            }
            if (m_on_world_changed) m_on_world_changed(wcd.world_index, wcd.accepted != 0);
            break;
        }
        case net::MessageType::OBJECTIVE_STATE: {
            if (msg.size < sizeof(net::ObjectiveStateData)) break;
            const std::lock_guard<std::mutex> lock(m_msg_mutex);
            net::ObjectiveStateData os;
            memcpy(&os, msg.payload, sizeof(os));
            os.id[sizeof(os.id) - 1] = '\0';
            // Upsert by (world_index, id): the server re-sends periodically.
            bool found = false;
            for (auto& o : m_objectives) {
                if (o.world_index == os.world_index && o.id == os.id) {
                    o.progress = os.progress;
                    o.target = os.target;
                    found = true;
                    break;
                }
            }
            if (!found) {
                ClientObjective o;
                o.world_index = os.world_index;
                o.id = os.id;
                o.progress = os.progress;
                o.target = os.target;
                m_objectives.push_back(o);
            }
            break;
        }
        case net::MessageType::PLAYER_UPDATE: {
            // Relayed player update from another player
            if (msg.size < sizeof(net::PlayerUpdateData)) return;
            const std::lock_guard<std::mutex> lock(m_msg_mutex);
            net::PlayerUpdateData pud;
            memcpy(&pud, msg.payload, sizeof(pud));
            // NOTE: no player_id==0 self-filter here — the server never relays a
            // client's own update back to it, and server ids start at 0, so that
            // check only made the first player invisible to everyone else.

            bool found = false;
            for (auto& rp : m_remote_players) {
                if (rp.player_id == pud.player_id) {
                    rp.position = pud.position;
                    rp.yaw = pud.yaw;
                    rp.pitch = pud.pitch;
                    rp.health = pud.health;
                    rp.stance = pud.stance;
                    rp.active = true;
                    found = true;
                    break;
                }
            }
            if (!found) {
                RemotePlayer rp;
                rp.player_id = pud.player_id;
                rp.position = pud.position;
                rp.yaw = pud.yaw;
                rp.pitch = pud.pitch;
                rp.health = pud.health;
                rp.stance = pud.stance;
                rp.active = true;
                    rp.color_packed = id_to_color_packed(pud.player_id);
                m_remote_players.push_back(rp);
            }
            break;
        }
        case net::MessageType::PLAYER_ACTION: {
            if (msg.size < sizeof(net::WeaponFireData)) return;
            const std::lock_guard<std::mutex> lock(m_msg_mutex);
            net::WeaponFireData wfd;
            memcpy(&wfd, msg.payload, sizeof(wfd));
            if (wfd.player_id == 0) break; // skip own actions

            ClientProjectile proj;
            proj.origin = {wfd.origin_x, wfd.origin_y, wfd.origin_z};
            proj.direction = {wfd.dir_x, wfd.dir_y, wfd.dir_z};
            proj.spawn_time = static_cast<float>(now_seconds());
            proj.weapon_type = wfd.weapon_type;
            proj.owner_id = wfd.player_id;
            proj.color_packed = id_to_color_packed(wfd.player_id);
            m_projectiles.push_back(proj);

            // Keep last 64 projectiles
            while (m_projectiles.size() > 64) {
                m_projectiles.erase(m_projectiles.begin());
            }
            break;
        }
        case net::MessageType::WEAPON_AMMO: {
            if (msg.size < sizeof(net::WeaponAmmoData)) return;
            const std::lock_guard<std::mutex> lock(m_msg_mutex);
            net::WeaponAmmoData wad;
            memcpy(&wad, msg.payload, sizeof(wad));
            // Update remote player ammo (for HUD / animation sync)
            for (auto& rp : m_remote_players) {
                if (rp.player_id == wad.player_id) {
                    rp.ammo = wad.ammo;
                    rp.magazine = wad.magazine;
                    break;
                }
            }
            break;
        }
        default:
            break;
    }

    // Notify OUTSIDE the switch so no lock_guard from a case is still held. The
    // snapshot is a copy: handing a consumer a reference into m_pickups is the
    // same iterator-invalidation race npcs()/pickups() already had to be fixed
    // for.
    if (pickups_changed && m_on_pickups_changed) {
        std::vector<ClientPickup> snapshot;
        {
            const std::lock_guard<std::mutex> lock(m_msg_mutex);
            snapshot = m_pickups;
        }
        m_on_pickups_changed(snapshot);
    }
}

void OmegaClient::request_pickup_resync() {
    if (!m_client.is_connected()) return;

    net::NetworkMessage msg{};
    msg.magic = net::MAGIC;
    msg.type = static_cast<uint32_t>(net::MessageType::PICKUP_RESYNC);
    msg.size = 0;
    msg.sequence = 0;
    msg.timestamp = static_cast<uint32_t>(time(nullptr));
    m_client.send_message(msg);
}

void OmegaClient::request_world(int world_index) {
    if (!m_client.is_connected()) return;
    if (world_index < 0) return;   // the server validates too; do not send nonsense

    net::RequestWorldData rwd{};
    rwd.world_index = world_index;

    net::NetworkMessage msg{};
    msg.magic = net::MAGIC;
    msg.type = static_cast<uint32_t>(net::MessageType::REQUEST_WORLD);
    msg.size = sizeof(rwd);
    msg.sequence = 0;
    msg.timestamp = static_cast<uint32_t>(time(nullptr));
    memcpy(msg.payload, &rwd, sizeof(rwd));
    m_client.send_message(msg);
}

int OmegaClient::server_world_index() const {
    const std::lock_guard<std::mutex> l(m_msg_mutex);
    return m_serverWorldIndex;
}

bool OmegaClient::world_change_accepted() const {
    const std::lock_guard<std::mutex> l(m_msg_mutex);
    return m_worldChangeAccepted;
}

bool OmegaClient::has_world_spawn() const {
    const std::lock_guard<std::mutex> l(m_msg_mutex);
    return m_hasWorldSpawn;
}

net::NetVec3 OmegaClient::world_spawn() const {
    const std::lock_guard<std::mutex> l(m_msg_mutex);
    return m_worldSpawn;
}

std::vector<int> OmegaClient::pickup_worlds() const {
    const std::lock_guard<std::mutex> l(m_msg_mutex);
    std::vector<int> out;
    for (const auto& p : m_pickups) {
        if (std::find(out.begin(), out.end(), p.world_index) == out.end())
            out.push_back(p.world_index);
    }
    return out;
}

bool OmegaClient::has_pickup_world(int world_index) const {
    const std::lock_guard<std::mutex> l(m_msg_mutex);
    for (const auto& p : m_pickups)
        if (p.world_index == world_index) return true;
    return false;
}

void OmegaClient::on_connected() {
    OZ_INFO("OmegaClient: connected to server");
}

void OmegaClient::on_disconnected() {
    OZ_INFO("OmegaClient: disconnected from server");
    const std::lock_guard<std::mutex> lock(m_msg_mutex);
    m_remote_players.clear();
    m_projectiles.clear();
    // Pickups and in-flight collects are per-session server state. They used to
    // survive a disconnect, so rejoining (or switching worlds) started from the
    // previous server's list and any collect that had not been answered was
    // still queued — granting an item for a pickup the new server never issued.
    m_pickups.clear();
    m_pending_collects.clear();
    m_scores.clear();
    m_objectives.clear();   // per-session server state
    // The world assignment is per-session server state too: -1 is "the new server has
    // not told us yet", which is exactly the state a stale 0 would masquerade as.
    m_serverWorldIndex = -1;
    m_worldChangeAccepted = false;
    m_lastWorldChangeAt = 0.0;
    m_hasWorldSpawn = false;
}

std::string OmegaClient::consume_pending_scene_data() {
    const std::lock_guard<std::mutex> lock(m_msg_mutex);
    std::string data = std::move(m_pending_scene);
    m_pending_scene.clear();
    return data;
}

std::string OmegaClient::consume_pending_chat_message() {
    const std::lock_guard<std::mutex> lock(m_msg_mutex);
    std::string msg = std::move(m_chat_msg);
    m_chat_msg.clear();
    return msg;
}