# Master Server & Internet Discovery

Angels95 / OmegaTech ships three cooperating pieces for internet server browsing,
mirroring the classic Unreal Tournament / GameSpy model:

1. **`AngelMaster`** — a standalone master server. Receives heartbeats from
   public game servers and serves the live server list as JSON.
2. **`AngelServ` uplink** — the dedicated server periodically announces itself
   to one or more masters (`--master`, `--master-http`).
3. **Client Internet browser** — the title menu's Multiplayer → Join page has a
   `LAN` / `Internet` source toggle. Internet mode fetches the master list and
   then queries each game server directly for live player counts and RTT.

LAN discovery (UDP 27100) is unchanged and still works alongside this.

---

## 1. Master server — `AngelMaster`

```bash
./AngelMaster [--port 27900] [--http-port 27950] [--max-servers 4096]
```

| Flag | Default | Meaning |
|---|---|---|
| `--port` | `27900` | UDP heartbeat listener |
| `--http-port` | `27950` | HTTP list/API port |
| `--max-servers` | `4096` | Cap on tracked servers |
| `--gamename` | `angels95` | Filter default for list queries |
| `--bind` | `127.0.0.1` | Interface to bind **both** the UDP heartbeat and the HTTP API (use `0.0.0.0` on a VPS) |

### HTTP API

| Endpoint | Description |
|---|---|
| `GET /api/servers?gamename=angels95` | JSON list of live servers |
| `GET /api/stats` | Lightweight `{servers, players}` summary |
| `POST /api/heartbeat` | Same payload as the UDP heartbeat (body = `key=value;...`) |

`Access-Control-Allow-Origin: *` is set on all responses so the web portal can
consume the list directly.

Example response:

```json
{"ok":true,"gamename":"angels95","count":1,"servers":[
  {"ip":"159.195.20.100","port":27015,"httpport":8080,"name":"Etherrealm EU",
   "map":"world_ether","gamever":"b97","proto":2,"players":3,"maxplayers":8,
   "password":false,"lastseen":1790551982}]}
```

Entries expire after **90 s** (3 × the 30 s heartbeat). Heartbeats are
rate-limited **per announced server (`ip:port`)** to one per 1.5 s, with a
coarse per-source-IP cap (10/s) so one host cannot flood the list. That is what
lets several dedicated servers behind one public IP / host networking all stay
listed; the old per-source-only limit silently dropped all but one of them
every cycle. The server cap is enforced and display strings are sanitized, but
heartbeats are unauthenticated by design (like classic masters).

A self-reported `publicip` is trusted **only from a private (LAN) source**. A
public source already *is* the address that must be listed, so honouring a
claimed value there would let any internet host announce an arbitrary
`ip:port` and poison the list. A private source (a server on the master's own
LAN, or a local reverse proxy) may announce its public address. Rate-limited
heartbeats are logged (`Master: rate-limited …`) instead of being dropped
silently.

---

## 2. Game server uplink — `AngelServ`

```bash
./AngelServ --port 27015 --http-port 8080 --dir GameData \
            --server-name "Etherrealm EU" \
            --master 127.0.0.1:27900 \
            --master-http http://127.0.0.1:27950 \
            --public-ip 203.0.113.7
```

| Flag | Meaning |
|---|---|
| `--bind IP\|HOST` | Interface for the game UDP socket **and** the HTTP API (default all interfaces) |
| `--server-name NAME` | Display name announced to masters (default `Angels95 Server`) |
| `--master host[:port]` | UDP heartbeat target, repeatable (default port `27900`) |
| `--master-http http://host:port` | HTTP heartbeat URL, repeatable |
| `--public-ip IP` | Public IP to announce when behind NAT (optional) |

- The first heartbeat is delayed by 0–3 s and the 30 s cadence carries ±5 s of
  jitter, so several servers on one host do not heartbeat in lockstep.
- Payload: `gamename`, `gamever`, `host`, `map`, `port`, `httpport`, `players`,
  `maxplayers`, `password`, `proto` (and optional `publicip`). `maxplayers` is
  the world's `levelinfo maxPlayers` (8 on the shipped levels), **not**
  `net::MAX_PLAYERS` (32); the server refuses the 9th joiner.
- The uplink runs on a background thread, so the 10 Hz game tick is never
  blocked by DNS or HTTP.
- Without any `--master`/`--master-http`, the server logs a warning and only
  uses LAN discovery.

The dedicated server also exposes live status (used by the client's direct
RTT/status query) at `GET /status`:

```json
{"ok":true,"server_name":"Etherrealm EU","players":0,"max_players":8,
 "uptime_seconds":42,"worlds":3,"map":"Dessert_Dreams","gamever":"b97",
 "proto":2,"password":false}
```

If `--auth-token` is set, `/status` is protected by the same Bearer gate as the
other HTTP endpoints (see `Wiki/Engine-Overview.md`).

### Build identity vs wire compatibility

Two numbers answer different questions and must not be conflated:

