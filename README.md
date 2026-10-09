[English](README.md) | [中文](README.zh.md)

<p align="center">
  <img src="docs/src/public/logo.svg" alt="Memex" width="96" height="96" />
</p>

<h1 align="center">Memex · Intranet Office Instant Messaging System</h1>

<p align="center">
  <a href="https://github.com/cuihairu/memex/actions/workflows/ci.yml"><img src="https://github.com/cuihairu/memex/actions/workflows/ci.yml/badge.svg" alt="CI" /></a>
  <a href="https://codecov.io/gh/cuihairu/memex"><img src="https://codecov.io/gh/cuihairu/memex/graph/badge.svg" alt="Codecov coverage" /></a>
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20(UOS%20%2F%20Kylin)%20%7C%20macOS%20%7C%20Android%20%7C%20iOS-e16531" alt="Platform support" />
  <img src="https://img.shields.io/badge/C%2B%2B17-Qt%206-e16531" alt="C++17 / Qt 6" />
  <a href="./LICENSE"><img src="https://img.shields.io/badge/License-Apache_2.0-blue.svg" alt="License: Apache-2.0" /></a>
</p>

An office instant messaging system for physically isolated intranet environments, modeled on the functional scope of WeCom (Enterprise WeChat) and covering communication, office collaboration, organization, and archive search. The business layer is written entirely in-house, while the foundation uses open-source components: Qt 6, asio, protobuf, SQLite, OpenSSL, and the AWS SDK (S3 interface, paired with RustFS); the inventory and licenses are listed in [third_party/](third_party/README.md). The system is not built on top of any open-source IM project, and it integrates no components under strong-copyleft licenses such as AGPL or SSPL. Desktop clients are C++17 + Qt; mobile clients are Android (Kotlin, shipped) and iOS (Swift, code-only with CI verification); the server is C++17, a single process on a single database.

## Documentation

- Documentation site (GitHub Pages): <https://cuihairu.github.io/memex/> — UI previews, quick tour, architecture and compliance; source under `docs/` (VitePress, build with `pnpm docs:build`)
- Product positioning: <https://cuihairu.github.io/memex/guide/product> · source [docs/src/guide/product.md](docs/src/guide/product.md)
- Review panel decision list (fifteen items + one optional): <https://cuihairu.github.io/memex/guide/decisions> · source [docs/src/guide/decisions.md](docs/src/guide/decisions.md)
- UI previews (live screenshots; 8 desktop screens + 4 mobile screens): <https://cuihairu.github.io/memex/guide/prototypes> · prototype sources [docs/design/prototypes/](docs/design/prototypes/)
- Product definition and phase basis: [docs/建设方案评审报告-V10.0.md](docs/建设方案评审报告-V10.0.md)
- Requirements and task list (R1–R19 mainline, R23–R27 additional batches, four-phase breakdown, acceptance criteria): [todo.md](todo.md)

## Product Form

A single installation package contains two communication engines:

| Mode | How to enter | Link | Archiving |
| --- | --- | --- | --- |
| Direct mode (default) | Works right after installation, zero configuration | UDP broadcast discovers terminals on the same subnet; TCP peer-to-peer connections (UDP 2425–2436 / TCP 2426–2437) | Stored on the local machine only |
| Collaboration mode (after sign-in) | Enter the server address and sign in | Persistent connection to the collaboration server | Fully persisted on the server, searchable |

The direct engine keeps running after sign-in (cross-mode interoperability requires it); when the server is unreachable, the client falls back to direct mode automatically. **The only purpose of connecting to the server is record retention**: no secret chats, no burn-after-reading, no deletion of archives upon message recall, no unilateral deletion of archives from the client, and no user-facing switch to turn record retention off.

Mobile clients run in collaboration mode only: the first launch must complete server address initialization, and anonymous use without sign-in is not provided.

## Monorepo Structure

```
memex/
├── client/               # Qt C++17 desktop client (Windows / UOS / Kylin / macOS)
│   ├── app/              # Entry point and main window, system integration (tray,
│   │                     #   notifications, autostart), screenshot annotation,
│   │                     #   themes, notification preferences (UI layer merged into app)
│   ├── engine/direct/    # Direct engine: UDP discovery, peer-to-peer messages and files
│   ├── engine/collab/    # Collaboration engine: persistent connections, message sync,
│   │                     #   offline messages and catch-up delivery
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
├── common/               # Shared by client and server: protocol definitions
│                         #   (common/proto/memex.proto), serialization, common utilities
├── apps/                 # Mobile clients
│   ├── android/          # Android (Kotlin, shipped in T6.3: R17/R18 + conversation list
│   │                     #   send/receive + three-level push banner and vibration)
│   ├── ios/              # iOS (Swift/SwiftUI, T6.4 code-only with CI verification)
│   ├── harmony/          # HarmonyOS NEXT (ArkTS, T6.6 code-only with CI logic verification,
│   │                     #   compile leg pending the OHOS toolchain)
├── admin/                # Admin console (planned; see the server CLI/HTTP meanwhile,
│                         #   this entry is a placeholder)
├── docs/                 # Documentation site (VitePress), review report, product docs,
│                         #   prototypes
└── third_party/          # Third-party component inventory and license review (A22;
                          #   dependencies managed via the vcpkg manifest)
```

