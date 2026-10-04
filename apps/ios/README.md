# Memex iOS 客户端（T6.4）

移动端全部为协作态（R17/R18）：唯一入口经守卫路由——初始化向导 → 登录 → 会话列表；
无免登录/匿名入口。

## 布局

```
apps/ios/
├── Package.swift              # SwiftPM 包 MemexKit（core 面，可测试）
├── project.yml                # XcodeGen 描述：App 壳 target（SwiftUI）
├── Sources/
│   ├── MemexKit/              # core：协议帧编解码、服务器地址、会话存储、
│   │   │                      #   长连接会话、探测/登录、路由守卫
│   │   └── Proto/             # memex.proto 生成代码（CI 生成，不入库）
│   └── MemexApp/              # SwiftUI：初始化向导/登录/会话列表/聊天页/推送
└── Tests/MemexKitTests/       # 核心单测（37 用例，真 socket 假服务端）
```

## 构建与测试（macOS）

```sh
# 1) 工具链
brew install protobuf swift-protobuf xcodegen

# 2) 协议生成（单一事实源 common/proto/memex.proto）
mkdir -p Sources/MemexKit/Proto
protoc --swift_opt=Visibility=Public \
  --swift_out=Sources/MemexKit/Proto ../../common/proto/memex.proto

# 3) 核心逻辑测试
swift test

# 4) App 编译
xcodegen generate
xcodebuild -project Memex.xcodeproj -scheme Memex \
  -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' \
  CODE_SIGNING_ALLOWED=NO build
```

CI（`.github/workflows/ios.yml`，macOS runner）执行 2–4 步。

## 消息推送（对齐桌面 T4.10 三级）

- normal 站内：仅列表与角标；
- important 重要：横幅＋声音（前台同样展示横幅）；
- urgent 紧急：横幅＋声音＋交互确认（category `MEMEX_URGENT`）。

APNs 设备令牌注册后经登录设备台账路径上报（T2.1/T3.3，代码路径已接、真机验证
待补——本机无 iOS 环境，验证腿走 CI／真机）；未配置 APNs 时
降级为应用内/本地通知横幅。