# 源码分层架构

> 本页记录 2026-11-07 从源码归纳出的运行时架构。这里的“层”按职责划分，
> 不按目录名划分。术语见 [架构术语表](/zh/guide/glossary)；英文页为
> [Source Layer Architecture](/guide/architecture)。

## 层清单

| # | 层 | 职责 |
| --- | --- | --- |
| 1 | 协议 | 连接端共享的线上契约。 |
| 2 | 服务端运行时 | 用 CLI/env 输入装配进程，管理各子系统生命周期。 |
| 3 | 身份、授权与审计 | 处理登录路由判登、默认拒绝授权，并记录管理面查阅。 |
| 4 | 协作面 | 提供 TCP 长连接会话、消息路由、在线与离线投递。 |
| 5 | 文件、工具与集成面 | 提供 HTTP 对象文件工作流和外部集成边界。 |
| 6 | 管理/运维台 | 在服务端存储上暴露独立 HTTP 控制面。 |
| 7 | 桌面客户端 | 提供 Qt UI、直连/协作双引擎和本地持久状态。 |
| 8 | 移动客户端 | 提供 Android/iOS/鸿蒙的仅协作态 UI 和传输适配。 |

## 1. 协议层

- **实现：** `common/include/memex/protocol/frame.hpp` 与 `common/src/frame.cpp` 定义 4 字节大端长度前缀和容忍半包的流式解码器；`common/proto/memex.proto` 定义 protobuf `Envelope` 和只增不改的类型编号。iOS 与鸿蒙在各自运行时镜像同一 codec（`apps/ios/Sources/MemexKit/FrameCodec.swift:3`、`apps/harmony/core/frame_codec.ts:2`）。
- **配置 schema：** 无；上限和帧规则是源码常量（`common/include/memex/protocol/frame.hpp:20`、`common/proto/memex.proto:9`）。
- **专属协议文件：** `common/proto/memex.proto`；Android 构建消费同一份文件（`apps/android/app/build.gradle.kts:10`、`apps/android/app/build.gradle.kts:49`）。
- **文案挂点：** 无产品文案；运行期诊断在实现旁边（`common/src/frame.cpp:25`、`common/tests/test_protocol.cpp:106`）。

## 2. 服务端运行时

- **实现：** `server/src/main.cpp` 分发子命令、解析 `serve` 输入、构造存储和可选面、接收 `SIGINT`/`SIGTERM` 并停止后台工作器（`server/src/main.cpp:3093`、`server/src/main.cpp:64`、`server/src/main.cpp:374`）。
- **配置 schema：** CLI 旗标在 `server/src/main.cpp:88` 定义和校验；S3/LDAP env 兜底在 `server/src/main.cpp:245`、`server/src/main.cpp:252`。对应结构体是 `S3Config`（`server/src/storage.hpp:16`）、`UplinkPolicy`（`server/src/files_server.hpp:88`）、`LdapConfig`（`server/src/ldap.hpp:26`）；LDAP 口令建议 env 优先（`server/src/ldap.hpp:44`）。
- **专属协议文件：** 无；本层消费公共协议。
- **文案挂点：** 启动与降级文案在 `server/src/main.cpp:349`、`server/src/main.cpp:391`、`server/src/main.cpp:447`。

## 3. 身份、授权与审计层

- **实现：** `Session::handle_login` 把凭据路由到本地或 LDAP 路径（`server/src/session.cpp:154`、`server/src/session.cpp:907`）；`LdapDirectory` 完成 BER/bind/search（`server/src/ldap.hpp:97`）；`AuthorizationService` 按 explicit deny、explicit allow、inherited、default-deny 裁决（`server/src/authz.hpp:40`、`server/src/authz.cpp:50`）。查阅类操作写入 `audit_reads`（`server/src/store.cpp:326`）。
- **配置 schema：** `LdapConfig`（`server/src/ldap.hpp:26`）；登录/设备规则落在 `ServerStore` 的 `accounts`、`login_records`、`devices` 等表（`server/src/store.cpp:78`、`server/src/store.cpp:85`、`server/src/store.cpp:107`）。
- **专属协议文件：** 无。
- **文案挂点：** 登录结果和授权拒绝诊断（`server/src/session.cpp:1019`、`server/src/session.cpp:1024`、`admin/src/admin_app.cpp:162`）。

## 4. 协作面

