# Source Layer Architecture

> This page records the runtime architecture found in the source tree on
> 2026-11-07. Layers describe runtime responsibility, not directory names.
> Terms are defined in [Architecture Glossary](/guide/glossary); the Chinese
> page is [源码分层架构](/zh/guide/architecture).

## Layer Inventory

| # | Layer | Responsibility |
| --- | --- | --- |
| 1 | Protocol | The wire contract shared by all connected clients and servers. |
| 2 | Server Runtime | Assembles the server process from CLI/env inputs and owns subsystem lifecycles. |
| 3 | Identity, Authorization, and Audit | Authenticates login routes, applies default-deny authorization, and records administrative reads. |
| 4 | Collaboration Plane | Serves the TCP long-connection face for sessions, messaging, presence, and offline delivery. |
| 5 | File, Tool, and Integration Plane | Serves HTTP object/file workflows and integration boundaries. |
| 6 | Admin/Ops Console | Exposes an independent HTTP control face over the server store. |
| 7 | Desktop Client | Provides Qt UI, dual direct/collaboration engines, and persistent local state. |
| 8 | Mobile Clients | Provides collab-only Android, iOS, and HarmonyOS UI and transport adapters. |

## 1. Protocol

- **Implementation:** `common/include/memex/protocol/frame.hpp` and `common/src/frame.cpp` define a four-byte big-endian length prefix and a streaming decoder that tolerates partial TCP reads; `common/proto/memex.proto` defines the protobuf `Envelope` and reserved message-type numbers. iOS and Harmony mirror the codec in their own runtimes (`apps/ios/Sources/MemexKit/FrameCodec.swift:3`, `apps/harmony/core/frame_codec.ts:2`).
- **Configuration schema:** None. Limits and frame rules are source constants (`common/include/memex/protocol/frame.hpp:20`, `common/proto/memex.proto:9`).
- **Dedicated protocol file:** `common/proto/memex.proto`; the Android build consumes this same file (`apps/android/app/build.gradle.kts:10`, `apps/android/app/build.gradle.kts:49`).
- **Text/copy location:** No product copy. Runtime diagnostics live beside implementations (`common/src/frame.cpp:25`, `common/tests/test_protocol.cpp:106`).

## 2. Server Runtime

- **Implementation:** `server/src/main.cpp` dispatches subcommands, parses `serve` inputs, constructs storage and optional planes, accepts `SIGINT`/`SIGTERM`, and shuts down background workers (`server/src/main.cpp:3093`, `server/src/main.cpp:64`, `server/src/main.cpp:374`).
- **Configuration schema:** CLI flags are defined and validated in `server/src/main.cpp:88`; S3 and LDAP env fallbacks are applied at `server/src/main.cpp:245` and `server/src/main.cpp:252`. The relevant structs are `S3Config` (`server/src/storage.hpp:16`), `UplinkPolicy` (`server/src/files_server.hpp:88`), and `LdapConfig` (`server/src/ldap.hpp:26`). LDAP secrets should prefer env (`server/src/ldap.hpp:44`).
- **Dedicated protocol file:** none; this layer consumes the common protocol.
- **Text/copy location:** startup and degradation messages are emitted from `server/src/main.cpp:349`, `server/src/main.cpp:391`, and `server/src/main.cpp:447`.

## 3. Identity, Authorization, and Audit

- **Implementation:** `Session::handle_login` routes credentials through local or LDAP paths (`server/src/session.cpp:154`, `server/src/session.cpp:907`); `LdapDirectory` performs BER/bind/search (`server/src/ldap.hpp:97`); `AuthorizationService` resolves explicit deny, explicit allow, inherited, and default-deny decisions (`server/src/authz.hpp:40`, `server/src/authz.cpp:50`). Read-audit operations write to `audit_reads` (`server/src/store.cpp:326`).
- **Configuration schema:** `LdapConfig` (`server/src/ldap.hpp:26`); login/device rules are persisted in `ServerStore` tables such as `accounts`, `login_records`, and `devices` (`server/src/store.cpp:78`, `server/src/store.cpp:85`, `server/src/store.cpp:107`).
- **Dedicated protocol file:** none.
- **Text/copy location:** login results and authorization denial diagnostics (`server/src/session.cpp:1019`, `server/src/session.cpp:1024`, `admin/src/admin_app.cpp:162`).

## 4. Collaboration Plane

