[English](README.md) | [中文](README.zh.md)

<p align="center">
  <img src="docs/src/public/logo.svg" alt="Memex" width="64" height="64" />
</p>

<h1 align="center">Memex · Intranet Office Instant Messaging System</h1>

<p align="center">
  <a href="https://github.com/cuihairu/memex/actions/workflows/ci.yml"><img src="https://github.com/cuihairu/memex/actions/workflows/ci.yml/badge.svg" alt="CI" /></a>
  <a href="https://codecov.io/gh/cuihairu/memex"><img src="https://codecov.io/gh/cuihairu/memex/graph/badge.svg" alt="Codecov coverage" /></a>
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS%20%7C%20Android%20%7C%20iOS%20%7C%20HarmonyOS-e16531" alt="Platform support" />
  <img src="https://img.shields.io/badge/C%2B%2B17-Qt%206-e16531" alt="C++17 / Qt 6" />
  <a href="./LICENSE"><img src="https://img.shields.io/badge/License-Apache_2.0-blue.svg" alt="License: Apache-2.0" /></a>
</p>

An office instant messaging system for physically isolated intranet environments, modeled on the functional scope of WeCom (Enterprise WeChat) and covering communication, office collaboration, organization, and archive search. The business layer is written entirely in-house, while the foundation uses open-source components: Qt 6, asio, protobuf, SQLite, OpenSSL, and the AWS SDK (S3 interface, paired with RustFS); the inventory and licenses are listed in [third_party/](third_party/README.md). The system is not built on top of any open-source IM project, and it integrates no components under strong-copyleft licenses such as AGPL or SSPL. Desktop clients are C++17 + Qt (Linux CI-verified; Windows/macOS artifacts come from the nightly build matrix); mobile clients are Android (Kotlin, shipped), iOS (Swift, code-only with CI verification), and HarmonyOS NEXT (ArkTS, code-only with CI logic verification); the server is C++17, a single process on a single database.

## Documentation

- Documentation site (GitHub Pages): <https://cuihairu.github.io/memex/> — UI previews, quick tour, architecture and compliance; source under `docs/` (VitePress, build with `pnpm docs:build`)
- Product positioning: <https://cuihairu.github.io/memex/guide/product> · source [docs/src/guide/product.md](docs/src/guide/product.md)
- Review panel decision list (fifteen items + one optional): <https://cuihairu.github.io/memex/guide/decisions> · source [docs/src/guide/decisions.md](docs/src/guide/decisions.md)
- UI previews (live screenshots; 8 desktop screens + 4 mobile screens): <https://cuihairu.github.io/memex/guide/prototypes> · prototype sources docs/design/prototypes/
- Product definition and phase basis: [product plan review report](docs/建设方案评审报告-V10.0.md)
- Requirements and task list (R1–R19 mainline, R23–R27 additional batches, four-phase breakdown, acceptance criteria): [todo.md](todo.md)

## Product Form

A single installation package contains two communication engines:

| Mode | How to enter | Link | Archiving |
| --- | --- | --- | --- |
| Direct mode (default) | Works right after installation, zero configuration | UDP broadcast discovers terminals on the same subnet; TCP peer-to-peer connections (UDP uses 2425 only, 2426–2436 reserved / TCP 2426–2437) | Stored on the local machine only |
| Collaboration mode (after sign-in) | Enter the server address and sign in | Persistent connection to the collaboration server | Fully persisted on the server, searchable |

The direct engine keeps running after sign-in (cross-mode interoperability requires it); when the server is unreachable, the client falls back to direct mode automatically. **The only purpose of connecting to the server is record retention**: no secret chats, no burn-after-reading, no deletion of archives upon message recall, no unilateral deletion of archives from the client, and no user-facing switch to turn record retention off.

Mobile clients run in collaboration mode only: the first launch must complete server address initialization, and anonymous use without sign-in is not provided.

## Monorepo Structure

