// Master-protocol tests — standalone, no raylib/socket dependency.
// Covers heartbeat encode/decode, sanitization, JSON list round-trip and
// expiry so the wire contract between AngelMaster / AngelServ / client holds.
#include "../Source/Network/MasterProtocol.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace master;

int test_count = 0, pass_count = 0;

static void expect(bool ok, const char* name, const std::string& detail = "") {
    test_count++;
    printf("  TEST %s... ", name);
    if (ok) { pass_count++; printf("PASS\n"); }
    else printf("FAIL%s%s\n", detail.empty() ? "" : ": ", detail.c_str());
}

static void test_heartbeat_roundtrip() {
    Heartbeat hb;
    hb.gamever = "0.2.1";
    hb.host = "Etherrealm EU";
    hb.map = "world_ether";
    hb.port = 27015;
    hb.http_port = 8080;
    hb.players = 3;
    hb.maxplayers = 16;
    hb.password = false;

    std::string line = EncodeHeartbeat(hb);
    Heartbeat out;
    bool ok = DecodeHeartbeat(line, out) && out.host == "Etherrealm EU" &&
              out.map == "world_ether" && out.port == 27015 &&
              out.http_port == 8080 && out.players == 3 && out.maxplayers == 16;
    expect(ok, "heartbeat encode/decode round-trip", line);
}

static void test_heartbeat_prefix() {
    std::string line = EncodeHeartbeat(Heartbeat{});
    expect(StartsWith(line, "OZHEARTBEAT:"), "heartbeat has OZHEARTBEAT prefix");
}

static void test_heartbeat_rejects_wrong_gamename() {
    Heartbeat out;
    expect(!DecodeHeartbeat("gamename=quake;port=27015", out),
           "rejects non-angels95 gamename");
}

static void test_heartbeat_rejects_missing_port() {
    Heartbeat out;
    expect(!DecodeHeartbeat("gamename=angels95;host=x", out),
           "rejects heartbeat without port");
}

static void test_heartbeat_sanitizes_injection() {
    Heartbeat hb;
    hb.host = "evil;port=9999";
    hb.port = 27015;
    Heartbeat out;
    bool ok = DecodeHeartbeat(EncodeHeartbeat(hb), out) && out.port == 27015 &&
              out.host.find(';') == std::string::npos;
    expect(ok, "sanitizes ';' injection in name", out.host);
}

static void test_heartbeat_public_ip() {
    Heartbeat hb;
    hb.port = 27015;
    hb.public_ip = "159.195.20.100";
    Heartbeat out;
    bool ok = DecodeHeartbeat(EncodeHeartbeat(hb), out) &&
              out.public_ip == "159.195.20.100";
    expect(ok, "public_ip round-trip", out.public_ip);
}

static void test_heartbeat_protocol() {
    Heartbeat hb;
    hb.port = 27015;
    hb.protocol = 2;
    Heartbeat out;
    bool ok = DecodeHeartbeat(EncodeHeartbeat(hb), out) && out.protocol == 2;
    expect(ok, "protocol round-trips in the heartbeat", std::to_string(out.protocol));
}

static void test_announced_ip_policy() {
    expect(ResolveAnnouncedIp("159.195.10.230", "1.2.3.4") == "159.195.10.230",
           "public source cannot claim a different ip");
    expect(ResolveAnnouncedIp("192.168.1.9", "203.0.113.7") == "203.0.113.7",
           "private/LAN source may announce its public ip");
    expect(ResolveAnnouncedIp("159.195.10.230", "not-an-ip") == "159.195.10.230",
           "invalid claimed ip is ignored");
    expect(ResolveAnnouncedIp("159.195.10.230", "") == "159.195.10.230",
           "empty claimed ip uses the source");
}