- **实现：** `CollabServer` 持有 acceptor、在线/互踢协调（`server/src/server.hpp:23`、`server/src/server.cpp:140`）；`Session` 做帧解码、登录分发、消息路由和离线持久化（`server/src/session.hpp:17`、`server/src/session.cpp:154`）。`deliver_notice` 与 `deliver_group_text` 把通知和工具结果复用到本面（`server/src/server.hpp:47`、`server/src/server.hpp:48`）。
- **配置 schema：** 装配时注入端口和 ServerStore（`server/src/main.cpp:374`）；策略数据留在 ServerStore。
- **专属协议文件：** `common/proto/memex.proto`。
- **文案挂点：** 会话错误、互踢/登出载荷（`server/src/session.cpp:1019`、`server/src/session.cpp:1037`）；移动端产品文案在各平台 UI 资源树。

## 5. 文件、工具与集成面

- **实现：** `FileServer` 路由内网 `/files/*` 和外网单向 `/uplink/*` HTTP 工作流（`server/src/files_server.hpp:107`、`server/src/files_server.cpp:808`）；`FileSessions` 是内存令牌/会话表（`server/src/files_server.cpp:190`）；`S3StorageImpl` 完成签名和对象传输（`server/src/storage.cpp:37`）；`WebhookServer`、`ModelGatewayServer`、`GroupAssistantWorker`、`TaskPuller` 是独立装配的集成工作器（`server/src/webhook.hpp:54`、`server/src/model_gateway.hpp:32`、`server/src/group_assistant.hpp:18`、`server/src/task_pull.hpp:57`）。
- **配置 schema：** `UplinkPolicy`（`server/src/files_server.hpp:88`）、`S3Config`（`server/src/storage.hpp:16`）、CLI/env 任务拉取凭据（`server/src/main.cpp:85`、`server/src/main.cpp:148`），以及 ServerStore 中的模型配置和 `MEMEX_ASSISTANT_TOKEN`（`server/src/main.cpp:493`）。
- **专属协议文件：** 通知回群复用 protobuf；HTTP/JSON 与 S3 接口是实现契约，不是 Memex 专属协议文件。
- **文案挂点：** HTTP 错误串、uplink 暴露/降级文案、群助手指令（`server/src/files_server.cpp:828`、`server/src/main.cpp:391`、`server/src/main.cpp:492`）。

## 6. 管理/运维台

- **实现：** `setup()` 注册 Drogon `/api/*` 路由并托管静态 SPA（`admin/src/admin_app.cpp:201`、`admin/src/admin_app.cpp:3937`）；`AdminContext` 持有 `ServerStore`、授权服务和健康检查 URL（`admin/src/admin_app.cpp:153`）；vanilla JS 实现 hash 路由和同源 API 调用（`admin/www/admin.js:1`、`admin/www/admin.js:7`）。
- **配置 schema：** CLI 优先，随后读取 `MEMEX_ADMIN_*`、`MEMEX_FILES_URL`、`MEMEX_STORAGE_URL`（`admin/src/admin_app.cpp:201`、`admin/src/admin_app.cpp:218`）。
- **专属协议文件：** 无。管理台使用同源 `/api/*` JSON；仅文件健康检查做 HTTP 代理（`admin/src/admin_app.cpp:3914`）。
- **文案挂点：** 中文 UI 文案在 `admin/www/admin.js`；服务端错误也由路由处理器返回（`admin/src/admin_app.cpp:162`、`admin/src/admin_app.cpp:3946`）。

## 7. 桌面客户端

- **实现：** `MainWindow` 与 Qt 对话框组合展示层和系统集成（`client/app/main_window.cpp:66`）；`DirectEngine`、`DiscoveryService`、`SecureChannel` 组成免登录直连面（`client/engine/direct/direct_engine.hpp:38`、`client/engine/direct/discovery.hpp:53`、`client/engine/direct/secure_channel.hpp:63`）；`CollabEngine` 和 `FilesClient` 组成登录面（`client/engine/collab/collab_engine.hpp:20`、`client/engine/collab/files_client.hpp:20`）；`LocalStore` 同时承载两态本地数据（`client/core/local_store.hpp:72`、`client/core/local_store.cpp:21`）。
- **配置 schema：** 应用多处使用 `QSettings("memex", "collab")`（`client/app/collab_defaults.hpp:51`、`client/app/task_provider_store.cpp:19`、`client/app/main_window.cpp:7128`）；平台/缺省端口是源码常量（`client/app/collab_defaults.hpp:20`、`client/app/collab_defaults.hpp:23`、`client/engine/direct/direct_transport.hpp:30`、`client/engine/direct/discovery.hpp:27`）。
- **专属协议文件：** 协作态使用 `common/proto/memex.proto`；UDP 发现/飞秋兼容是独立 JSON 契约（`common/proto/memex.proto:3`、`client/engine/direct/ipmsg.hpp:14`）。
- **文案挂点：** `client/app` 下的 Qt 对话框源码，包括状态/帮助文本（`client/app/main_window.cpp:7708`）；主题资源和内置文案留在 Qt 资源。

