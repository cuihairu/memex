# Memex 第三方组件登记（持续事项：引入任何第三方组件即登记名称、版本、
# 许可证、引入方式）。许可口径：以下均为宽松许可或同类义务明确的组件；
# 无 AGPL/SSPL **链接**进 memex 二进制（OnlyOffice 以独立容器进程运行，
# 进程边界即许可边界，见 AGPL 合规条）。
#
# 格式：名称 | 版本（核验日期）| 许可证 | 引入方式 | 用途 | 备注

## 构建依赖（vcpkg 清单见 vcpkg.json，基线 builtin-baseline 锁版本）

| 名称 | 版本 | 许可证 | 引入方式 | 用途 |
|---|---|---|---|---|
| asio | 1.32.0 | BSL-1.0 | vcpkg（header-only） | 协作服务端网络 I/O |
| nlohmann-json | 3.12.0 | MIT | vcpkg | 组织架构／偏好等 JSON 编解码 |
| protobuf（含 protoc） | 6.33.4 | BSD-3-Clause | vcpkg（运行时＋生成工具） | 线路协议唯一事实源 memex.proto |
| sqlite3 | 3.53.4 | Public Domain | vcpkg | 服务端归档库 |
| openssl（仅 libcrypto） | 3.6.4 | Apache-2.0 | vcpkg | 口令 PBKDF2／指纹 SHA-256 |
| aws-sdk-cpp（s3 feature） | 1.11.880 | Apache-2.0 | vcpkg（server feature） | R23-1 存储抽象层 S3 兼容客户端（服务端侧 find_package 消费） |
| qtbase（含 Widgets/Sql/Network，xcb/glib/xkb 插件） | 6.11.1 | LGPL-3.0（动态链接，未修改） | vcpkg client feature | 桌面客户端 |
| crashpad | 2026-07-02（date） | Apache-2.0 | vcpkg crashpad feature | 桌面客户端崩溃采集（dump 仅本地落盘，win/mac preset 启用） |
| vcpkg 本体 | 基线见 vcpkg.json（builtin-baseline 锁版本） | MIT | 构建工具 | 依赖管理 |

## 移动端／鸿蒙／文档站（非 vcpkg 面；版本以各 manifest 与 lockfile 实读为准，2026-10-10 核验）

### 运行时随产物链接

| 名称 | 版本 | 许可证 | 引入方式 | 用途 |
|---|---|---|---|---|
| androidx.appcompat | 1.7.0 | Apache-2.0 | Google Maven（implementation，随 APK 链接） | Android 兼容 UI 组件（apps/android） |
| protobuf-javalite | 4.31.1 | BSD-3-Clause | Google Maven（implementation，随 APK 链接；与 protoc 同版本） | memex.proto 协议运行时（lite 变体适配 Android） |
| swift-protobuf | 1.38.1 | Apache-2.0 | SwiftPM（Package.swift 声明 from 1.26.0；仓库未提交 Package.resolved，2026-10-09 CI 实解析 1.38.1；随 app 链接） | memex.proto 协议运行时（apps/ios） |

### 构建期工具（不随交付物分发）

| 名称 | 版本 | 许可证 | 引入方式 | 用途 |
|---|---|---|---|---|
| protoc（Android 侧） | 4.31.1 | BSD-3-Clause | protobuf-gradle-plugin 拉取（构建期生成 Java lite 码） | memex.proto → Java 生成码（与 C++ 侧 protoc 同族，版本独立钉） |
| protobuf-gradle-plugin | 0.9.4 | BSD-3-Clause | Gradle 插件（plugins，settings.gradle.kts 钉版） | 驱动 protoc 代码生成 |
| Gradle | 8.14.3 | Apache-2.0 | wrapper 钉版（gradle-wrapper.properties） | Android 构建工具链 |
| Android Gradle Plugin（AGP） | 8.13.1 | Apache-2.0 | Gradle 插件（plugins，settings.gradle.kts 钉版） | Android 应用构建 |
| Kotlin | 2.2.21 | Apache-2.0 | Gradle 插件（plugins，settings.gradle.kts 钉版） | Android 客户端语言工具链 |
| protoc-gen-swift | 随 brew（CI 不钉版） | Apache-2.0 | CI `brew install swift-protobuf`（构建期生成 Swift 码） | memex.proto → Swift 生成码（apps/ios） |
| XcodeGen | 随 brew（CI 不钉版） | MIT | CI `brew install xcodegen`（生成 Memex.xcodeproj） | iOS 工程组装（project.yml → .xcodeproj） |
| TypeScript（tsc） | 5.9.3 | Apache-2.0 | npm devDependency（声明 ^5.6.0，package-lock 实解析；构建期编译 core TS→JS，交付物为编译产物） | 鸿蒙逻辑层编译（apps/harmony） |
| VitePress | 1.6.4 | MIT | pnpm devDependency（声明 ^1.6.3，pnpm-lock 实解析；构建期生成静态站，交付物为构建产物） | 文档站框架（docs） |
| Vite | 6.4.3 | MIT | pnpm devDependency（声明 ^6.4.3，pnpm-lock 实解析；VitePress 底层打包器，pnpm overrides 钉版） | 文档站构建（docs） |