static void test_heartbeat_limiter() {
    HeartbeatLimiter lim;
    const std::string ip = "159.195.10.230";
    // Two dedicated servers behind one public IP must both be admitted: the
    // limiter keys on ip:port, not on the source address.
    const bool a = lim.Accept(ip + ":27015", ip, 0);
    const bool b = lim.Accept(ip + ":27016", ip, 200);
    const bool again = lim.Accept(ip + ":27015", ip, 400);   // inside per-key 1.5s
    const bool later = lim.Accept(ip + ":27015", ip, 1600);  // after per-key window
    const bool burst = lim.Accept(ip + ":27017", ip, 1650);  // inside per-source 100ms
    const bool burst_ok = lim.Accept(ip + ":27017", ip, 1750);
    expect(a && b, "limiter admits two servers behind one public ip");
    expect(!again, "limiter rejects the same ip:port inside 1.5s");
    expect(later, "limiter admits the same ip:port after 1.5s");
    expect(!burst, "per-source cap rejects a >10/s burst");
    expect(burst_ok, "per-source cap admits after 100ms");

    lim.Prune(70000);
    expect(lim.server_keys() == 0 && lim.source_keys() == 0,
           "limiter prunes stale keys");
}

static void test_server_list_roundtrip() {
    std::vector<ServerEntry> in(2);
    in[0].ip = "159.195.20.100"; in[0].port = 27015; in[0].http_port = 8080;
    in[0].name = "Ether \"One\""; in[0].map = "world_ether"; in[0].gamever = "0.2.1";
    in[0].protocol = 2;
    in[0].players = 3; in[0].maxplayers = 16;
    in[1].ip = "10.0.0.7"; in[1].port = 28001; in[1].name = "LAN";
    in[1].players = 0; in[1].maxplayers = 8; in[1].password = true;

    std::string json = BuildServerListJson(in, GAMENAME);
    std::vector<ServerEntry> out;
    bool ok = ParseServerListJson(json, out) && out.size() == 2 &&
              out[0].ip == "159.195.20.100" && out[0].name == "Ether \"One\"" &&
              out[0].players == 3 && out[0].protocol == 2 && out[1].password;
    expect(ok, "server list JSON round-trip", json);
}

static void test_server_list_skips_malformed() {
    std::string json =
        "{\"servers\":[{\"ip\":\"1.2.3.4\"},{\"ip\":\"5.6.7.8\",\"port\":27015}]}";
    std::vector<ServerEntry> out;
    bool ok = ParseServerListJson(json, out) && out.size() == 1 &&
              out[0].ip == "5.6.7.8" && out[0].port == 27015;
    expect(ok, "JSON parser skips entries without a port");
}

static void test_server_list_brace_in_string() {
    std::string json =
        "{\"servers\":[{\"ip\":\"1.2.3.4\",\"port\":27015,\"name\":\"a}b\"}]}";
    std::vector<ServerEntry> out;
    bool ok = ParseServerListJson(json, out) && out.size() == 1 &&
              out[0].name == "a}b";
    expect(ok, "JSON parser ignores braces inside strings");
}

static void test_expiry() {
    ServerEntry e;
    e.last_seen = 1000.0;
    expect(!IsExpired(e, 1000 + EXPIRE_SECONDS), "not expired within window");
    expect(IsExpired(e, 1000 + EXPIRE_SECONDS + 1), "expired after window");
}

int main() {
    printf("Master protocol tests:\n");
    test_heartbeat_roundtrip();
    test_heartbeat_prefix();
    test_heartbeat_rejects_wrong_gamename();
    test_heartbeat_rejects_missing_port();
    test_heartbeat_sanitizes_injection();
    test_heartbeat_public_ip();
    test_heartbeat_protocol();
    test_announced_ip_policy();
    test_heartbeat_limiter();
    test_server_list_roundtrip();
    test_server_list_skips_malformed();
    test_server_list_brace_in_string();
    test_expiry();

    printf("\nResults: %d/%d passed\n", pass_count, test_count);
    return (pass_count == test_count) ? 0 : 1;
}