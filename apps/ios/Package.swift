// swift-tools-version:5.9
// Memex iOS 客户端（T6.4：代码-only＋CI 验证）。
// 核心逻辑（core 面）为 SwiftPM 包 MemexKit，纯 Foundation＋SwiftProtobuf；
// SwiftUI App 壳在 Sources/MemexApp，由 project.yml（XcodeGen）组装成
// iOS 应用 target 并在 CI 的 macOS runner 上 xcodebuild 验证编译。
import PackageDescription

let package = Package(
    name: "MemexKit",
    platforms: [
        .iOS(.v16),
        .macOS(.v13), // 单测腿与 Future Linux 预检的余量
    ],
    products: [
        .library(name: "MemexKit", targets: ["MemexKit"]),
    ],
    dependencies: [
        // 协议生成代码的运行时（proto 单一事实源 common/proto/memex.proto）
        .package(url: "https://github.com/apple/swift-protobuf.git", from: "1.26.0"),
    ],
    targets: [
        .target(
            name: "MemexKit",
            dependencies: [
                .product(name: "SwiftProtobuf", package: "swift-protobuf"),
            ],
            path: "Sources/MemexKit"
        ),
        .testTarget(
            name: "MemexKitTests",
            dependencies: ["MemexKit"],
            path: "Tests/MemexKitTests"
        ),
    ]
)