```
memex/
├── client/               # Qt C++17 desktop client (Windows / Linux / macOS)
│   ├── app/              # Entry point and main window, system integration (tray,
│   │                     #   notifications, autostart), screenshot annotation,
│   │                     #   themes, notification preferences (UI layer merged into app)
│   ├── engine/direct/    # Direct engine: UDP discovery, peer-to-peer messages and files
│   ├── engine/collab/    # Collaboration engine: persistent connections, message sync,
│   │                     #   offline messages and catch-up delivery
│   ├── engine/task/      # External task providers (dingtalk / feishu / github) over a
│   │                     #   capability SPI (L1 jump / L2 read / L3 write; R27-2)
│   ├── core/             # Shared core: local store (SQLite), conversations, file transfer
│   └── tests/            # Desktop unit tests
├── server/               # C++17 collaboration server (single-host centralized deployment)
│   └── src/              # Single process on a single database, covering the six modules
│                         #   of the review report (gateway / accounts & devices /
│                         #   organization / message archive / policy / file metadata):
│                         #   server.cpp session.cpp (gateway and messaging)
│                         #   store.cpp authz.cpp cred.cpp (accounts / org / policy / archive)
│                         #   files_server.cpp storage.cpp (R23 file plane and S3 storage)
│                         #   webhook.cpp (T4.10 notification integration)
│                         #   main.cpp (entry point and CLI), model_gateway.cpp
│                         #   (model gateway, platform phase 3), ldap.cpp (enterprise identity),
│                         #   group_assistant.cpp (in-group assistant), task_pull.cpp
│                         #   (external task pull), agent_main.cpp (R26-1 agent:
│                         #   host-load heartbeat)
├── common/               # Shared by client and server: protocol definitions
│                         #   (common/proto/memex.proto), serialization, common utilities
├── apps/                 # Mobile clients
│   ├── android/          # Android (Kotlin, shipped in T6.3: R17/R18 + conversation list
│   │                     #   send/receive + three-level push banner and vibration;
│   │                     #   39/39 unit tests + on-device walkthrough, no CI leg)
│   ├── ios/              # iOS (Swift/SwiftUI, T6.4 code-only with CI verification)
│   ├── harmony/          # HarmonyOS NEXT (ArkTS, T6.6 code-only with CI logic verification,
│   │                     #   compile leg pending the OHOS toolchain)
├── admin/                # Admin web console (Drogon process, port 8089; started with
│                         #   compose — see the Admin web console section)
├── docs/                 # Documentation site (VitePress), review report, product docs,
│                         #   prototypes
├── scripts/              # Ops/drill scripts (WAN drill, backup/restore drill, nightly,
│                         #   deploy helpers, tray walkthrough)
├── packaging/            # Packaging support (Windows Inno script, OHOS HAP notes, LDAP
│                         #   env example)
├── examples/             # Example integrations (echo bot)
├── install.sh            # One-command install from nightly artifacts (OS/arch detection,
│                         #   idempotent re-run; curl-able from the main branch)
├── install.ps1           # Windows counterpart of install.sh
├── BUGS.md               # Bug ledger (register-first policy, fix/acceptance records)
└── third_party/          # Third-party component inventory and license review (A22;
                          #   dependencies managed via the vcpkg manifest)
```

Module boundaries correspond to Chapter 5 of the review report: the two engines are each self-contained and share no communication state; they meet only in the shared core (local message store, conversation list, contact views, file transfer module). The six server modules are consolidated into a single process (review decision no. 7: single-process, single-database implementation with logical splitting reserved); the data model is in the same chapter.

## Build

Dependencies (asio / protobuf / nlohmann-json / sqlite3 / openssl / aws-sdk-cpp:s3 / drogon (admin console) / crashpad (client crash collection) / Qt6) are managed uniformly through the vcpkg manifest (`vcpkg.json`, versions pinned by `builtin-baseline`); CMake builds with a single command via `CMakePresets.json`:

