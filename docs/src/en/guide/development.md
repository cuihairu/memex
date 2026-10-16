> **English** | [中文](/guide/development)

# Clone and Build

## Environment requirements

- CMake 4.x, a C++17 compiler (GCC 15 or MSVC; the CI toolchain is not version-pinned —
  `ci.yml` uses whatever compiler the ubuntu-latest image ships with)
- [vcpkg](https://github.com/microsoft/vcpkg): every third-party dependency is managed via the
  manifest
- The system development packages needed to compile the Qt client on Linux (vcpkg does not
  manage system libraries such as X11/GL; Ubuntu reference:
  `autoconf autoconf-archive automake libtool` (needed for the source builds of autotools ports
  such as gperf — without them the qtbase dependency chain fails at gperf);
  `libx11-dev libx11-xcb-dev libxext-dev libxfixes-dev libxi-dev libxrender-dev libxrandr-dev libxcursor-dev libxinerama-dev libxkbcommon-dev libxkbcommon-x11-dev libxcb1-dev libxcb-cursor-dev libxcb-icccm4-dev libxcb-util-dev libxcb-image0-dev libxcb-keysyms1-dev libxcb-randr0-dev libxcb-render0-dev libxcb-render-util0-dev libxcb-shape0-dev libxcb-shm0-dev libxcb-sync-dev libxcb-glx0-dev libxcb-xfixes0-dev libxcb-xinerama0-dev libxcb-xkb-dev libxcb-xinput-dev libgl1-mesa-dev libglu1-mesa-dev mesa-common-dev libfontconfig1-dev libfreetype-dev libdbus-1-dev libicu-dev`;
  identical to the install list in the CI workflows)
- Documentation site: Node 24 + pnpm (CI scope, `docs-check.yml` / `docs-deploy.yml`)

## Dependency management (vcpkg manifest)

The repo-root `vcpkg.json` declares every third-party dependency (asio, protobuf,
nlohmann-json, sqlite3, openssl, aws-sdk-cpp:s3, qtbase, drogon, crashpad); versions are
pinned by `builtin-baseline` to a particular vcpkg upstream commit, and any package needing a
locked version goes into `overrides` (currently an empty placeholder). CI and the daily build
pin vcpkg to the same commit and enable the GitHub Actions binary cache (`x-gha`) plus an
installed-tree cache, reusing build artifacts across runs.

Dependency list:

| Dependency | Purpose | Notes |
| --- | --- | --- |
| `asio` | Server network I/O | header-only, `server` feature |
| `protobuf` | Protocol payload encode/decode (libprotobuf + protoc generation) | Installed by default (base); the Android/iOS/HarmonyOS ends each generate from the same `common/proto/memex.proto` via their own toolchains |
| `nlohmann-json` | Protocol JSON encode/decode | Installed by default (base, ≥3.11.3) |
| `sqlite3` | Server local store (accounts / devices / archive / file metadata) | `server` feature |
| `openssl` | Credential PBKDF2, device fingerprint SHA-256 (libcrypto only) | Installed by default (base, shared by both ends) |
| `aws-sdk-cpp` | R23 storage abstraction layer (S3-compatible object storage client) | `server` feature, with `s3` enabled |
| `qtbase` | Client UI framework | `client` feature, with `glib` / `xkb` / `xkbcommon-x11` enabled |
| `drogon` | Admin console HTTP framework (the admin process listens on 8089 as a separate process) | `admin` feature (decision taken 2026-10-11; registration in `third_party/组件清单.md` still pending) |
| `crashpad` | Client crash reporting (a standalone handler process writes the minidump; dumps stay on local disk only) | `crashpad` feature (enabled with the `client` build) |
| `gcovr` | CI coverage aggregation | No such port in vcpkg, provided by CI system packages (a host tool, not shipped with artifacts) |

The base (no feature enabled) installs only the dependencies shared by both ends
(protobuf / nlohmann-json / openssl); a server build enables the `server` feature (the preset
declares it via `VCPKG_MANIFEST_FEATURES`, and the container deps layer calls
`vcpkg install --x-feature=server` directly), and a build with the Qt desktop enables the
`client` feature (+`crashpad`) — so the daily build's server artifacts do not drag in Qt
compilation, and the manifest remains the single entry point. The server preset additionally
uses the installed tree `vcpkg_installed-server/`, which coexists with the Qt-bearing
`vcpkg_installed/`, and neither prunes the other. The HarmonyOS cross build (T6.2) uses the
`client` feature + the official `arm64-ohos` triplet and does not install the server trio.

Qt's `glib` feature is not optional: the xcb platform plugin of desktop Qt on Linux references
glib event-dispatcher symbols, and without that feature the desktop client aborts at startup.

The triplet is uniformly `x64-linux-dynamic` (a community triplet): **dynamically linking** Qt
under LGPL-3.0 is the established compliance decision, whereas vcpkg's default triplet
(`x64-linux`) builds qtbase statically and cannot be used in this project.

## vcpkg Bootstrap (one time, then go straight to the presets)

```bash
# 1. 获取 vcpkg（建议放在固定目录，如 ~/vcpkg）
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
cd ~/vcpkg
./bootstrap-vcpkg.sh        # Linux/macOS
# .\bootstrap-vcpkg.bat     # Windows

# 2. 导出环境变量（写入 ~/.bashrc / ~/.zshrc / 用户环境变量）
export VCPKG_ROOT=~/vcpkg
```

> **CI / daily build** already has `lukka/run-vcpkg@v11` built in (automatic clone,
> bootstrap, binary cache); no manual intervention is needed.

## Build and test (one preset command)

```bash
git clone https://github.com/cuihairu/memex.git
cd memex
export VCPKG_ROOT=~/vcpkg   # 若未写入 shell rc

# 开发构建：Release · 服务端+客户端+测试
cmake --preset dev          # 首次配置按 manifest 自动装包（qtbase 编译较久）
cmake --build --preset dev
ctest --preset dev
```

Preset overview (`CMakePresets.json`, Ninja generator; on a local Linux machine the vcpkg
installed tree is unified at `vcpkg_installed/`, while cross-platform presets each use their
own installed tree):

| Preset | Purpose | Build directory | Configuration |
| --- | --- | --- | --- |
| `base` | Shared baseline (hidden, inherited by each preset) | `build/` | Ninja · vcpkg toolchain · `x64-linux-dynamic` |
| `dev` | Local development | `build/` | Release · server + client + tests (manifest `server;client;crashpad;admin`) |
| `ci` | CI gate | `build-ci/` | Debug · `--coverage` · full set (manifest `server;client;crashpad;admin`) |
| `server-release` | Daily build · server | `build-server/` | Release · server only (manifest `server;admin`) |
| `client-release` | Daily build · client | `build-client/` | Release · client only (manifest `client;crashpad`) |
| `client-release-win` | Daily build · client (Windows x64) | `build-client-win/` | Release · official Qt + vcpkg non-Qt dependencies (manifest `crashpad`) |
| `client-release-mac` | Daily build · client (macOS arm64) | `build-client-mac/` | Release · official Qt + vcpkg static non-Qt dependencies (manifest `crashpad`) |
| `client-release-ohos` | Daily build · client (HarmonyOS arm64 cross) | `build-client-ohos/` | Release · Qt for OpenHarmony + vcpkg ohos dependencies (manifest `client`) |
| `server-release-mac` | Daily build · server (macOS arm64) | `build-server-mac/` | Release · vcpkg static dependencies, single-binary distribution |

Each preset has its own build directory, so caches never pollute one another; the vcpkg
installed trees are managed uniformly by the manifest — Qt-bearing builds on a local Linux
machine share `vcpkg_installed/`, the server preset uses `vcpkg_installed-server/`, and the
win / mac / ohos presets each use `vcpkg_installed-{win,mac,ohos}[-server]/`.

Binary cache: locally the default is `~/.cache/vcpkg/archives` (redirect it with
`VCPKG_DEFAULT_BINARY_CACHE`). When the manifest has not changed, a second configure restores
build artifacts from cache and finishes in seconds.

## Documentation site

```bash
cd docs
pnpm install
pnpm docs:dev      # 本地开发
pnpm docs:build    # 构建到 src/.vitepress/dist
pnpm docs:preview  # 本地预览构建产物
```

## Engineering discipline

- Commit gate: a commit / push is allowed only when the full test suite is green; before
  pushing, `git fetch origin && git rebase origin/main`.
- One commit per "verifiable increment"; no tags, no releases, no force pushes.
- Component-level license discipline: no AGPL / SSPL components are introduced; components
  under permissive licenses are registered in the `third_party/` list.
- Naming convention: codename Memex, service name MemexServer, database and log prefix `memex`.

## Module boundaries

The Monorepo structure and module boundaries correspond to chapters four / five of the review
report: the two engines are each self-contained and share no communication state, meeting only
through the shared core; the server's six modules (gateway, accounts & devices, organization
structure, messaging & archive, policy, file metadata) are implemented together as a single
process on a single database. For the complete structure see the repo
[README](https://github.com/cuihairu/memex#readme) (including the three mobile ends under
`apps/`).
