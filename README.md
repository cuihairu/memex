<p align="center">
  <img src="docs/src/public/logo.svg" alt="Memex" width="96" height="96" />
</p>

<h1 align="center">Memex · 内网办公即时通讯系统</h1>

<p align="center">
  <a href="https://github.com/cuihairu/memex/actions/workflows/ci.yml"><img src="https://github.com/cuihairu/memex/actions/workflows/ci.yml/badge.svg" alt="CI" /></a>
  <a href="https://codecov.io/gh/cuihairu/memex"><img src="https://codecov.io/gh/cuihairu/memex/graph/badge.svg" alt="Codecov 覆盖率" /></a>
  <a href="https://cuihairu.github.io/memex/"><img src="https://img.shields.io/website?url=https%3A%2F%2Fcuihairu.github.io%2Fmemex%2F&up_message=%E5%9C%A8%E7%BA%BF&down_message=%E7%A6%BB%E7%BA%BF&label=%E6%96%87%E6%A1%A3%E7%AB%99&color=e16531" alt="文档站" /></a>
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%E5%9B%BD%E4%BA%A7%20UOS%2F%E9%BA%92%E9%BA%9F%20%7C%20macOS%20%7C%20Android%20%7C%20iOS-e16531" alt="平台支持" />
  <img src="https://img.shields.io/badge/C%2B%2B20-Qt%206-e16531" alt="C++20 / Qt 6" />
  <a href="./LICENSE"><img src="https://img.shields.io/badge/License-Apache_2.0-blue.svg" alt="License: Apache-2.0" /></a>
</p>

内网物理隔离环境下的办公即时通讯系统，参照企业微信的功能范围，覆盖沟通、办公、组织与归档检索能力。业务层全部自写，底座采用开源组件：Qt 6、asio、protobuf、SQLite、OpenSSL、AWS SDK（S3 接口，对接 RustFS），清单与许可见 [third_party/](third_party/README.md)；不基于任何开源 IM 二次开发，不集成 AGPL／SSPL 等强传染性许可组件。桌面客户端 C++20 + Qt；手机端 Android（Kotlin，已落地）／iOS（Swift，代码-only＋CI 验证）；服务端 C++20 单进程单库。

- 产品定义与分期依据：[docs/建设方案评审报告-V10.0.md](docs/建设方案评审报告-V10.0.md)
- 产品定位一页纸：[docs/src/guide/product.md](docs/src/guide/product.md)
- 需求与任务清单（R1–R19 主线、R23–R27 追加批次、四期拆解、验收口径）：[todo.md](todo.md)
- 评审组决策清单（十五项 + 一项可选）：[docs/src/guide/decisions.md](docs/src/guide/decisions.md)
- 文档站（界面预览、快速预览、架构与合规）：`docs/`（VitePress，构建 `pnpm docs:build`，部署于 GitHub Pages `/memex/`）
- 原型设计稿：`docs/design/prototypes/`（桌面 7 屏 + 移动 4 屏），截图 `docs/src/public/screenshots/`

## 产品形态

单一安装包内含两套通信引擎：

| 形态 | 进入方式 | 链路 | 归档 |
| --- | --- | --- | --- |
| 直连态（默认） | 安装即用，零配置 | UDP 广播发现同网段终端，TCP 点对点直达（UDP 2425–2436 / TCP 2426–2437） | 仅存本机 |
| 协作态（登录后） | 填服务器地址并登录 | 与协作服务端长连接 | 全量落服务端，可检索 |

登录后直连引擎不关闭（跨态互通需要）；服务端不可达时自动回落直连态。**连接服务端的唯一目的是留痕**：不提供密聊、阅后即焚、撤回即删归档、终端侧单方删除归档、面向使用者的留痕开关。

手机端全部为协作态：首次启动必须先完成服务器地址初始化，不提供免登录匿名使用。

## Monorepo 结构

```
memex/
├── client/               # Qt C++20 桌面客户端（Windows / UOS / 麒麟 / macOS）
│   ├── app/              # 入口与主窗口、系统集成（托盘、通知、开机启动）、
│   │                     #   截图标注、主题、通知偏好（界面层并入 app）
│   ├── engine/direct/    # 直连引擎：UDP 发现、点对点消息与文件
│   ├── engine/collab/    # 协作引擎：长连接、消息同步、离线与补传
│   ├── core/             # 共享内核：本地库（SQLite）、会话、文件传输
│   └── tests/            # 桌面端单测
├── server/               # C++20 协作服务端（单台集中部署）
│   └── src/              # 单进程单库实现评审报告六模块职能（网关／账号设备／
│                         #   组织架构／消息归档／策略／文件元数据）：
│                         #   server.cpp session.cpp（网关与消息）
│                         #   store.cpp authz.cpp cred.cpp（账号/组织/策略/归档）
│                         #   files_server.cpp storage.cpp（R23 文件面与 S3 存储抽象）
│                         #   webhook.cpp（T4.10 通知接入）
├── common/               # 双端共用：协议定义（common/proto/memex.proto）、
│                         #   序列化、公共工具
├── apps/                 # 手机端客户端
│   ├── android/          # Android（Kotlin，T6.3 已落地：R17/R18 + 会话列表收发
│   │                     #   + 三级推送横幅震动）
│   ├── ios/              # iOS（Swift/SwiftUI，T6.4 代码-only＋CI 验证）
│   ├── harmony/          # 鸿蒙 NEXT（ArkTS，T6.6 代码-only＋CI 逻辑验证，
│   │                     #   编译腿待 OHOS 工具链）
├── admin/                # 管理后台（规划；背面见 server CLI/HTTP，README 占位）
├── docs/                 # 文档站（VitePress）、评审报告、产品文档、原型
└── third_party/          # 第三方组件清单与许可核验（A22；依赖经 vcpkg manifest 管理）
```

模块边界对应评审报告第四章：两套引擎各自完整、不共享通信状态，只通过共享内核（本地消息库、会话列表、联系人视图、文件传输模块）交汇；服务端六模块以单进程形式集聚（评审决策第 7 项：单进程单库实现，预留逻辑拆分），数据模型见报告第五章。

## 构建

依赖（asio / protobuf / nlohmann-json / sqlite3 / openssl / aws-sdk-cpp:s3 / Qt6）经 vcpkg manifest 统一管理（`vcpkg.json`，版本由 `builtin-baseline` 钉住），CMake 经 `CMakePresets.json` 一条命令构建：

```bash
# 1. 准备 vcpkg（一次性；建议把 VCPKG_ROOT 写入 shell profile）
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT="$HOME/vcpkg"

# 2. 开发构建（Release · 服务端+客户端+测试；首次配置自动按 manifest 装包）
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Preset 一览：`dev`（开发构建）、`ci`（Debug+覆盖率，CI 门禁）、`server-release` / `client-release`（每日构建产物口径）。默认只装服务端依赖，带 Qt 桌面的构建启用 manifest 的 `client` feature。首次配置会编译 qtbase，耗时较长；vcpkg 二进制缓存（默认 `~/.cache/vcpkg/archives`）在后续配置中复用编译产物，秒级完成。CI 与每日构建使用 GitHub Actions 缓存（`x-gha` 二进制缓存 + 安装树整取）加速。

## 工程纪律

全量测试绿才允许 commit／push，push 前先 `git fetch origin && git rebase origin/main`；每个可验收增量一笔提交，不打 tag、不发 release、不 force push。许可上不引入 AGPL／SSPL 组件，宽松许可组件登记进 `third_party/` 清单。命名按报告第五章第六节：代号 Memex，服务名 MemexServer，数据库与日志前缀 `memex`。
