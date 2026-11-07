# Architecture Glossary

> Source-layer analysis of the Memex codebase (2026-11-07). Each abstract-layer concept is listed one per row. The `file:line` column cites the primary definition or entry point. The Chinese page is [架构术语表](/zh/guide/glossary); the full runtime picture is [Source Layer Architecture](/guide/architecture).

| 术语 (term) | 英文 (English) | 定义 (definition) | file:line |
| --- | --- | --- | --- |
| 协议层 | Protocol layer | 单一事实源的线上协议：帧格式（4 字节大端长度前缀）+ `Envelope` 序列化载荷，类型编号只增不改 | `common/proto/memex.proto:1`, `common/include/memex/protocol/frame.hpp:35` |
| 帧编解码 | Frame codec | 流式 `FrameDecoder` 容忍任意 TCP 切分（粘包/半包），畸形输入报 `ProtocolError` 不崩溃；iOS/鸿蒙实现同一线格式 | `common/src/frame.cpp:25`, `common/src/frame.cpp:52`, `apps/ios/Sources/MemexKit/FrameCodec.swift:3`, `apps/harmony/core/frame_codec.ts:2` |
| 消息信封 | Envelope | 所有帧的载荷体，`oneof body` 字段号 = 100 + 对应类型编号 | `common/proto/memex.proto:367` |
| 服务端接入面 | Server gateway | TCP 长连接接受器（默认 24360），`CollabServer` 管理在线会话表与单点互踢 | `server/src/server.cpp:140`, `server/src/server.cpp:145` |
| 会话处理器 | Session handler | 一连接一会话：帧解码、心跳、登录校验、消息路由（T2.2） | `server/src/session.hpp:17`, `server/src/session.cpp:154` |
| 服务端存储层 | Server store | SQLite 单库（账号/登录/设备/归档/群/回执/审计/文件元数据），只做存储不含业务策略 | `server/src/store.hpp:257`, `server/src/store.hpp:266` |
| 归档去重 | Archive dedup | `sha256(from:seq)` 派生 msg_id；撞键内容异则换盐重排（BUG-007） | `server/src/session.cpp:110` |
| 授权服务 | Authorization service | 统一裁决引擎：`ExplicitDeny > ExplicitAllow > Inherited`，无命中=default-deny（白名单口径） | `server/src/authz.hpp:40`, `server/src/authz.cpp:50` |
| 文件面 | File plane | HTTP 文件面（独立端口）：上传/下载/列表/配额/备忘录/群工具/凭据等 `/files/*` 路由 | `server/src/files_server.hpp:107`, `server/src/files_server.cpp:766` |
| 对象存储抽象 | S3 storage abstraction | S3 兼容存储抽象（R23-1）：签名/分片/预签名，默认 RustFS/MinIO/云 S3 | `server/src/storage.hpp:48`, `server/src/storage.cpp:37` |
| 外网单向面 | Uplink plane | 外网单向上传面（R23-4）：`/uplink/*` 只写入，独立监听口 | `server/src/files_server.cpp:808` |
| 通知接入 | Webhook access | 独立端口 HTTP POST 接收器（T4.10），投递面镜像 TEXT（离线入队+归档+在线扇出） | `server/src/webhook.hpp:39`, `server/src/webhook.hpp:54` |
| 模型网关 | Model gateway | OpenAI 兼容入口（平台三期）：独立端口独立线程，归档红线只路由 `is_local` 端点 | `server/src/model_gateway.hpp:32` |
| 企业目录认证 | Enterprise directory auth | LDAP/AD 可选判登：fail-closed 不回落本地口令，本地账号常备 | `server/src/ldap.hpp:97`, `server/src/ldap.hpp:150` |
| 群内智能助手 | In-group assistant | 独立线程轮询 bot 队列，四指令（@助手/@纪要/@整理/@检索），回群走回环 /bot/send | `server/src/group_assistant.hpp:18` |
| 任务源拉取 | Task puller | 60s 主循拉外部任务（GitHub 起步），三道闸逐道短路=零外呼 | `server/src/task_pull.hpp:57`, `server/src/task_pull.hpp:76` |
| 服务器 agent | Server agent | 轻量常驻（Linux 起步）：向文件面心跳上报本机负载，只读采样不开监听 | `server/src/agent_main.cpp:1` |
| 凭据面 | Credential plane | PBKDF2 口令摘要 + 服务端 AES-256-GCM 凭据密封（工具/任务源） | `server/src/cred.hpp:16`, `server/src/cred.hpp:27` |
| 客户端 UI 层 | Client UI layer | Qt 主窗与全部对话框：窗口、系统集成（托盘/通知/自启）、截图标注、主题 | `client/app/main_window.cpp:66` |
| 客户端直连引擎 | Client direct engine | 未登录零配置直连态：UDP 发现 + TCP 点对点文本 + 文件传输 + 本地 SQLite 历史 | `client/engine/direct/direct_engine.hpp:38`, `client/engine/direct/direct_engine.hpp:57` |
| 发现服务 | Discovery service | UDP 2425 周期宣告与同网段在线表；飞秋/IPMSG 兼容（首字节分流） | `client/engine/direct/discovery.hpp:53`, `client/engine/direct/discovery.hpp:17` |
| 直连安全信道 | Direct secure channel | 设备身份（Ed25519）+ X25519 握手 + AEAD，TOFU 定针；无明文回退 | `client/engine/direct/secure_channel.hpp:63`, `client/engine/direct/secure_channel.hpp:1` |
| 客户端协作引擎 | Client collab engine | 登录协作服务端长连接：心跳/重连/离线补投/消息路由/断线补传 | `client/engine/collab/collab_engine.hpp:20`, `client/engine/collab/collab_engine.hpp:38` |
| 文件客户端 | Files client | FileServer HTTP API 的 Qt 封装；会话令牌内存持有，客户端永不直连对象存储 | `client/engine/collab/files_client.hpp:20` |
| 客户端本地存储 | Client local store | 本地 SQLite：消息/会话/文件元数据/配额/身份种子/扫描白名单 | `client/core/local_store.hpp:72`, `client/core/local_store.cpp:21` |
| 本地存储抽象 | Storage interface | 直连引擎可调用的本地存储接口；当前主实现是 `StorageLocal` | `client/core/storage_interface.hpp:33`, `client/core/storage_local.hpp:12` |
| 任务 Provider SPI | Task provider SPI | 外部任务能力声明制（L1 跳转/L2 只读/L3 双向），注册表装配内置预设 | `client/engine/task/task_provider.hpp:47`, `client/engine/task/task_provider.hpp:138` |
| 配置输入（服务端） | Server config input | CLI 旗标 + env 兜底（`--port`/`--db`/`--files-port`/`--uplink-port`/`--ldap-*`/`--s3-*` 等） | `server/src/main.cpp:64`, `server/src/main.cpp:100` |
| 配置输入（客户端） | Client config input | `QSettings("memex","collab")` 等持久化设置（主题/通知/网段黑名单/文件端口） | `client/app/collab_defaults.hpp:51`, `client/app/main_window.cpp:7128` |
| 路由守卫 | Route guard | 移动端唯一启动流：未初始化进向导，已初始化未登录进登录页，两者齐备进主界面 | `apps/android/app/src/main/java/com/memex/im/core/InitStore.kt:28`, `apps/ios/Sources/MemexKit/AppState.swift:12`, `apps/harmony/core/route.ts:29` |
| 应用模型 | App model | iOS 应用级状态机：按 ServerSettings 与 LoginState 重算入口，并持有 AppChatManager | `apps/ios/Sources/MemexApp/AppModel.swift:6` |
| 应用状态 | App state | Harmony 应用级装配：初始化态、内存消息表、常驻通知、登录态、会话和监听器 | `apps/harmony/entry/src/main/ets/common/AppState.ets:129` |
| 会话持有者 | Chat holder | Android 登录成功后移交的进程内存单例，负责界面共享和管理器重建 | `apps/android/app/src/main/java/com/memex/im/core/ChatHolder.kt:7` |
| 会话管理器 | Chat manager | 把长连接事件投递到 UI 线程，并聚合本地会话数据的平台桥 | `apps/android/app/src/main/java/com/memex/im/core/ChatManager.kt:14`, `apps/ios/Sources/MemexApp/AppChatManager.swift:26` |
| 管理台上下面 | Admin context | Admin 进程长命状态：直接打开的 ServerStore、授权服务、健康检查 URL 和互斥锁 | `admin/src/admin_app.cpp:153` |
| 服务地址 | Server address | 移动端解析/校验后的服务器主机与端口；协作态默认 24360 | `apps/harmony/core/address.ts:22`, `apps/ios/Sources/MemexKit/ServerAddress.swift:12` |
| 服务器设置 | Server settings | iOS 初始化地址与端口偏好存取，底层为 UserDefaults | `apps/ios/Sources/MemexApp/ServerSettings.swift:7` |
| 设备身份 | Device identity | 直连态本端 Ed25519 密钥；首次从 LocalStore 生成并复用 | `client/engine/direct/secure_channel.hpp:45`, `client/core/local_store.hpp:72` |
| 文件会话表 | File sessions | FileServer 进程内 Bearer 令牌到账号/过期/scope 的映射 | `server/src/files_server.cpp:190` |
| Uplink 策略 | Uplink policy | 外网单向面的扩展名黑白名单、独立上限、二次验证等安全参数 | `server/src/files_server.hpp:88` |
| 任务 Provider 存储 | Task provider store | 桌面任务源凭据的 AES-256-GCM 加密包，经 QSettings 落盘 | `client/app/task_provider_store.cpp:19`, `client/app/task_provider_store.cpp:91` |
| 管理台前端 | Admin SPA | 零构建 vanilla JS hash 路由；所有改写请求带同源 CSRF 轻闸头 | `admin/www/admin.js:1` |
| 配置输入（管理台） | Admin config input | CLI 优先，随后读取 `MEMEX_ADMIN_*`、文件与存储 URL | `admin/src/admin_app.cpp:201`, `admin/src/admin_app.cpp:218` |
| 移动协作客户端 | Mobile collab client | Android/iOS/鸿蒙仅登录后的协作态，各自实现传输、会话与 UI 桥 | `apps/android/app/src/main/java/com/memex/im/core/MemexClient.kt:51`, `apps/ios/Sources/MemexKit/MemexClient.swift:29`, `apps/harmony/core/client.ts:27` |
| 移动会话 | Mobile chat session | 平台长连接对象：分帧、发送 seq、ACK/KICK 分发和本地落库 | `apps/android/app/src/main/java/com/memex/im/core/ChatSession.kt:40`, `apps/ios/Sources/MemexKit/ChatSession.swift:62`, `apps/harmony/core/session.ts:63` |
| 移动消息存储 | Mobile chat store | 平台本地消息/会话接口；当前 App 工厂均为内存实现 | `apps/android/app/src/main/java/com/memex/im/core/ChatStore.kt:38`, `apps/ios/Sources/MemexKit/ChatStore.swift:57`, `apps/harmony/core/chat_store.ts:32` |
| 移动初始化存储 | Mobile init store | 平台初始化地址落盘：SharedPreferences、UserDefaults 或 `@ohos.data.preferences` | `apps/android/app/src/main/java/com/memex/im/core/PrefsInitStore.kt:8`, `apps/ios/Sources/MemexApp/ServerSettings.swift:7`, `apps/harmony/entry/src/main/ets/common/AppState.ets:50` |
| 序号台账 | Sequence ledger | 移动端按账号持久化本地发送序号，重登续位并取 max 合并 | `apps/android/app/src/main/java/com/memex/im/core/SeqLedger.kt:29`, `apps/ios/Sources/MemexApp/AppChatManager.swift:6`, `apps/harmony/entry/src/main/ets/common/AppState.ets:95` |