- **Implementation:** `CollabServer` owns the acceptor and presence/kick coordination (`server/src/server.hpp:23`, `server/src/server.cpp:140`); `Session` performs frame decoding, login dispatch, message routing, and offline persistence (`server/src/session.hpp:17`, `server/src/session.cpp:154`). `deliver_notice` and `deliver_group_text` reuse this plane for outbound notices and tool results (`server/src/server.hpp:47`, `server/src/server.hpp:48`).
- **Configuration schema:** only the assembled port and server store are injected (`server/src/main.cpp:374`); policy data remains in `ServerStore`.
- **Dedicated protocol file:** `common/proto/memex.proto`.
- **Text/copy location:** session errors and kick/logout payloads (`server/src/session.cpp:1019`, `server/src/session.cpp:1037`); product-visible mobile strings live under each mobile UI resource tree.

## 5. File, Tool, and Integration Plane

- **Implementation:** `FileServer` routes internal `/files/*` and one-way `/uplink/*` HTTP workflows (`server/src/files_server.hpp:107`, `server/src/files_server.cpp:808`); `FileSessions` is the in-memory token/session map (`server/src/files_server.cpp:190`); `S3StorageImpl` signs and transfers objects (`server/src/storage.cpp:37`); `WebhookServer`, `ModelGatewayServer`, `GroupAssistantWorker`, and `TaskPuller` are separately assembled integration workers (`server/src/webhook.hpp:54`, `server/src/model_gateway.hpp:32`, `server/src/group_assistant.hpp:18`, `server/src/task_pull.hpp:57`).
- **Configuration schema:** `UplinkPolicy` (`server/src/files_server.hpp:88`), `S3Config` (`server/src/storage.hpp:16`), task-pull credentials from CLI/env (`server/src/main.cpp:85`, `server/src/main.cpp:148`), and model/assistant settings in `ServerStore` plus `MEMEX_ASSISTANT_TOKEN` (`server/src/main.cpp:493`).
- **Dedicated protocol file:** collaboration notification results reuse protobuf; HTTP/JSON and S3 interfaces are implementation contracts rather than a Memex-specific file.
- **Text/copy location:** HTTP error strings, uplink exposure/degradation messages, and assistant instructions (`server/src/files_server.cpp:828`, `server/src/main.cpp:391`, `server/src/main.cpp:492`).

## 6. Admin/Ops Console

- **Implementation:** `setup()` registers `/api/*` Drogon routes and static SPA hosting (`admin/src/admin_app.cpp:201`, `admin/src/admin_app.cpp:3937`); `AdminContext` owns the `ServerStore`, authorization service, and health URLs (`admin/src/admin_app.cpp:153`); vanilla JS implements hash routing and same-origin API calls (`admin/www/admin.js:1`, `admin/www/admin.js:7`).
- **Configuration schema:** CLI overrides followed by `MEMEX_ADMIN_*`, `MEMEX_FILES_URL`, and `MEMEX_STORAGE_URL` env inputs (`admin/src/admin_app.cpp:201`, `admin/src/admin_app.cpp:218`).
- **Dedicated protocol file:** none. Admin uses same-origin `/api/*` JSON; only file health is proxied (`admin/src/admin_app.cpp:3914`).
- **Text/copy location:** the Chinese UI copy is in `admin/www/admin.js`; server-side errors are also returned from route handlers (`admin/src/admin_app.cpp:162`, `admin/src/admin_app.cpp:3946`).

## 7. Desktop Client

- **Implementation:** `MainWindow` and Qt dialogs compose presentation and OS integration (`client/app/main_window.cpp:66`); `DirectEngine`, `DiscoveryService`, and `SecureChannel` form the zero-login direct plane (`client/engine/direct/direct_engine.hpp:38`, `client/engine/direct/discovery.hpp:53`, `client/engine/direct/secure_channel.hpp:63`); `CollabEngine` and `FilesClient` form the logged-in plane (`client/engine/collab/collab_engine.hpp:20`, `client/engine/collab/files_client.hpp:20`); `LocalStore` persists both faces (`client/core/local_store.hpp:72`, `client/core/local_store.cpp:21`).
- **Configuration schema:** `QSettings("memex", "collab")` inputs are used throughout the app (`client/app/collab_defaults.hpp:51`, `client/app/task_provider_store.cpp:19`, `client/app/main_window.cpp:7128`); platform/default ports are constants (`client/app/collab_defaults.hpp:20`, `client/app/collab_defaults.hpp:23`, `client/engine/direct/direct_transport.hpp:30`, `client/engine/direct/discovery.hpp:27`).
- **Dedicated protocol file:** collab uses `common/proto/memex.proto`; UDP discovery/FeiQ compatibility is a separate JSON contract (`common/proto/memex.proto:3`, `client/engine/direct/ipmsg.hpp:14`).
- **Text/copy location:** Qt dialog sources under `client/app`, including status/help text (`client/app/main_window.cpp:7708`); theme assets and built-in copy remain in Qt resources.

