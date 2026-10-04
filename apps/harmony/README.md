# Memex 鸿蒙手机版（HarmonyOS NEXT / ArkTS）

阶段 6「其他手机端」的鸿蒙客户端（T6.6 口径：代码-only ＋ CI 逻辑验证；
排序在 Android、iOS 之后）。

## 结构

```
apps/harmony/
├── core/                  纯 TS 逻辑层（har 模块 @memex/core，Node 可测）
│   ├── wire.ts            protobuf wire 手写编解码（与 protobufjs 交叉验证）
│   ├── frame_codec.ts     帧编解码（镜像 client/core frame.cpp，字节级一致）
│   ├── address.ts         服务器地址解析（镜像 apps/android ServerAddress）
│   ├── chat_store.ts      本地会话存储（内存实现）
│   ├── route.ts           R17/R18 路由门禁（RouteGuard/InitGate）
│   ├── client.ts          probe/login（R17 向导前置门、R18 登录）
│   ├── session.ts         长连接会话（收发/ACK/去重/三级通知/KICK）
│   └── wire_channel.ts    帧通道（Transport + FrameDecoder + wire）
├── entry/                 HAP 壳（ArkTS stage 模型）
│   └── src/main/ets/
│       ├── common/        HarmonyTransport(@ohos.net.socket)/AppState/
│       │                  NoticeManager(三级通知)/DeviceIdentity(设备指纹)
│       └── pages/         Index(守卫)/InitPage(R17)/LoginPage(R18)/
│                          MainPage(会话列表)/ChatPage(收发)
├── test/                  Node 单测（node:test，ubuntu CI 全绿）
└── AppScope/              工程级应用配置
```

## 功能面（对齐 T6.3 / T6.4）

- **R17 初始化向导**：首次启动强制设服务器地址，`MemexClient.probe`
  （PING→PONG）连通性校验通过才落盘放行；重设必须重新通过校验。
- **R18 无匿名入口**：唯一入口 Index 守卫页（RouteGuard），未初始化→向导、
  未登录→登录页；进程重启后须重新登录（初始化态持久化）。
- **登录**：设备指纹（稳定种子推导）＋单点互踢（KICK→清登录态回登录页，
  replacedBy 展示对方设备名）。
- **会话列表**：未读角标、最近消息预览、时间倒序；新建会话（账号或
  `group:N`）。
- **单聊/群聊收发**：线格式与桌面端字节级一致；自己发的消息本地立即落库
  （受理回执 ACK(seq)→onSent）；收方 ACK(msg_id)＋msg_id 去重；群消息按
  `to=group:N` 归会话。
- **三级通知**（对齐桌面 T4.10 与 Android T6.3）：
  普通＝仅站内会话消息不弹；重要＝横幅＋震动强提醒（不看前后台）；
  紧急＝前台应用内确认弹窗（未确认的排队弹出）/后台常驻横幅（isOngoing
  不可滑除）＋震动，确认收悉后才消失。
- **移动端适配**：气泡左右分侧、进入自动滚底、进入即清未读、长按复制。

## 验证口径（如实声明）

**本机与 CI 均无 OHOS 工具链（hvigor / hdc / ohpm NOT FOUND），不做本地
构建。** 已验证与未验证的边界：

| 层 | 验证方式 | 状态 |
|---|---|---|
| core 纯逻辑（wire/frame/地址/存储/路由/会话全链路） | `npm test`：73 用例，ubuntu CI（`.github/workflows/harmony.yml`） | ✅ 全绿 |
| 线格式字节级一致 | wire 与 protobufjs（加载 `common/proto/memex.proto` 单一事实源）**双向字节交叉**；帧前缀镜像 `client/core frame.cpp` | ✅ 全绿 |
| ArkTS 壳编译（hvigor → HAP） | 待 OHOS runner 与 SDK 可用后补编译腿 | ⛔ 未验证 |

不造假绿：CI 只跑 Node 逻辑层，ArkTS 编译腿在 CI 注释与本表中如实标注
「待 OHOS runner」，工程级/模块级 hvigor 配置与 ohpm 本地依赖（entry →
`@memex/core: file:../core`）已就位，SDK 可用后接 DevEco/hvigor 即可。

本地跑法：

```bash
cd apps/harmony
npm install
npm test   # tsc strict 构建 + node --test 全量单测
```

## 线格式单一事实源

协议唯一定义在 `common/proto/memex.proto`。`core/wire.ts` 是其手写
protobuf 编解码（ArkTS 无第三方运行时依赖），正确性由测试与 protobufjs
双向字节对照背书——不另造线格式；帧 = 4 字节大端长度前缀 + Envelope
（同 `client/core frame.cpp`）。
