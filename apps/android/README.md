# Memex Android 客户端（T6.3）

内网办公 IM 的 Android 端（Kotlin 实现）。**T6.3 三块已全部落地**：

1. **工程骨架＋登录块**——初始化向导（R17）＋登录（R18：移动端全部为协作态，必须登录后使用）；
2. **会话列表与收发**——长连接会话（单 TCP 读线程、ACK 回执、msg_id 去重、KICK 互踢）、
   会话列表（未读角标／发起会话／退出登录）、聊天页（气泡／撤回置灰／发送／自动滚底）；
3. **消息推送横幅＋震动＋移动端适配**——三级通知语义（普通＝仅站内、重要＝横幅＋声音＋震动、
   紧急＝常驻需「确认收悉」）、通知渠道、Android 13+ POST_NOTIFICATIONS 运行时申请、
   进会话清未读、长按复制消息。

单测 57/57 绿（debug 变体；release 变体在本机存在 2–3 处偶发红，时序敏感，
登记 BUGS.md BUG-005 只登记未修）。遗留：会话本地持久化（现内存实现，SQLite 随需要补）。

## 口径

- **R17 初始化向导**：首次启动强制进入服务器地址设置页（域名／IP，可带端口，
  缺省 24360），**连通性校验通过**（PING→PONG）才落盘放行去登录页；无跳过路径。
- **R18 无匿名入口**：唯一 launcher 是 `MainActivity`，onCreate 经 `RouteGuard`
  守卫——未初始化改道向导、未登录改道登录页；应用内不存在任何免登录/匿名形态
  （结构性校验见 `R18SurfaceTest`）。
- 直连态（UDP 广播发现、点对点）是**桌面端专属**能力，本端不做。

## 结构

```
apps/android/
  settings.gradle.kts / build.gradle.kts / gradle.properties
  app/
    src/main/java/com/memex/im/
      core/   # 纯 Kotlin：地址解析、帧编解码、路由守卫、探测/登录客户端、
              #   ChatStore/ChatSession/ChatManager/ChatHolder 长连接收发、
              #   NoticeGrade 三级通知分级、InitStore 配置持久化
      ui/     # InitActivity / LoginActivity / MainActivity（launcher+守卫）/
              #   ChatActivity / NotificationHelper / MemexNotifyReceiver
    src/test/ # JVM 单测（57 用例：假服务端探测/登录/收发/群归会话/ACK/去重/KICK/三级通知）
```

协议单一事实源＝仓库根 `common/proto/memex.proto`（构建时跨目录引入，protoc
生成 lite Java；与 C++ 服务端/桌面端线格式一致）。帧＝4 字节大端长度前缀 +
Envelope。

## 构建与测试

需要 JDK 17+、Android SDK（platform 36），`ANDROID_HOME` 指向 SDK：

```bash
cd apps/android
./gradlew test          # JVM 单测
./gradlew assembleDebug # 产出 app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

模拟器访问宿主机服务端：地址填 `10.0.2.2:24360`（宿主机侧跑
`build-server/server/memex_server serve`，建号
`memex_server account add <账号> <口令> --name <显示名>`）。

## 后续排期

会话本地持久化（现内存实现，SQLite 随需要补）；「我」屏（设备管理等，随需要启动）；
深色主题实况走查截图（补文档站手机端深色实况）。