## 8. Mobile Clients

- **Implementation:** Android `ChatSession`/`ChatManager`/`ChatHolder` transport collab state into Activity UI (`apps/android/app/src/main/java/com/memex/im/core/ChatSession.kt:40`, `apps/android/app/src/main/java/com/memex/im/core/ChatManager.kt:14`, `apps/android/app/src/main/java/com/memex/im/core/ChatHolder.kt:7`); iOS `MemexClient`/`ChatSession`/`AppChatManager` use SwiftPM core plus SwiftUI shell (`apps/ios/Sources/MemexKit/MemexClient.swift:29`, `apps/ios/Sources/MemexKit/ChatSession.swift:62`, `apps/ios/Sources/MemexApp/AppChatManager.swift:26`); Harmony `MemexClient`/`ChatSession`/`AppState` use injected transports (`apps/harmony/core/client.ts:27`, `apps/harmony/core/session.ts:63`, `apps/harmony/entry/src/main/ets/common/AppState.ets:129`). Route guards enforce the single startup flow (`apps/android/app/src/main/java/com/memex/im/core/InitStore.kt:28`, `apps/ios/Sources/MemexKit/AppState.swift:12`, `apps/harmony/core/route.ts:29`).
- **Configuration schema:** Android uses `InitStore` + `PrefsInitStore` SharedPreferences (`apps/android/app/src/main/java/com/memex/im/core/InitStore.kt:7`, `apps/android/app/src/main/java/com/memex/im/core/PrefsInitStore.kt:8`); iOS uses `ServerSettings`/`UserDefaults` (`apps/ios/Sources/MemexApp/ServerSettings.swift:7`); Harmony uses preferences-backed `PrefsInitStore` and `PrefsSeqLedger` (`apps/harmony/entry/src/main/ets/common/AppState.ets:50`, `apps/harmony/entry/src/main/ets/common/AppState.ets:95`).
- **Dedicated protocol file:** Android compiles the shared proto (`apps/android/app/build.gradle.kts:10`); iOS/Harmony implement the same wire shape in Swift/TypeScript.
- **Text/copy location:** Android resources and activities, SwiftUI views, and ArkTS pages respectively (`apps/android/app/src/main/java/com/memex/im/ui/MainActivity.kt:42`, `apps/ios/Sources/MemexApp/RootView.swift:4`, `apps/harmony/entry/src/main/ets/pages/Index.ets:10`).

## Storage Map

| Component | Storage | Evidence |
| --- | --- | --- |
| ServerStore | One SQLite file containing accounts, archive, groups, files metadata, integrations, and audit tables | `server/src/store.hpp:257`, `server/src/store.cpp:52`, `server/src/store.cpp:78` |
| Object bytes | S3-compatible backend behind `S3Storage` | `server/src/storage.hpp:48`, `server/src/storage.cpp:37` |
| File HTTP sessions | Process memory map | `server/src/files_server.cpp:190`, `server/src/files_server.cpp:430` |
| Admin runtime | ServerStore direct access + Drogon cookie session | `admin/src/admin_app.cpp:153`, `admin/src/admin_app.cpp:3940` |
| Desktop local data | SQLite through QtSql | `client/core/local_store.cpp:21`, `client/core/local_store.cpp:57` |
| Desktop settings | `QSettings`/platform config files | `client/app/task_provider_store.cpp:21`, `client/app/main_window.cpp:7128` |
| Android runtime messages | In-memory store (current app factory) | `apps/android/app/src/main/java/com/memex/im/core/ChatStore.kt:65`, `apps/android/app/src/main/java/com/memex/im/core/ChatHolder.kt:42` |
| Android init/sequence | SharedPreferences | `apps/android/app/src/main/java/com/memex/im/core/PrefsInitStore.kt:8`, `apps/android/app/src/main/java/com/memex/im/core/SeqLedger.kt:32` |
| iOS runtime messages | In-memory store | `apps/ios/Sources/MemexKit/ChatStore.swift:87` |
| iOS init/sequence | UserDefaults | `apps/ios/Sources/MemexApp/ServerSettings.swift:7`, `apps/ios/Sources/MemexApp/AppChatManager.swift:6` |
| Harmony runtime messages | In-memory store | `apps/harmony/core/chat_store.ts:51`, `apps/harmony/entry/src/main/ets/common/AppState.ets:133` |
| Harmony init/sequence | `@ohos.data.preferences` | `apps/harmony/entry/src/main/ets/common/AppState.ets:57`, `apps/harmony/entry/src/main/ets/common/AppState.ets:100` |

