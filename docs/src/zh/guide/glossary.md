# 架构术语表

> 本表记录 2026-11-07 的源码分层分析，每个抽象层概念一行。`file:line` 列引用主要定义或入口。英文页为 [Architecture Glossary](/guide/glossary)；完整运行时结构见 [源码分层架构](/zh/guide/architecture)。

| 术语 | 英文 | 定义 | file:line |
| --- | --- | --- | --- |
| 协议层 | Protocol layer | 连接端共享的线上协议：4 字节大端长度前缀 + `Envelope` 序列化载荷，类型编号只增不改 | `common/proto/memex.proto:1`, `common/include/memex/protocol/frame.hpp:35` |
| 帧编解码 | Frame codec | 流式 `FrameDecoder` 容忍 TCP 粘包/半包；畸形输入报 `ProtocolError`，不崩溃 | `common/src/frame.cpp:25`, `common/src/frame.cpp:52` |
| 消息信封 | Envelope | 所有帧的 protobuf 载荷体；`oneof body` 字段号 = 100 + 对应类型编号 | `common/proto/memex.proto:367` |
| 服务端接入面 | Server gateway | TCP 长连接接受器（默认 24360）；`CollabServer` 管理在线表与单点互踢 | `server/src/server.cpp:140`, `server/src/server.cpp:145` |
| 会话处理器 | Session handler | 一连接一会话：帧解码、心跳、登录校验和消息路由 | `server/src/session.hpp:17`, `server/src/session.cpp:154` |
| 服务端存储层 | Server store | SQLite 单库，覆盖账号、登录、设备、归档、群、回执、审计和文件元数据 | `server/src/store.hpp:257`, `server/src/store.cpp:52` |
| 归档去重 | Archive dedup | `sha256(from:seq)` 派生 `msg_id`；撞键但内容不同时换盐重排 | `server/src/session.cpp:110` |
| 企业目录认证 | Enterprise directory auth | 可选 LDAP/AD 判登：目录不可用即拒绝，不回落本地口令；本地账号常备 | `server/src/ldap.hpp:97` |
| 授权服务 | Authorization service | 统一裁决：explicit deny > explicit allow > inherited > default deny | `server/src/authz.hpp:40`, `server/src/authz.cpp:50` |
| 文件面 | File plane | 独立 HTTP 端口上的 `/files/*` 上传、下载、列表、配额、备忘录、群工具和凭据面 | `server/src/files_server.hpp:107`, `server/src/files_server.cpp:766` |
| 文件会话表 | File sessions | FileServer 进程内 Bearer 令牌到账号、过期时间与 scope 的内存映射 | `server/src/files_server.cpp:190` |
| 对象存储抽象 | S3 storage abstraction | S3 兼容存储抽象，支持 RustFS/MinIO/云 S3；客户端不直连对象存储 | `server/src/storage.hpp:48`, `server/src/storage.cpp:37` |
| 外网单向面 | Uplink plane | 独立监听口上的 `/uplink/*` 只写面；跨面令牌被双向拒绝 | `server/src/files_server.cpp:808` |
| Uplink 策略 | Uplink policy | 外网面的扩展名黑白名单、独立大小上限和可选二次验证 | `server/src/files_server.hpp:88` |
| 通知接入 | Webhook access | 独立 HTTP 端口接收器；投递复用协作面的离线入队、归档和在线扇出 | `server/src/webhook.hpp:54` |
| 模型网关 | Model gateway | OpenAI 兼容入口；独立端口/线程，且只路由 `is_local` 端点 | `server/src/model_gateway.hpp:32` |
| 群内智能助手 | In-group assistant | 独立线程轮询 bot 队列，处理四指令并经 webhook 回环回群 | `server/src/group_assistant.hpp:18` |
| 任务源拉取 | Task puller | 主循环拉取外部任务；凭据、存储开关和任务源三道闸全开才外呼 | `server/src/task_pull.hpp:57` |
| 服务器 agent | Server agent | 轻量常驻代理：向文件面心跳上报本机负载，只读采样且不开监听 | `server/src/agent_main.cpp:1` |
| 凭据面 | Credential plane | PBKDF2 口令摘要加上服务端 AES-256-GCM 凭据密封 | `server/src/cred.hpp:16`, `server/src/cred.hpp:27` |
| 管理台上下面 | Admin context | Admin 进程长命状态：ServerStore、授权服务、健康检查 URL 和互斥锁 | `admin/src/admin_app.cpp:153` |
| 管理台前端 | Admin SPA | 零构建 vanilla JS hash 路由，所有改写请求带 CSRF 轻闸头 | `admin/www/admin.js:1` |
| 客户端 UI 层 | Client UI layer | Qt 主窗和对话框，承载窗口、托盘、通知、截图、主题与系统集成 | `client/app/main_window.cpp:66` |
| 客户端直连引擎 | Client direct engine | 免登录直连态：UDP 发现、TCP 点对点文本、文件传输和本地历史 | `client/engine/direct/direct_engine.hpp:38` |
| 发现服务 | Discovery service | UDP 2425 周期宣告与同网段在线表；可伴发飞秋/IPMSG 兼容报文 | `client/engine/direct/discovery.hpp:53` |
| 直连安全信道 | Direct secure channel | Ed25519 设备身份 + X25519/ECDH/HKDF + AEAD；TOFU 定针且无明文回退 | `client/engine/direct/secure_channel.hpp:63` |
| 设备身份 | Device identity | 直连态本端 Ed25519 密钥；首次从 LocalStore 生成，后续复用 | `client/engine/direct/secure_channel.hpp:45` |
| 客户端协作引擎 | Client collab engine | 登录协作服务端长连接：心跳、重连、离线补投、消息路由和断线补传 | `client/engine/collab/collab_engine.hpp:20` |
| 文件客户端 | Files client | FileServer HTTP API 封装；会话令牌内存持有，字节仍经服务端 | `client/engine/collab/files_client.hpp:20` |
| 客户端本地存储 | Client local store | 本地 SQLite：消息、会话、文件元数据、身份种子和同步状态 | `client/core/local_store.hpp:72`, `client/core/local_store.cpp:21` |
| 本地存储抽象 | Storage interface | 直连引擎可调用的本地存储接口；当前主实现是 `StorageLocal` | `client/core/storage_interface.hpp:33`, `client/core/storage_local.hpp:12` |
| 任务 Provider SPI | Task provider SPI | 外部任务能力声明：跳转、只读或双向；注册表装配内置 Provider | `client/engine/task/task_provider.hpp:47`, `client/engine/task/task_provider.hpp:138` |
| 任务 Provider 存储 | Task provider store | 桌面任务源凭据的 AES-256-GCM 加密包，经 QSettings 落盘 | `client/app/task_provider_store.cpp:19`, `client/app/task_provider_store.cpp:91` |
| 配置输入（服务端） | Server config input | CLI 旗标优先，S3/LDAP 等敏感项可用 env 兜底 | `server/src/main.cpp:88`, `server/src/main.cpp:245` |
| 配置输入（管理台） | Admin config input | CLI 优先，`MEMEX_ADMIN_*`、文件和存储 URL 随后兜底 | `admin/src/admin_app.cpp:201`, `admin/src/admin_app.cpp:218` |
| 配置输入（客户端） | Client config input | `QSettings("memex", "collab")` 及平台配置文件中的主题、通知、网段和端口设置 | `client/app/collab_defaults.hpp:51`, `client/app/main_window.cpp:7128` |
| 移动协作客户端 | Mobile collab client | Android/iOS/鸿蒙仅登录后的协作态；各自实现传输、会话与 UI 桥 | `apps/android/app/src/main/java/com/memex/im/core/MemexClient.kt:51`, `apps/ios/Sources/MemexKit/MemexClient.swift:29`, `apps/harmony/core/client.ts:27` |
| 移动会话 | Mobile chat session | 平台长连接对象：分帧、发送 seq、ACK/KICK 分发和本地落库 | `apps/android/app/src/main/java/com/memex/im/core/ChatSession.kt:40`, `apps/ios/Sources/MemexKit/ChatSession.swift:62`, `apps/harmony/core/session.ts:63` |
| 移动消息存储 | Mobile chat store | 平台本地消息/会话接口；当前 App 工厂均为内存实现 | `apps/android/app/src/main/java/com/memex/im/core/ChatStore.kt:38`, `apps/ios/Sources/MemexKit/ChatStore.swift:57`, `apps/harmony/core/chat_store.ts:32` |
| 会话管理器 | Chat manager | 把长连接事件投递到 UI 线程并聚合会话数据的平台桥 | `apps/android/app/src/main/java/com/memex/im/core/ChatManager.kt:14`, `apps/ios/Sources/MemexApp/AppChatManager.swift:26` |
| 会话持有者 | Chat holder | Android 登录后移交的进程内存单例，供多个 Activity 共享 | `apps/android/app/src/main/java/com/memex/im/core/ChatHolder.kt:7` |
| 应用模型 | App model | iOS 应用状态机：按 ServerSettings/LoginState 重算入口并持有 AppChatManager | `apps/ios/Sources/MemexApp/AppModel.swift:6` |
| 应用状态 | App state | Harmony 应用装配：初始化态、内存消息表、通知裁决、登录态、会话和监听器 | `apps/harmony/entry/src/main/ets/common/AppState.ets:129` |
| 路由守卫 | Route guard | 移动端唯一启动流：未初始化向导、未登录登录页、齐备主界面 | `apps/android/app/src/main/java/com/memex/im/core/InitStore.kt:28`, `apps/ios/Sources/MemexKit/AppState.swift:12`, `apps/harmony/core/route.ts:29` |
| 服务地址 | Server address | 移动端解析/校验后的主机与端口；协作态默认 24360 | `apps/harmony/core/address.ts:22`, `apps/ios/Sources/MemexKit/ServerAddress.swift:12` |
| 服务器设置 | Server settings | iOS 初始化地址/端口偏好存取，底层为 UserDefaults | `apps/ios/Sources/MemexApp/ServerSettings.swift:7` |
| 移动初始化存储 | Mobile init store | 平台初始化地址落盘：SharedPreferences、UserDefaults 或 `@ohos.data.preferences` | `apps/android/app/src/main/java/com/memex/im/core/PrefsInitStore.kt:8`, `apps/ios/Sources/MemexApp/ServerSettings.swift:7`, `apps/harmony/entry/src/main/ets/common/AppState.ets:50` |
| 序号台账 | Sequence ledger | 移动端按账号持久化本地发送序号，重登续位并取 max 合并 | `apps/android/app/src/main/java/com/memex/im/core/SeqLedger.kt:29`, `apps/ios/Sources/MemexApp/AppChatManager.swift:6`, `apps/harmony/entry/src/main/ets/common/AppState.ets:95` |