### 仅测试期（不随交付物分发）

| 名称 | 版本 | 许可证 | 引入方式 | 用途 |
|---|---|---|---|---|
| junit | 4.13.2 | EPL-1.0 | Maven（testImplementation） | Android JVM 单测框架 |
| org.json | 20240303 | Public Domain | Maven（testImplementation，顶掉 android.jar 占位桩） | JVM 单测 org.json 替身（主代码用 Android 内置实现） |
| protobufjs | 7.6.6 | BSD-3-Clause | npm devDependency（声明 ^7.4.0，package-lock 实解析；仅 test/wire.test.ts 引用） | 鸿蒙逻辑层协议字节级交叉验证（ArkTS 运行时用手写编解码，无第三方依赖） |
| @types/node | 22.20.5 | MIT | npm devDependency（声明 ^22.10.0，package-lock 实解析） | Node 类型定义（类型检查／单测） |

## 音视频／协同文档底座（独立进程部署，保持进程边界）

| 名称 | 版本 | 许可证 | 引入方式 | 用途 |
|---|---|---|---|---|
| livekit-server | v1.8（docker image） | Apache-2.0 | 容器（scripts/deploy/meet-compose.yml） | 音视频会议 SFU＋内嵌 TURN |
| onlyoffice-documentserver | 8.2（docker image） | **AGPL-3.0** | 容器（scripts/deploy/docs-compose.yml） | 协同文档编辑 |
| collabora/code | —（备选，未部署） | MPL-2.0 | — | OnlyOffice 替代方案（若 AGPL 流程成本过高） |
| jitsi（jvb＋jicofo＋meet） | —（备选，未部署） | Apache-2.0 | — | LiveKit 替代方案（SFU 选型备份） |

### AGPL 合规条（OnlyOffice）
1. **未修改**：使用官方 `onlyoffice/documentserver:8.2` 镜像原样运行，
   不打补丁、不二次分发修改版——AGPL 第 13 条的网络交互源码提供义务
   以"未修改官方发行版"为前提履行：部署文档必须给出上游源码地址
   （https://github.com/ONLYOFFICE/DocumentServer）与 exact 镜像 digest。
2. **进程边界**：memex 二进制不链接、不嵌入 OnlyOffice 任何代码；
   仅经 HTTP API（编辑器回调）交互——AGPL 传染止于容器边界。
3. **客户交付**：随交付物提供本登记表＋镜像 digest 获取方式
   （`docker inspect`），满足 AGPL 第 4-6 条 notices 义务。
4. 若法务判定 AGPL 流程不可接受，切换备选 Collabora（MPL-2.0，
   文件级 copyleft，无网络条款），compose 模板届时补入。

## 构建期工具（不进交付物）

| 名称 | 用途 |
|---|---|
| ImageMagick `convert` | client/resources/logo.ico 由正典 logo.svg 机械渲染（见 qrc 注释），仅构建期使用 |
| protoc（随 protobuf） | memex.proto → C++ 生成码（构建树内，不入库） |

## 代码来源声明
client/、server/、common/ 下为本项目自写代码——业务层不含任何开源 IM 的二开代码，
底座依赖即上表开源组件；单测断言为手写宏（无 gtest/doctest 等单测框架依赖）。
client/server/common 面除 vcpkg 已登记组件外**无**其他第三方库（find_package 全部落在
vcpkg 依赖内，2026-10-10 核）；移动端／iOS／鸿蒙／文档站第三方依赖见上节（非 vcpkg 面）。