| Field | Example | Meaning | Enforced? |
|---|---|---|---|
| `gamever` | `b97` | Build id: the release tag (or `dev` locally) | No — display only |
| `proto` | `2` | `net::PROTOCOL_VERSION`, the wire contract | **Yes** — a mismatch is rejected at `CLIENT_AUTH` |

`gamever` used to be the hand-edited literal `0.2.1` in three places, so every
build claimed the same version and the browser could not tell b94 from b97.
It is now `oz::kBuildVersion` from `Source/Version.hpp`, stamped by the build:

```bash
make BUILD_VERSION=b97 AngelServ        # local
# CI sets BUILD_VERSION=${GITHUB_REF_NAME} for every job
```

`AngelServ --version` and `AngelMaster --version` print the stamped id, which is
the reliable way to fingerprint a deployed binary. The master stores and
returns `proto`; the client browser shows both `Build:` and `Protocol:` and
disables **Join** when a non-zero protocol differs (a legacy master that omits
`proto` reports 0 = unknown and is still allowed).

---

## 3. Client configuration — `System/Angels95.ini`

The client reads master addresses from the `[MasterServers]` section. Both a
comma-separated list and numbered keys are supported:

```ini
[MasterServers]
Master=https://angels95.tribewarez.com/master,http://127.0.0.1:27950
Master1=http://backup.example.net:27950
```

- If the section is missing/empty, the client falls back to the official public
  master `https://angels95.tribewarez.com/master`.
- `http://` and `https://` are both supported. TLS needs no vendored crypto:
  Windows uses WinHTTP (Schannel), Linux/macOS shell out to `curl`.
- When hosting from the menu (Host Game → Start Server), the client forwards
  these addresses to the launched `AngelServ` as `--master` / `--master-http`.

### Using the browser

1. Open **Multiplayer → Join Game**.
2. Set **Source** to `Internet` — the master list is fetched automatically on
   first open (**Scan Internet** re-fetches on demand).
3. The list is populated from every reachable master; each row shows
   `name ip:port (players/max) map RTT`. RTT is the real round trip to the game
   server's `/status` endpoint.
4. Click a row to fill IP/port, then **Connect**. The join path is identical to
   LAN/direct connect.

> Official master: `https://angels95.tribewarez.com/master` — game servers
> announce with `--master-http https://angels95.tribewarez.com/master/api/heartbeat`
> (or `--master <host>:27900` for the UDP path).

---

## 4. Running the three pieces locally

Terminal 1 — master:

```bash
./AngelMaster --port 27900 --http-port 27950
```

Terminal 2 — dedicated server announcing to the master:

```bash
./AngelServ --port 27015 --http-port 8080 --dir GameData \
            --server-name "Local Test" \
            --master 127.0.0.1:27900 \
            --master-http http://127.0.0.1:27950
```

Verify with curl:

```bash
curl "http://127.0.0.1:27950/api/servers?gamename=angels95"
curl "http://127.0.0.1:8080/status"
```

Terminal 3 — client: launch `Angels95`, go to Multiplayer → Join → Source
`Internet` → **Scan Internet** (the default master list already points at
`http://127.0.0.1:27950`).

For a public deployment, run `AngelMaster` on a reachable host (e.g.
`master.example.com:27900/27950`), point game servers at it, and add that URL
to each client's `[MasterServers]` section. Both daemons accept `--bind
0.0.0.0` for VPS hosting, and `System/angels95-serv.service` ships systemd
units for `AngelServ` and `AngelMaster`.

---

## 5. Implementation map

| Piece | File |
|---|---|
| Build identity (`oz::kBuildVersion`, `--version`) | `Source/Version.hpp` |
| Shared wire codec (heartbeat + JSON list + limiter) | `Source/Network/MasterProtocol.hpp` |
| Minimal HTTP(S) client (WinHTTP on Windows, curl on POSIX — no vendored crypto) | `Source/Network/MasterHttp.hpp` |
| Server → master uplink thread | `Source/Server/Master/MasterClient.hpp` |
| Master daemon | `Source/Server/Master/Master.cpp` |
| Master address loader + default URL | `Source/Client/MasterList.hpp` |
| Client internet browser (async) | `Source/Menu/InternetBrowser.hpp` |
| Client browser UI | `Source/Menu/TitleMenu.hpp` |
| Uplink wiring | `Source/Server/Server.cpp` |
| Tests | `tests/Master.test.cpp` (`make test_master`) |

### Limitations

- Heartbeats/list uploads are unauthenticated by design (like classic
  masters); spoofing is mitigated by rate limiting, expiry, and the b65
  anti-spoof rule that only trusts `publicip` from non-private sources.
- TLS adds no vendored crypto — Windows uses WinHTTP (Schannel), POSIX shells
  out to `curl`; custom CAs resolve through the OS trust store.
- The pure GameSpy binary/`\status\` query protocol is **not** implemented; the
  JSON/HTTP path is the supported integration. The wire format is small and
  documented above if a GameSpy-compatible endpoint is added later.