## 8. 移动客户端

- **实现：** Android 的 `ChatSession`/`ChatManager`/`ChatHolder` 把协作态接到 Activity UI（`apps/android/app/src/main/java/com/memex/im/core/ChatSession.kt:40`、`apps/android/app/src/main/java/com/memex/im/core/ChatManager.kt:14`、`apps/android/app/src/main/java/com/memex/im/core/ChatHolder.kt:7`）；iOS 的 `MemexClient`/`ChatSession`/`AppChatManager` 由 SwiftPM core 加 SwiftUI 壳组成（`apps/ios/Sources/MemexKit/MemexClient.swift:29`、`apps/ios/Sources/MemexKit/ChatSession.swift:62`、`apps/ios/Sources/MemexApp/AppChatManager.swift:26`）；鸿蒙的 `MemexClient`/`ChatSession`/`AppState` 注入传输（`apps/harmony/core/client.ts:27`、`apps/harmony/core/session.ts:63`、`apps/harmony/entry/src/main/ets/common/AppState.ets:129`）。三端路由守卫强制唯一启动流（`apps/android/app/src/main/java/com/memex/im/core/InitStore.kt:28`、`apps/ios/Sources/MemexKit/AppState.swift:12`、`apps/harmony/core/route.ts:29`）。
- **配置 schema：** Android 用 `InitStore` + `PrefsInitStore` SharedPreferences（`apps/android/app/src/main/java/com/memex/im/core/InitStore.kt:7`、`apps/android/app/src/main/java/com/memex/im/core/PrefsInitStore.kt:8`）；iOS 用 `ServerSettings`/`UserDefaults`（`apps/ios/Sources/MemexApp/ServerSettings.swift:7`）；鸿蒙用 preferences 实现的 `PrefsInitStore` 与 `PrefsSeqLedger`（`apps/harmony/entry/src/main/ets/common/AppState.ets:50`、`apps/harmony/entry/src/main/ets/common/AppState.ets:95`）。
- **专属协议文件：** Android 编译共享 proto（`apps/android/app/build.gradle.kts:10`）；iOS/鸿蒙用 Swift/TypeScript 实现同一线格式。
- **文案挂点：** 分别是 Android 资源和 Activity、SwiftUI 视图、ArkTS 页面（`apps/android/app/src/main/java/com/memex/im/ui/MainActivity.kt:42`、`apps/ios/Sources/MemexApp/RootView.swift:4`、`apps/harmony/entry/src/main/ets/pages/Index.ets:10`）。

## 状态存储

| 组件 | 存储 | 证据 |
| --- | --- | --- |
| ServerStore | 单 SQLite 文件，含账号、归档、群、文件元数据、集成和审计表 | `server/src/store.hpp:257`、`server/src/store.cpp:52`、`server/src/store.cpp:78` |
| 对象字节 | `S3Storage` 后面的 S3 兼容存储 | `server/src/storage.hpp:48`、`server/src/storage.cpp:37` |
| 文件 HTTP 会话 | 进程内存表 | `server/src/files_server.cpp:190`、`server/src/files_server.cpp:430` |
| 管理台运行态 | 直接访问 ServerStore + Drogon cookie session | `admin/src/admin_app.cpp:153`、`admin/src/admin_app.cpp:3940` |
| 桌面本地数据 | QtSql 访问 SQLite | `client/core/local_store.cpp:21`、`client/core/local_store.cpp:57` |
| 桌面设置 | `QSettings`/平台配置文件 | `client/app/task_provider_store.cpp:21`、`client/app/main_window.cpp:7128` |
| Android 运行态消息 | 当前应用工厂使用内存存储 | `apps/android/app/src/main/java/com/memex/im/core/ChatStore.kt:65`、`apps/android/app/src/main/java/com/memex/im/core/ChatHolder.kt:42` |
| Android 初始化/序号 | SharedPreferences | `apps/android/app/src/main/java/com/memex/im/core/PrefsInitStore.kt:8`、`apps/android/app/src/main/java/com/memex/im/core/SeqLedger.kt:32` |
| iOS 运行态消息 | 内存存储 | `apps/ios/Sources/MemexKit/ChatStore.swift:87` |
| iOS 初始化/序号 | UserDefaults | `apps/ios/Sources/MemexApp/ServerSettings.swift:7`、`apps/ios/Sources/MemexApp/AppChatManager.swift:6` |
| 鸿蒙运行态消息 | 内存存储 | `apps/harmony/core/chat_store.ts:51`、`apps/harmony/entry/src/main/ets/common/AppState.ets:133` |
| 鸿蒙初始化/序号 | `@ohos.data.preferences` | `apps/harmony/entry/src/main/ets/common/AppState.ets:57`、`apps/harmony/entry/src/main/ets/common/AppState.ets:100` |

