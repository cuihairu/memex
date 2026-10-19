# 深度源码审核第一批（A0）执行记录

日期：2026-10-19
基线：`main`，同步后 HEAD `e394d57`（`origin/main` 同步；`git fetch origin`、`git pull --rebase origin main` 均成功，pull 输出 `Already up to date.`）
工作目录：`/home/cui/workspaces/memex`

## 范围与计数

`git ls-files` 清单共 654 个受版本控制文件。按源码/测试类型初步分区：common 5/1、client 157/51、server 56/29、admin 5/1、Android 36/10、iOS 28/8、Harmony 38/12（每组依次为源码文件数/测试文件数；文件类型口径见脚本记录）。其余包括 169 个 docs 文件、构建/CI、脚本、资源与清单。完整路径清单由 `git ls-files` 生成；审计矩阵按真实模块而非关键词命中划分。

本文件当前有 **45 个未勾选框**：37 个独立审计/决策项（A0 8、P0 4、P1 9、P2 10、D 6）及第 5 节 8 个执行顺序框。这里的“45”是框数，不是 45 个独立工作项。此次只对能以证据完整完成的执行规则更新状态；P0/P1/P2 的实现、负向测试和故障注入不因本记录而关闭。

## 审计矩阵（本批初查，不等于所有风险项已验收）

| 模块 | 入口与关键调用链（本批所查） | 已识别风险/关联项 | 已有测试证据 | 平台状态 |
|---|---|---|---|---|
| 公共协议 | `FrameDecoder::feed` → 长度解析 → payload 输出（`common/src/frame.cpp:39`） | 追加输入前/累计缓冲上限仍需专项验证（P1-4） | CTest `protocol` | Linux 本机可运行；其他平台未在本批执行 |
| 协作客户端 | TCP connected → `send_login_frame`；入站帧 → store append → ACK（`client/engine/collab/collab_engine.cpp:52,534,904-912`） | TLS、append 失败 ACK、服务端受理语义（P0-1/2/3） | `collab_login`、`collab_chat`、`collab_resend`、`msg_faults` | Linux 源码/测试可审；真机/跨平台未验证 |
| 协作服务端 | accept → session frame handler → archive/queue/online delivery/ACK（`server/src/server.cpp:146`、`server/src/session.cpp:275-299`） | `queue_offline` 失败传播与整体事务语义需故障注入（P0-3、P1-7） | `msg_dedupe`、`msg_faults`、`group`、`authz_audit` | Linux 本机可运行；生产部署反代边界未验证 |
| 直连与文件 | discovery → `SecureChannel` 握手/TOFU；`FileTransfer` sanitize → 路径 → part → rename（`client/engine/direct/secure_channel.cpp:208`、`file_transfer.cpp:344-407,532-545`） | 首次信任交互、路径逃逸、旧文件提前删除（P1-1/2、P0-4） | `discovery`、`direct_secure`、`direct_file` | Linux 本机可运行；Windows/macOS 文件语义未验证 |
| 存储与文件 API | session/authz → metadata/store/storage → object provider | DB/对象/配额一致性、协议解析和持久化错误路径（P1-7、P2-1/2/4） | `files_meta`、`files_api`、`uplink`、`s3_e2e`、`files_verify` | Linux 测试可运行；外部 S3 故障场景未在本批跑 |
| Admin | HTTP route → session/authz/CSRF → store/audit（`admin/src/admin_app.cpp:146,291,309`） | 固定自定义头仅为轻量 CSRF 闸；Cookie/部署全链路待专项验证（P1-6） | `admin_api` | 本机 Linux；公网 HTTPS 部署未验证 |
| 任务 Provider | UI/配置 → provider SPI → HTTP provider / credential store | 能力声明、URL 校验、token 生命周期和错误重试待专项审计（P2-9） | `task_provider*`、`github_provider`、`dingtalk_provider`、`feishu_provider` | 本机 Linux；外部真实服务未验证 |
| Android | Activity → ChatManager/ChatSession → protocol/store/socket | debug/release CI 缺口属于队列项 P1-8 | 10 个 JVM 测试文件；命令待本批执行 | JDK、Android SDK/adb 可用；本批不等于模拟器/真机验收 |
| iOS | SwiftUI/AppModel → MemexClient/ChatSession → wire/store/socket | 网络与存储链路需纳入跨平台门禁（P1-9） | 8 个 XCTest 文件 | Swift 工具链可用；设备走查状态未确认 |
| HarmonyOS | ArkTS 页面 → AppState/ChatSession → core transport/wire | ArkTS 壳编译与真机仍须遵守后置/待验证红线（A0-5、P1-9、D6） | Node/core 测试与现有 CI 文档 | 本机未发现 OHOS SDK、`hvigor`、`ohpm`；不声称壳编译或真机通过 |
| 构建、CI、文档 | CMake presets、GitHub Actions、安装/容器清单、产品口径文档 | 平台门禁、安装供应链、依赖与发布物对账待查（P1-8/9、P2-10） | `.github/workflows/ci.yml` 的 CMake/CTest；其他平台 workflow 单独存在 | Linux runner 配置可审；Windows/macOS/UOS/麒麟/OHOS 环境未在本机提供 |

## 可复核证据样例