Module boundaries correspond to Chapter 4 of the review report: the two engines are each self-contained and share no communication state; they meet only in the shared core (local message store, conversation list, contact views, file transfer module). The six server modules are consolidated into a single process (review decision no. 7: single-process, single-database implementation with logical splitting reserved); the data model is in Chapter 5 of the report.

## Build

Dependencies (asio / protobuf / nlohmann-json / sqlite3 / openssl / aws-sdk-cpp:s3 / Qt6) are managed uniformly through the vcpkg manifest (`vcpkg.json`, versions pinned by `builtin-baseline`); CMake builds with a single command via `CMakePresets.json`:

```bash
# 1. Set up vcpkg (once; adding VCPKG_ROOT to your shell profile is recommended)
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT="$HOME/vcpkg"

# 2. Development build (Release · server + client + tests; the first configure
#    installs packages per the manifest automatically)
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Presets: `dev` (development build), `ci` (Debug + coverage, the CI gate), and `server-release` / `client-release` (the daily-build artifact configuration). By default only server dependencies are installed; Qt desktop builds enable the manifest's `client` feature. The first configure compiles qtbase and takes a while; the vcpkg binary cache (default `~/.cache/vcpkg/archives`) reuses build artifacts on later configures, which then finish in seconds. CI and daily builds use GitHub Actions caching (the `x-gha` binary cache plus the full install tree) for speed.

## Deployment

### Docker (recommended)

Multi-stage build (`server/Dockerfile`): the dependency layer compiles statically per the vcpkg manifest (invalidated only when `vcpkg.json` changes), the build layer produces a single binary, and the runtime layer is `ubuntu:24.04` running as non-root (uid 10001). Dependency breakdown: asio and nlohmann-json are header-only; protobuf, openssl (libcrypto), sqlite3, and aws-sdk-cpp (s3) are statically linked into the binary, so the runtime layer contains zero `.so` files. The container build and the host development tree (`x64-linux-dynamic`, shared with the Qt client) are fully independent.

The vcpkg source is not cloned over the network: a snapshot is injected through a buildx named context (it must contain the commit matching the `builtin-baseline` in `vcpkg.json`; a clean clone with that commit checked out is enough, and if the snapshot carries the `downloads/` cache, the dependency layer installs fully offline). CI fetches the snapshot automatically in the workflow; local builds from source must specify it:

```bash
# First image build from source: inject the vcpkg snapshot
git clone https://github.com/microsoft/vcpkg vcpkg-src
git -C vcpkg-src checkout 10541e317a660f4165ba4ac2851ab54a8d4577b1
docker buildx build --build-context vcpkgsrc=./vcpkg-src \
  -f server/Dockerfile -t ghcr.io/cuihairu/memex-server:local .

# One-command start: server (SQLite persistent volume) + RustFS (S3-compatible object storage)
# (compose uses the same build by default: the VCPKG_SRC environment variable points to
#  the snapshot, ./vcpkg-src by default; once an image exists, plain up skips the build
#  and needs no snapshot)
docker compose up -d
docker compose ps        # ready once both services are healthy
# Client connections: <host>:24360 (collaboration plane), <host>:24561 (file plane)
```

For production, write the object-storage credentials into `.env` in the same directory (`MEMEX_S3_ACCESS_KEY` / `MEMEX_S3_SECRET_KEY`) before starting; the example credentials are for local trials only. Server-side S3 flags take precedence over environment variables (`MEMEX_S3_ENDPOINT` / `MEMEX_S3_BUCKET` / `MEMEX_S3_ACCESS_KEY` / `MEMEX_S3_SECRET_KEY`); the bucket is created idempotently when the file plane starts (with retries until the endpoint is ready). The health probe is the unauthenticated file-plane `GET /files/health` (if you change the file-plane port, also set the container's `MEMEX_FILES_PORT`).

Image publishing: pushing a `v*` tag or pushing to main has CI (`.github/workflows/docker.yml`) build and push `ghcr.io/<owner>/memex-server` (from main: immutable `sha-<full sha>` plus latest; `v*` tags additionally produce semver tags), with the dependency layer on the buildx gha cache.

### CI deployment chain (runner-Docker, 192.168.5.5)

Chain: ci (tests) green → docker (build and push to ghcr) → **deploy** (`.github/workflows/deploy.yml`, self-hosted runner): pull the immutable `sha-<full sha>` → compose up in `/data/deploy/memex` (only the `TAG=` line of `.env` is rewritten; host configuration such as the S3 credentials is preserved as-is and never printed during the process) → bounded polling (30×10s) of the container healthcheck plus a real host-side HTTP probe of `/files/health` = 200 → if the gate fails, roll back automatically to the previous tag and verify the rolled-back deployment is healthy. Manual `workflow_dispatch` can roll back explicitly to a given `sha-xxx` (rollback material = the historical sha images retained on the host).

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

All tests must be green before commit/push; before pushing, run `git fetch origin && git rebase origin/main`. One commit per acceptable increment; no tags, no releases, no force pushes. On licensing: no AGPL/SSPL components, and permissively licensed components are registered in the `third_party/` inventory. Naming follows Section 6 of Chapter 5 of the report: the codename is Memex, the service name is MemexServer, and databases and logs use the `memex` prefix.