## 调用面

| 调用方 | 通道 | 被调方 | 证据 |
| --- | --- | --- | --- |
| 桌面/移动客户端 | TCP 24360 protobuf | 协作面 | `server/src/main.cpp:43`、`apps/ios/Sources/MemexKit/ServerAddress.swift:12`、`apps/harmony/core/address.ts:8` |
| 桌面/移动客户端 | HTTP 24561 JSON/文件 | 文件、工具与集成面 | `client/app/collab_defaults.hpp:23`、`apps/ios/Sources/MemexKit/FilesClient.swift:64`、`apps/harmony/core/files_client.ts:83` |
| 桌面 | UDP 2425 发现；TCP 2426–2437 点对点传输 | 桌面对端 | `client/engine/direct/discovery.hpp:27`、`client/engine/direct/direct_transport.hpp:2`、`client/engine/direct/direct_transport.hpp:30` |
| 浏览器 | 同源 HTTP `/api/*` | 管理/运维台后端 | `admin/www/admin.js:3`、`admin/src/admin_app.cpp:3937` |
| 管理台后端 | 直接库调用 | ServerStore | `admin/src/admin_app.cpp:235` |
| 管理台后端 | HTTP `/files/health` 代理 | 文件面 | `admin/src/admin_app.cpp:3914` |
| 外部 webhook 生产方 | 独立 HTTP 端口 | WebhookServer | `server/src/main.cpp:44`、`server/src/webhook.hpp:54` |
| 外部模型调用方 | 独立 HTTP 端口 | ModelGatewayServer | `server/src/main.cpp:47`、`server/src/model_gateway.hpp:32` |

## 为什么是这八层

1. **协议是边界，不是子系统。** 仓库明确把 `common/proto/memex.proto` 命名为线格式单一事实源，且类型编号只增不改（`common/proto/memex.proto:1-3`）；Android 编译同一份文件（`apps/android/app/build.gradle.kts:10-15`）。
2. **服务端运行时独立于每个可选监听器。** `cmd_serve` 装配存储、协作、文件/uplink、webhook、模型、助手和工作器，再运行同一个 Asio 循环（`server/src/main.cpp:64`、`server/src/main.cpp:374`、`server/src/main.cpp:435`）。
3. **认证与授权走不同决策路径。** LDAP fail-closed，且刻意不回落本地口令（`server/src/ldap.hpp:8-12`）；`AuthorizationService` 定义 explicit deny、explicit allow、inherited、default-deny 的优先级（`server/src/authz.hpp:24`、`server/src/authz.cpp:50`）。
4. **协作面拥有长连接状态。** 一个 socket 对应一个 `Session`；该对象解码帧、处理登录/路由，并调用同一 server 做在线与投递（`server/src/session.hpp:17`、`server/src/session.cpp:154`、`server/src/session.cpp:907`）。
5. **文件与集成的传输和生命周期不同。** 文件流量走 HTTP、内存会话和 S3 字节（`server/src/files_server.hpp:107`、`server/src/files_server.cpp:190`、`server/src/storage.cpp:37`）；webhook/模型/助手/任务工作器独立运行（`server/src/webhook.hpp:54`、`server/src/model_gateway.hpp:32`、`server/src/group_assistant.hpp:18`、`server/src/task_pull.hpp:57`）。
6. **管理台有独立信任面和进程模型。** 它直接打开同一个库，用单 IO 线程序列化并加互斥锁兜底，只暴露 `/api/*` 与 SPA（`admin/src/admin_app.cpp:231`、`admin/src/admin_app.cpp:3937`、`admin/src/admin_app.cpp:3944`）。
7. **桌面端组合两个客户端，因为一个用户体验要切换两态。** 直连与协作引擎是不同类，但共享一个 `LocalStore` 承载消息状态（`client/engine/direct/direct_engine.hpp:38`、`client/engine/collab/collab_engine.hpp:20`、`client/core/local_store.hpp:72`）。
8. **移动端必须分开，因为运行时和持久化不同。** 它们只共享线契约；各自有 transport、路由守卫、UI 壳和偏好存储（`apps/android/app/build.gradle.kts:10`、`apps/ios/Sources/MemexKit/ChatSession.swift:62`、`apps/harmony/core/session.ts:63`、`apps/harmony/entry/src/main/ets/common/AppState.ets:57`）。