1. `client/engine/collab/collab_engine.cpp:52` 将 `QTcpSocket::connected` 接到 `send_login_frame()`；`client/engine/collab/collab_engine.cpp:534` 是登录帧发送函数，当前链路应纳入 P0-1 TLS 核验。
2. `client/engine/collab/collab_engine.cpp:904` 入站消息调用 `store_->append(sm, &inserted)`；`client/engine/collab/collab_engine.cpp:906-913` 随后发送 ACK。调用结果是否约束 ACK 是 P0-2 的待修/待故障注入点，不能凭构建通过关闭。
3. `server/src/session.cpp:275-299` 展示服务端归档、离线队列、在线投递及 ACK 的先后顺序；`server/src/store.cpp:1631` 定义 `queue_offline`。调用方失败传播和事务边界列入 P0-3/P1-7。
4. `client/engine/direct/file_transfer.cpp:387` 在冲突内容下删除已有最终文件；`file_transfer.cpp:532-545` 完成时再次先移除目标再 rename part，符合 P0-4 描述的风险，需修复与失败场景测试。
5. `client/engine/direct/secure_channel.cpp:208` 定义 `verify_pin`；首次 pin 写入和用户确认流程属于 P1-1，当前抽查未发现足以关闭该项的用户确认链路。
6. `admin/src/admin_app.cpp:146` 的 `csrf_ok` 与 route 检查点 `291,309` 可定位；Cookie SameSite 设置见 `admin/src/admin_app.cpp:3941`。这些点不覆盖整项 P1-6 验收。

以上样例是源码定位和审计边界证据，不是缺陷复现或修复验收。没有把编译、CI 文件存在或测试名称当成运行通过。

## 任务状态复核与本批结论

| 项目 | 状态 | 结论 |
|---|---|---|
| A0.1 | 进行中 | 已生成并按模块统计 654 个 tracked 文件，完成构建/CI/设计与现有任务记录入口盘点；尚未逐个阅读全部 tracked 源码/测试，故不勾选。 |
| A0.2 | 进行中 | 已抽查 P0-1/2/3/4、P1-1/4/6 的关键当前链路；未完成 45 个框逐项的历史、测试和全调用链核对，故不勾选。 |
| A0.3 | 未完成 | 需逐个业务入口覆盖拒绝、超时、断线、重试、重启及磁盘/数据库失败；本批仅建立入口/调用链矩阵与风险映射。 |
| A0.4 | 完成 | 本报告每项记录符号/文件/行、测试证据与平台边界；未执行的验证标为未验证，不把静态证据写成运行验收。 |
| A0.5 | 完成 | 本机平台工具可用性已检查；OHOS 工具链及 Windows/macOS/UOS/麒麟环境未提供，报告明确保留待验证状态。 |
| A0.6 | 完成 | 本批只做取证与记录，不改安全检查、不降低测试；确认的 P0/P1 风险留在原审计队列，不重复造轮子。 |
| A0.7 | 完成 | 对照根 `todo.md` 保留服务端归档留痕、撤回留原文、清缓存不删云归档、桌面直连不经服务端、移动端初始化登录、Harmony 后置待真机等既定边界。 |
| A0.8 | 完成（本批矩阵） | 已提交模块矩阵、发现与未覆盖范围，并在根 `todo.md` 增加本次审计记录索引；完整源码逐项审计尚未完成。 |

其余 P0/P1/P2 与 D 项均保持未关闭。独立条目中 P0-1/2/3/4 与 P1-1/4/6 在当前源码中仍有需解决/验证的风险迹象；不能只因本次定位就视为已复现全部影响或完成验收。P1-5 及其他条目须结合完整调用链、历史和测试后再分类，避免复述旧 TODO 或重复开发。

## 环境与复跑命令

- `pwd` → `/home/cui/workspaces/memex`
- 清点：`git ls-files`；文件类型和模块计数用 Python 对清单分组。
- C++ 构建配置：`build-ci/CMakeCache.txt` 显示 Ninja、Debug、`MEMEX_BUILD_CLIENT=ON`、`MEMEX_BUILD_SERVER=ON`、`MEMEX_BUILD_TESTS=ON`；`ctest --test-dir build-ci -N` 列出 84 项。直接 `cmake --build build-ci` 因 `CMakeFiles/rules.ninja` 缺失退出 1。`cmake --preset ci` 首次因未设置 `VCPKG_ROOT` 退出 1；使用已存在的 `/home/cui/vcpkg` 重试后进入 vcpkg 依赖安装（执行超过 12 分钟，仍在编译 protobuf，未到项目 configure/build 阶段）。
- 旧 `build-ci` 二进制直接跑 `ctest --test-dir build-ci --output-on-failure -j 4`：84 项中 3 通过（`search_fuzzy`、`model_gateway`、`authz`），81 项无法启动，主要错误为缺 `libprotobufd.so.33.4.0` 或系统 Qt 低于构建要求的 Qt 6.11。该结果不计为 C++ 通过；需依赖配置完成后重新构建并重跑。
- Android：`cd apps/android && ./gradlew test` 成功，debug 95/95、release 95/95，failures/errors/skips 均为 0（Gradle 输出 `BUILD SUCCESSFUL`）。Harmony core：`cd apps/harmony && npm test` 成功，89/89，fail 0、skipped 0。文档：`cd docs && ./node_modules/.bin/vitepress build src` 成功，VitePress 1.6.4，build complete in 7.79s。
- iOS：`cd apps/ios && swift test` 在 Linux 主机退出失败；编译出现生成类型 `Memex_Protocol_V1_Envelope` 不可见及 Linux FoundationNetworking/`URLSession` 错误。此主机不能代表 iOS/macOS CI；iOS 平台测试状态仍为未验证。本次运行未将这些失败改写为通过。