## Call Face

| Caller | Channel | Callee | Evidence |
| --- | --- | --- | --- |
| Desktop / mobile client | TCP 24360 protobuf | Collaboration Plane | `server/src/main.cpp:43`, `apps/ios/Sources/MemexKit/ServerAddress.swift:12`, `apps/harmony/core/address.ts:8` |
| Desktop / mobile client | HTTP 24561 JSON/files | File, Tool, and Integration Plane | `client/app/collab_defaults.hpp:23`, `apps/ios/Sources/MemexKit/FilesClient.swift:64`, `apps/harmony/core/files_client.ts:83` |
| Desktop | UDP 2425 discovery; TCP 2426–2437 peer transfer | Desktop peer | `client/engine/direct/discovery.hpp:27`, `client/engine/direct/direct_transport.hpp:2`, `client/engine/direct/direct_transport.hpp:30` |
| Browser | Same-origin HTTP `/api/*` | Admin/Ops Console backend | `admin/www/admin.js:3`, `admin/src/admin_app.cpp:3937` |
| Admin backend | Direct library call | ServerStore | `admin/src/admin_app.cpp:235` |
| Admin backend | HTTP `/files/health` proxy | File plane | `admin/src/admin_app.cpp:3914` |
| External webhook producer | Independent HTTP port | WebhookServer | `server/src/main.cpp:44`, `server/src/webhook.hpp:54` |
| External model caller | Independent HTTP port | ModelGatewayServer | `server/src/main.cpp:47`, `server/src/model_gateway.hpp:32` |

## Why These Eight Layers

1. **Protocol is a boundary, not a subsystem.** The repository explicitly names `common/proto/memex.proto` the single source of wire truth and forbids changing assigned type numbers (`common/proto/memex.proto:1-3`); Android compiles that same file (`apps/android/app/build.gradle.kts:10-15`).
2. **Server runtime is separate from every optional listener.** `cmd_serve` assembles storage, collaboration, file/uplink, webhook, model, assistant, and worker objects, then runs one Asio loop (`server/src/main.cpp:64`, `server/src/main.cpp:374`, `server/src/main.cpp:435`).
3. **Authentication and authorization have different decision paths.** LDAP is fail-closed and deliberately does not fall back to a local password (`server/src/ldap.hpp:8-12`), while `AuthorizationService` defines explicit deny, explicit allow, inherited, and default-deny precedence (`server/src/authz.hpp:24`, `server/src/authz.cpp:50`).
4. **Collaboration owns long-connection state.** A socket maps to one `Session`; that object decodes frames, handles login/routing, and calls the same server for presence and delivery (`server/src/session.hpp:17`, `server/src/session.cpp:154`, `server/src/session.cpp:907`).
5. **Files and integrations have different transport and lifecycle needs.** File traffic uses HTTP, in-memory sessions, and S3 bytes (`server/src/files_server.hpp:107`, `server/src/files_server.cpp:190`, `server/src/storage.cpp:37`), while webhook/model/assistant/task workers run independently (`server/src/webhook.hpp:54`, `server/src/model_gateway.hpp:32`, `server/src/group_assistant.hpp:18`, `server/src/task_pull.hpp:57`).
6. **Admin has a separate trust face and process model.** It opens the same database directly, serializes on one IO thread with a mutex fallback, and exposes only `/api/*` plus its SPA (`admin/src/admin_app.cpp:231`, `admin/src/admin_app.cpp:3937`, `admin/src/admin_app.cpp:3944`).
7. **Desktop combines two clients because one user experience must switch faces.** Direct and collab engines are distinct classes, while one `LocalStore` carries shared message state (`client/engine/direct/direct_engine.hpp:38`, `client/engine/collab/collab_engine.hpp:20`, `client/core/local_store.hpp:72`).
8. **Mobile clients are separate because their runtimes and persistence differ.** They share only the wire contract; each platform has its own transport, route guard, UI shell, and preference storage (`apps/android/app/build.gradle.kts:10`, `apps/ios/Sources/MemexKit/ChatSession.swift:62`, `apps/harmony/core/session.ts:63`, `apps/harmony/entry/src/main/ets/common/AppState.ets:57`).
