> **English** | [中文](compliance.md)

# Compliance and Naming Consistency (T5.3)

> Corresponds to acceptance item A22 (component license inventory) and A23 (system identity consistency). Verification date: 2026-10-03; supplemented 2026-10-04 (the R23 storage abstraction layer introduced aws-sdk-cpp:s3; the manifest's server-side dependencies sqlite3/openssl were registered together, see `third_party/组件清单.md`); 2026-10-10 closed-loop re-verification of the three vite advisories plus introduction-date annotations (see the table and research note below); 2026-10-13 A22 inventory re-verification added crashpad/drogon (drogon pending registration in `third_party/组件清单.md`).

## A22 Component License Verification Record

Third-party components are declared through the vcpkg manifest (the single source of truth at the repo root `vcpkg.json`); the inventory is in `third_party/组件清单.md`. Conclusions of this verification round:

| Verification point | Conclusion |
| --- | --- |
| Strongly copyleft licenses (AGPL/SSPL etc.) | None — the inventory covers asio (BSL-1.0), protobuf (BSD-3-Clause), nlohmann/json (MIT), sqlite3 (Public Domain), openssl (Apache-2.0), aws-sdk-cpp:s3 (Apache-2.0), vcpkg (MIT), Qt (LGPL-3.0), crashpad (Apache-2.0), drogon (MIT AND BSD-3-Clause, declared in the vcpkg manifest; registration in `third_party/组件清单.md` pending) |
| Qt linking | LGPL-3.0 dynamic linking (`x64-linux-dynamic` triplet); no static linking, no modification of Qt source |
| Source vendoring | The repo vendors no third-party source; everything is fetched by vcpkg per the baseline |
| Dependency upgrade path | Bumping `builtin-baseline` updates the inventory version column together; `overrides` is currently empty (no individual version pins) |

This inventory ships with the deliverable package and constitutes the A22 deliverable.

## Dependency Security Advisory Handling (dependabot)

2026-10-03 handling record (`gh api .../dependabot/alerts`, 4 items):

| Advisory | Package | Handling |
| --- | --- | --- |
| GHSA-67mh-4wv8-2f99 (esbuild ≤0.24.2) | esbuild 0.21.5 | Fixed: added `pnpm.overrides.esbuild ^0.25.0` to `docs/package.json` → 0.25.12, `docs:build` green |
| GHSA-4w7w-66w2-5vf9 / GHSA-fx2h-pf6j-xcff / GHSA-v6wh-96g9-6wx3 (vite ≤6.4.1/≤6.4.2) | vite 5.4.21 (embedded in the initial docs site) | Fixed (2026-10-04 `1747f69`): `pnpm.overrides.vite ^6.4.3` unifies everything — the vitepress 1.x embedded copy is pinned along with it, the lockfile now contains only 6.4.3, `docs:build` passes on all 15 pages of the site; the three dependabot alerts show status=fixed (re-verified 2026-10-10). The earlier assessment that "upgrading requires vitepress 2.0 alpha (vite ^8)" does not hold (the override needs no vitepress change); the downgrade rationale "dev server only, static artifacts do not carry it" still holds, but downgrading is no longer needed |

Review cycle: quarterly, or upon each vitepress stable release.

Introduction-date research (annotated 2026-10-10, same source as the introduction-date column in `third_party/组件清单.md`): VitePress was introduced with the docs site on 2026-10-02 (`9cf6f72`; devDeps contain only vitepress, with vite entering the same day as an embedded transitive dependency at 5.4.21); vite became a direct devDep on 2026-10-04 (`1747f69`, upgraded to 6.4.3 plus overrides pinning).

## A23 System Identity Consistency Check

Checked the current artifacts item by item against "Table 14 System Identity and Naming Inventory" in section 5.6 of review report V10.0:

| Spec item | Spec value | Repo status | Conclusion |
| --- | --- | --- | --- |
| English name and codename | Memex | README, docs site title, protocol package `memex.protocol.v1` | ✓ |
| Code repository name | memex | GitHub `cuihairu/memex`; server artifact `memex_server`, client `memex_client` binaries and `memex-server`/`memex-client` package names | ✓ |
| Database and log prefix | memex, [MEMEX] | Server default database `memex-server.db`, client local database `memex-local.db`, log line prefix `[MEMEX]` (`server/src/server.cpp`) | ✓ |
| Windows service name | MemexServer | The docs site (development.md, report.md) and the server entry comment `MemexServer 入口` use the same wording; the server runtime log outputs `[MEMEX] MemexServer 监听` | ✓ |
| Intranet domain / hostname | memex.corp.local (example), memex-srv | The review report's deployment chapter gives the same example | ✓ |
| Client installer naming | `MemexClient-x.y.z-*` | Nightly artifacts are uniform: linux `MemexClient-<version>-linux-x64-<date>-<short-SHA>.tar.gz`, win `MemexClient-<version>-win-x64.exe` (Inno installer) `/.zip` (portable), macOS `MemexClient-<version>-macos-arm64.dmg` (version taken from root CMakeLists PROJECT_VERSION; landed in T4.7) | ✓ |
| Wording convention | "Archive and retrieval"; do not use "monitor", "track", or "audit" | The in-app archive banner says 「消息不进归档」; README and docs site use the same wording | ✓ |

Check conclusion: all identities and naming match the inventory, no deviations (T4.7 installer naming has landed).
