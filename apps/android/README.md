# Memex Android 客户端（T6.3）

内网办公 IM 的 Android 端，Kotlin 从零起步。当前已落地：**登录块**——
手机端初始化向导（R17）＋登录（R18：移动端全部为协作态，必须登录后使用）。

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
      core/   # 纯 Kotlin：地址解析、帧编解码、路由守卫、探测/登录客户端
      ui/     # InitActivity / LoginActivity / MainActivity（launcher+守卫）
    src/test/ # JVM 单测（含假服务端的探测/登录全链路）
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

## 登录块之后的排期

会话列表、单聊/群聊收发、三级通知（横幅/强提醒/需确认）随 T6.3 后续块落地。