```bash
# 1. Set up vcpkg (once; adding VCPKG_ROOT to your shell profile is recommended)

<p align="center">
<a href="https://github.com/cuihairu/memex/releases"><img alt="Status" src="https://img.shields.io/badge/status-in%20development-orange"></a>
</p>
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT="$HOME/vcpkg"

# 2. Development build (Release · server + client + tests; the first configure
#    installs packages per the manifest automatically)
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Presets: `dev` (development build), `ci` (Debug + coverage, the CI gate), `server-release` / `client-release` (the daily-build artifact configuration), and the platform variants `client-release-win` / `client-release-mac` / `client-release-ohos` / `server-release-mac` (a hidden `base` preset supplies the shared vcpkg settings). By default only the shared client/server dependencies (protobuf, nlohmann-json, openssl) are installed; server builds enable the manifest's `server` feature (asio, sqlite3, aws-sdk-cpp), and Qt desktop builds enable `client`. The first configure compiles qtbase and takes a while; the vcpkg binary cache (default `~/.cache/vcpkg/archives`) reuses build artifacts on later configures, which then finish in seconds. CI and daily builds use GitHub Actions caching (the `x-gha` binary cache plus the full install tree) for speed.

## Local testing conventions

CI and local development split into three lanes by change surface — a one-line
doc change should not trigger a full build.

1. **Doc changes** (`*.md`, `docs/**`, images): no compilation. The main CI
   workflow's `paths-ignore` already skips these paths.
2. **Code changes**: run only the affected subset, never the full suite.
   - `ctest --preset ci -R '<affected tests>'`
3. **Full suite** (`ctest --preset ci`): only for CMakeLists.txt, `.github/**`,
   Docker/Compose config changes, or pre-release.

## Deployment

### Docker (recommended)

Multi-stage build (`server/Dockerfile`): the dependency layer compiles statically per the vcpkg manifest (invalidated only when `vcpkg.json` changes), the build layer produces a single binary, and the runtime layer is `ubuntu:24.04` running as non-root (uid 10001). Dependency breakdown: asio and nlohmann-json are header-only; protobuf, openssl (libcrypto), sqlite3, and aws-sdk-cpp (s3) are statically linked into the binary, so the runtime layer carries no dependency-side `.so` files beyond the base image's own system libraries. The container build and the host development tree (`x64-linux-dynamic`, shared with the Qt client) are fully independent.

The vcpkg source is not cloned over the network: a snapshot is injected through a buildx named context (it must contain the commit matching the `builtin-baseline` in `vcpkg.json`; a clean clone with that commit checked out is enough, and if the snapshot carries the `downloads/` cache, the dependency layer installs fully offline). CI fetches the snapshot automatically in the workflow; local builds from source must specify it:

```bash
# First image build from source: inject the vcpkg snapshot
git clone https://github.com/microsoft/vcpkg vcpkg-src
git -C vcpkg-src checkout 10541e317a660f4165ba4ac2851ab54a8d4577b1
docker buildx build --build-context vcpkgsrc=./vcpkg-src \
  -f server/Dockerfile -t ghcr.io/cuihairu/memex-server:local .

# One-command start: server (SQLite persistent volume) + RustFS (S3-compatible object storage) + admin web console
# (compose uses the same build by default: the VCPKG_SRC environment variable points to
#  the snapshot, ./vcpkg-src by default; once an image exists, plain up skips the build
#  and needs no snapshot)
docker compose up -d
docker compose ps        # ready once all three services are healthy
# Client connections: <host>:8088 (collaboration plane, external port aligned with the
# tunnel origin tcp://192.168.5.5:8088) or <host>:24360 (LAN default-port compat),
# <host>:24561 (file plane)
# Model gateway: http://<host>:24362 (platform phase 3; off by default — pass
# --model-port N to bind it; idle unless an upstream endpoint is registered)
# Admin console: http://<host>:8089 (see below)
```

For production, write the object-storage credentials into `.env` in the same directory (`MEMEX_S3_ACCESS_KEY` / `MEMEX_S3_SECRET_KEY`) before starting; the example credentials are for local trials only. Server-side S3 flags take precedence over environment variables (`MEMEX_S3_ENDPOINT` / `MEMEX_S3_BUCKET` / `MEMEX_S3_ACCESS_KEY` / `MEMEX_S3_SECRET_KEY`); the bucket is created idempotently when the file plane starts (with retries until the endpoint is ready). The health probe is the unauthenticated file-plane `GET /files/health` (if you change the file-plane port, also set the container's `MEMEX_FILES_PORT`).

Image publishing: pushing a `v*` tag or pushing to main has CI (`.github/workflows/docker.yml`) build and push `ghcr.io/<owner>/memex-server` and `ghcr.io/<owner>/memex-admin` (from main: immutable `sha-<full sha>` plus latest; `v*` tags additionally produce semver tags), with the dependency layer on the buildx gha cache (per-image cache scopes, so the two images do not evict each other).

### Public access via Cloudflare tunnel (TCP 8088)

```
client (cloudflared access tcp) → Cloudflare edge: server.memex.cuihairu.site
  → user-side tunnel → origin tcp://<origin-host>:8088 → host 0.0.0.0:8088 → container 24360
```

The mapping itself is configured on the user's Cloudflare side (token and tunnel config are not managed in this repo). The repo's side of the contract is one line: **publish the collaboration plane on host port 8088** (the compose default) and point the tunnel origin at the machine that actually runs `docker compose up` — if the deployment lives on a different host than the tunnel config assumes, update the origin, the port stays 8088.

- Client side of a raw-TCP tunnel: the edge cannot be hit with a bare TCP client from the internet — run `cloudflared access tcp --hostname server.memex.cuihairu.site --url 127.0.0.1:8088` on the client machine, then point the Memex client at `127.0.0.1:8088`. LAN clients skip the tunnel entirely and connect to `<host>:8088` directly.
- Webhook (24361): public exposure needs its own ingress rule (an HTTP-mapped hostname or an extra HTTP ingress on the same tunnel) — deliberately **not managed in this repo**; the hook token is a bearer secret, so prefer keeping 24361 LAN-only.

Status checks and troubleshooting — walk from the origin outward:

```bash
# 1) Origin really serving? (all green here before blaming the tunnel)
docker compose ps                                   # server healthy
nc -zv <origin-host> 8088                           # collaboration plane accepts
curl -sf http://<origin-host>:24561/files/health    # file plane health = 200

# 2) Tunnel status (user side): Cloudflare dashboard → Zero Trust → Networks →
#    Tunnels, or from the cloudflared host:
cloudflared tunnel list && cloudflared tunnel info <tunnel>

# 3) Client cannot connect? In order: does server.memex.cuihairu.site resolve?
#    → tunnel HEALTHY (connector online)? → can the cloudflared host reach
#    <origin-host>:8088? → is the client really going through cloudflared access
#    (a bare TCP client never reaches the edge)?
#    LAN alternative: hosts-override the domain to the server IP and connect
#    directly — no tunnel in the path.
```

**LAN direct connect: domain resolution override.** Default is no override — on the
public internet the domain resolves through normal DNS into the tunnel. Inside the
LAN you can pin `server.memex.cuihairu.site` to the real server IP so traffic never
leaves the segment; pick one of:

- **Client setting (built-in)**: Memex menu Settings → Connection Settings…, fill
  "domain resolution override" as `domain=IP`
  (`server.memex.cuihairu.site=192.168.5.5`; several entries separated by
  `;`). The override applies to this machine's connections only and takes precedence
  over system DNS; clearing the field restores normal resolution.
- **hosts file**: point the domain at the server IP on each LAN machine —
  Linux/macOS `/etc/hosts`, Windows `C:\Windows\System32\drivers\etc\hosts`, line:
  `192.168.5.5  server.memex.cuihairu.site`.
- **Internal DNS**: a split-horizon A record for the domain pointing at the server's
  LAN address — zero per-machine setup, needs control over the LAN resolver.

### Domains and access

All names below are **optional**. With none of them configured, everything still
works — LAN clients connect straight to `<server-host>:<port>`, and an entry
without a mapping is simply not exposed. Mappings live on the user's Cloudflare
side (tunnel); this repo does not manage them — it only owns the host-port
contract each name maps to.

#### server.memex.cuihairu.site — collaboration plane (TCP 8088)

- **Purpose**: public entry for desktop/mobile clients — login, messaging, archive
  retrieval. TCP through the Cloudflare tunnel to the origin host's **8088**
  (compose maps host 8088 → container 24360).
- **Where configured**: user-side Cloudflare tunnel (`… → tcp://<origin-host>:8088`).
  The repo-side contract is just: publish the collaboration plane on host 8088 and
  point the origin at the machine actually running `docker compose up`.
- **Override** (LAN direct connect): client setting Settings → Connection
  Settings… (`domain=IP`),
  hosts file, or split-horizon DNS — see "LAN direct connect" above.
- **Optional**: without it, LAN clients use `<host>:8088` directly; the name only
  buys public reachability. The deep-dive is the tunnel section above.

#### admin.memex.cuihairu.site — admin web console (HTTP 8089)

- **Purpose**: browser admin console — org, policy, retention, audit export,
  device/login ledger, health.
- **Where configured**: user-side tunnel HTTP mapping `… → http://<origin-host>:8089`
  (repo-side contract: compose publishes 8089; keep it off the open internet or
  front it with an authenticating proxy — see the console section below).
- **Override**: browsers have no client-side override — hosts file or internal
  split DNS; on the LAN, `http://<host>:8089` directly.
- **Optional**: without it the console stays LAN-only at `http://<host>:8089`.

#### webhook 24361 — no domain today

- **Purpose**: inbound `POST /hook/<token>` notices (automation/bot entry).
- **Where configured**: nowhere today — no name maps to it. Public exposure needs
  its own HTTP ingress (user-side; not managed in this repo). The hook token is a
  bearer secret — prefer LAN-only.
- **Override**: hosts / split DNS if you ever name it; LAN clients use
  `<host>:24361` directly.
- **Optional**: feature surface, not an entry point anyone depends on.

#### file plane 24561 — no domain today

- **Purpose**: `GET/POST /files/*` — files, avatars, branding, group-tool
  attachments; every client file byte flows through this plane.
- **Where configured**: nowhere today; clients set the port in the login dialog
  (file port) and talk to `<host>:24561`.
- **Override**: hosts / split DNS if a name is ever wanted; default is LAN direct.
- **Optional**: required for file features, but as a *port*, not a name. If a
  public entry is ever needed, this plane — not RustFS — is the right candidate.

#### File sharing and RustFS: why there is no rustfs domain

RustFS (the S3-compatible object store) listens on `http://rustfs:9000` **inside
the compose network only**. The server is its only client: it holds the S3
credentials and proxies every object byte for clients (files never go
client → object store directly). A `rustfs.memex.cuihairu.site` is therefore
neither needed nor wanted — publishing object storage would only widen the
attack surface around a credential-bearing endpoint. If remote file access is
ever required, the candidate for a public name is the **file plane (24561)**
itself, which stays authenticated and server-mediated.

### Admin web console (port 8089)

`memex-admin` is an independent Drogon process that opens the same SQLite database as the server (busy-timeout concurrency, same-CLI data plane) and serves the org-management surface on **8089**: accounts & org tree, policy push, retention (dual-approval purge), archived-message search/export + online audit trail, device/login ledger, health. Authorization reuses the server's az engine and role semantics: managing requires the **org-admin** effective role, reading archived messages requires the **auditor** role (an org-admin is *not* automatically able to read messages — denied attempts are logged); sessions time out after 30 minutes of inactivity.

```bash
# First admin account bootstrap (one-off, via the server container's CLI; the web
# console takes over after that):
docker exec -it memex-server memex_server account add admin --role admin   # check exact flags with `memex_server account --help`
docker exec -it memex-server memex_server org role grant admin org-admin

# Domain access: admin.memex.cuihairu.site → http://192.168.5.5:8089 (tunnel/reverse
# proxy maps to the host's 8089; keep the mapping terminal-side and the LAN resolves
# through the same mechanism as the existing memex domains). All console APIs are
# POST-only with a session cookie plus a custom X-Memex-Admin header (cross-site
# form protection, honestly labeled: not a full CSRF solution; keep 8089 off the
# public internet or front it with an authenticating proxy).
```

### CI deployment chain (runner-Docker, 192.168.5.5)

Chain: ci (tests) green → docker (build and push both images to ghcr) → **deploy** (`.github/workflows/deploy.yml`, self-hosted runner): pull the immutable `sha-<full sha>` → compose up in `/data/deploy/memex` (only the `TAG=` line of `.env` is rewritten; host configuration such as the S3 credentials is preserved as-is and never printed during the process) → bounded polling (30×10s) of the container healthchecks plus a real host-side HTTP probe of `/files/health` = 200 → if the gate fails, roll back automatically to the previous tag and verify the rolled-back deployment is healthy. Manual `workflow_dispatch` can roll back explicitly to a given `sha-xxx` (rollback material = the historical sha images retained on the host). The compose file also brings up `memex-admin`; when upgrading an existing deployment, the first `up` pulls the admin image alongside the server one and the console appears on the host's 8089.

One-time setup of runner-Docker (run on that machine):

```bash
# 1) actions runner: use the custom label memex-docker100 at registration
#    (deploy.yml runs-on depends on it)
mkdir -p ~/actions-runner && cd ~/actions-runner
# Fetch the latest package URL and TOKEN from repo Settings→Actions→Runners
# (the token expires; obtain it fresh when needed)
tar xzf actions-runner-linux-x64-*.tar.gz
./config.sh --url https://github.com/cuihairu/memex --token <TOKEN> --labels memex-docker100
sudo ./svc.sh install && sudo ./svc.sh start

# 2) ghcr pull credentials (install them for the runner user if the package is private;
#    skip if the package is public)
docker login ghcr.io   # user cuihairu; the PAT needs read:packages

# 3) Deployment directory and runtime configuration (the deploy workflow maintains
#    the TAG line; the rest is managed by hand)
mkdir -p /data/deploy/memex && cd /data/deploy/memex
cat > .env <<'EOF'
MEMEX_S3_ACCESS_KEY=<production credential>
MEMEX_S3_SECRET_KEY=<production credential>
EOF
```

Rollback drill (acceptance criterion: deliberately fail one deployment and prove the rollback chain really returns to a 200): dispatch a deployment of a nonexistent sha with `gh workflow run deploy -f tag=sha-deadbeef`. Expected outcome: the pull fails right away, or the container starts but fails the health gate; the workflow then rolls back automatically to the previous tag and probes 200 (the run log shows the "rollback … healthy" entry), and the live service is not interrupted. Manual recovery: take the credentials from the running container (`docker exec memex-server printenv | grep ^MEMEX_S3_`); never print them to external channels.

### Bare metal / virtual machines

```bash
# Daily-build configuration (Release · server only)
cmake --preset server-release && cmake --build --preset server-release
./build-server/server/memex_server serve --db /var/lib/memex/memex.db \
  --port 24360 --webhook-port 0 --files-port 24561 \
  --s3-endpoint http://127.0.0.1:9000 --s3-bucket memex \
  --s3-access-key ... --s3-secret-key ...
```

For the RustFS object store, use `./build-server/server/memex_server storage compose` (generates a `docker compose` file) or run `docker run rustfs/rustfs:latest` directly (the listening contract is documented in the notes in `server/src/storage.cpp`).

## Engineering Discipline

All tests must be green before commit/push; before pushing, run `git fetch origin && git rebase origin/main`. One commit per acceptable increment; no git tags, no formal releases (the nightly rolling binaries are the daily-build channel — a rolling `nightly` Release plus `sha-` / latest image tags), no force pushes. On licensing: no AGPL/SSPL components, and permissively licensed components are registered in the `third_party/` inventory. Naming follows Section 6 of Chapter 5 of the report: the codename is Memex, the service name is MemexServer, and databases and logs use the `memex` prefix.
