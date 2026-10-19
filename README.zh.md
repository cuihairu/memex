[English](README.md) | [中文](README.zh.md)

<p align="center">
  <img src="docs/src/public/logo.svg" alt="Memex" width="64" height="64" />
</p>

<h1 align="center">Memex · 内网办公即时通讯系统</h1>

<p align="center">
  <a href="https://github.com/cuihairu/memex/actions/workflows/ci.yml"><img src="https://github.com/cuihairu/memex/actions/workflows/ci.yml/badge.svg" alt="CI" /></a>
  <a href="https://codecov.io/gh/cuihairu/memex"><img src="https://codecov.io/gh/cuihairu/memex/graph/badge.svg" alt="Codecov 覆盖率" /></a>
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS%20%7C%20Android%20%7C%20iOS%20%7C%20HarmonyOS-e16531" alt="平台支持" />
  <img src="https://img.shields.io/badge/C%2B%2B17-Qt%206-e16531" alt="C++17 / Qt 6" />
  <a href="./LICENSE"><img src="https://img.shields.io/badge/License-Apache_2.0-blue.svg" alt="License: Apache-2.0" /></a>
</p>

内网物理隔离环境下的办公即时通讯系统，参照企业微信的功能范围，覆盖沟通、办公、组织与归档检索能力。业务层全部自写，底座采用开源组件：Qt 6、asio、protobuf、SQLite、OpenSSL、AWS SDK（S3 接口，对接 RustFS），清单与许可见 [third_party/](third_party/README.md)；不基于任何开源 IM 二次开发，不集成 AGPL／SSPL 等强传染性许可组件。桌面客户端 C++17 + Qt（Linux 走 CI，Windows／macOS 产物来自每日构建矩阵）；手机端 Android（Kotlin，已落地）／iOS（Swift，代码-only＋CI 验证）／鸿蒙 NEXT（ArkTS，代码-only＋CI 逻辑验证）；服务端 C++17 单进程单库。

## 文档

- 文档站（GitHub Pages）：<https://cuihairu.github.io/memex/> ——界面预览、快速预览、架构与合规；源码在 `docs/`（VitePress，构建 `pnpm docs:build`）
- 产品定位一页纸：<https://cuihairu.github.io/memex/guide/product> · 源 [docs/src/guide/product.md](docs/src/guide/product.md)
- 评审组决策清单（十五项 + 一项可选）：<https://cuihairu.github.io/memex/guide/decisions> · 源 [docs/src/guide/decisions.md](docs/src/guide/decisions.md)
- 界面预览（实况截图；桌面 8 屏 + 移动 4 屏）：<https://cuihairu.github.io/memex/guide/prototypes> · 原型源 [docs/design/prototypes/](docs/design/prototypes/)
- 产品定义与分期依据：[docs/建设方案评审报告-V10.0.md](docs/建设方案评审报告-V10.0.md)
- 需求与任务清单（R1–R19 主线、R23–R27 追加批次、四期拆解、验收口径）：[todo.md](todo.md)

## 产品形态

单一安装包内含两套通信引擎：

| 形态 | 进入方式 | 链路 | 归档 |
| --- | --- | --- | --- |
| 直连态（默认） | 安装即用，零配置 | UDP 广播发现同网段终端，TCP 点对点直达（UDP 实际仅用 2425、2426–2436 为保留段 / TCP 2426–2437） | 仅存本机 |
| 协作态（登录后） | 填服务器地址并登录 | 与协作服务端长连接 | 全量落服务端，可检索 |

登录后直连引擎不关闭（跨态互通需要）；服务端不可达时自动回落直连态。**连接服务端的唯一目的是留痕**：不提供密聊、阅后即焚、撤回即删归档、终端侧单方删除归档、面向使用者的留痕开关。

手机端全部为协作态：首次启动必须先完成服务器地址初始化，不提供免登录匿名使用。

## Monorepo 结构

```
memex/
├── client/               # Qt C++17 桌面客户端（Windows / Linux / macOS）
│   ├── app/              # 入口与主窗口、系统集成（托盘、通知、开机启动）、
│   │                     #   截图标注、主题、通知偏好（界面层并入 app）
│   ├── engine/direct/    # 直连引擎：UDP 发现、点对点消息与文件
│   ├── engine/collab/    # 协作引擎：长连接、消息同步、离线与补传
│   ├── engine/task/      # 外部任务 Provider（钉钉／飞书／GitHub），能力声明 SPI
│   │                     #   （L1 跳转／L2 只读／L3 双向；R27-2）
│   ├── core/             # 共享内核：本地库（SQLite）、会话、文件传输
│   └── tests/            # 桌面端单测
├── server/               # C++17 协作服务端（单台集中部署）
│   └── src/              # 单进程单库实现评审报告六模块职能（网关／账号设备／
│                         #   组织架构／消息归档／策略／文件元数据）：
│                         #   server.cpp session.cpp（网关与消息）
│                         #   store.cpp authz.cpp cred.cpp（账号/组织/策略/归档）
│                         #   files_server.cpp storage.cpp（R23 文件面与 S3 存储抽象）
│                         #   webhook.cpp（T4.10 通知接入）、main.cpp（入口与 CLI）、
│                         #   model_gateway.cpp（模型网关，三期）、ldap.cpp（企业身份）、
│                         #   group_assistant.cpp（群内智能助手）、task_pull.cpp
│                         #   （外部任务拉取）、agent_main.cpp（R26-1 负载心跳 agent）
├── common/               # 双端共用：协议定义（common/proto/memex.proto）、
│                         #   序列化、公共工具
├── apps/                 # 手机端客户端
│   ├── android/          # Android（Kotlin，T6.3 已落地：R17/R18 + 会话列表收发
│   │                     #   + 三级推送横幅震动；39/39 单测＋真机走查，无 CI 腿）
│   ├── ios/              # iOS（Swift/SwiftUI，T6.4 代码-only＋CI 验证）
│   ├── harmony/          # 鸿蒙 NEXT（ArkTS，T6.6 代码-only＋CI 逻辑验证，
│   │                     #   编译腿待 OHOS 工具链）
├── admin/                # 管理后台（Drogon 进程，8089；随 compose 起——见下文管理后台节）
├── docs/                 # 文档站（VitePress）、评审报告、产品文档、原型
├── scripts/              # 运维／演练脚本（穿通演练、备份恢复演练、nightly、
│                         #   deploy 助手、托盘走查）
├── packaging/            # 打包支持（Windows Inno 脚本、OHOS HAP 注记、LDAP 环境样例）
├── examples/             # 集成示例（echo bot）
├── install.sh            # 一键安装（取 nightly 资产、OS／架构检测、重跑幂等；
│                         #   可从 main 分支 curl 直装）
├── install.ps1           # install.sh 的 Windows 对应件
├── BUGS.md               # 缺陷台账（只登记不修原则、修复与验收留痕）
└── third_party/          # 第三方组件清单与许可核验（A22；依赖经 vcpkg manifest 管理）
```

模块边界对应评审报告第五章：两套引擎各自完整、不共享通信状态，只通过共享内核（本地消息库、会话列表、联系人视图、文件传输模块）交汇；服务端六模块以单进程形式集聚（评审决策第 7 项：单进程单库实现，预留逻辑拆分），数据模型见同章。

## 构建

依赖（asio / protobuf / nlohmann-json / sqlite3 / openssl / aws-sdk-cpp:s3 / drogon（管理后台）/ crashpad（客户端崩溃采集）/ Qt6）经 vcpkg manifest 统一管理（`vcpkg.json`，版本由 `builtin-baseline` 钉住），CMake 经 `CMakePresets.json` 一条命令构建：

```bash
# 1. 指向与 vcpkg.json builtin-baseline 匹配的 vcpkg checkout
export VCPKG_ROOT=/path/to/vcpkg
# 可选：明确使用本地文件二进制缓存；Linux 默认缓存通常已是 ~/.cache/vcpkg/archives
export VCPKG_BINARY_SOURCES="clear;files,$HOME/.cache/vcpkg/archives,readwrite"

# 2. 配置并构建（Release · 服务端+客户端+测试）
cmake --preset dev
cmake --build --preset dev

# 3. 运行与 CI 相同的测试门禁（Debug + coverage）
cmake --preset ci
cmake --build --preset ci
ctest --preset ci
```

Preset 一览：`dev`（开发构建）、`ci`（Debug+覆盖率，CI 门禁）、`server-release` / `client-release`（每日构建产物口径）、平台变体 `client-release-win` / `client-release-mac` / `client-release-ohos` / `server-release-mac`（另有 hidden `base` preset 提供共享 vcpkg 配置）。默认只装双端共享依赖（protobuf／nlohmann-json／openssl），服务端构建启用 manifest 的 `server` feature（asio／sqlite3／aws-sdk-cpp），带 Qt 桌面的构建启用 `client` feature。vcpkg 默认二进制缓存位于 Linux 的 `~/.cache/vcpkg/archives`；已有缓存命中时复用编译包，缺失项仍会编译或下载。离线构建要求 manifest 所需源码归档或二进制包已预热到本地缓存，首次配置前请确认缓存完整；CI 使用 GitHub Actions `x-gha` 二进制缓存和安装树缓存加速。

CI 命令对应 `.github/workflows/ci.yml` 的 Configure、Build、Test 步骤：`cmake --preset ci`、`cmake --build --preset ci`、`ctest --preset ci`。本地干净重建可在确认 `build/` 是生成目录后删除，再执行 `cmake --preset dev` 和 `cmake --build --preset dev`；测试门禁使用 CI preset，而不是旧的 `dev` 测试树。

## 本地测试口径

CI 与本地开发按改动面三路分流——改一行文档不该全量编译。

1. **文档改动**（`*.md`、`docs/**`、图片）：不编译。主 CI workflow 的
   `paths-ignore` 已跳过这些路径。
2. **代码改动**：只跑改动子集，不全量。
   - `ctest --preset ci -R '<改动测试>'`
3. **全量**（`ctest --preset ci`）：仅以下情形触发——CMakeLists.txt、
   `.github/**`、Docker/Compose 配置、发布前。

## 部署

### Docker（推荐）

多阶段构建（`server/Dockerfile`）：依赖层按 vcpkg manifest 静态编译（vcpkg.json 变更才失效）、构建层出单二进制、运行层 `ubuntu:24.04` 非 root（uid 10001）运行。依赖口径：asio／nlohmann-json 头文件即用；protobuf／openssl(libcrypto)／sqlite3／aws-sdk-cpp(s3) 静态链接进二进制——运行层不拖带依赖侧 `.so`（基础镜像自带系统库除外）。容器构建与宿主开发树（`x64-linux-dynamic`，Qt 客户端同树）互不相干。

vcpkg 源不走网络克隆：经 buildx named context 注入快照（须含 `vcpkg.json` builtin-baseline 对应 commit；干净 clone 一份、把该 commit checkout 出来即可，快照带 `downloads/` 缓存则依赖层全离线装）。CI 侧由 workflow 自动取快照，本地从源码构建需指定：

```bash
# 首次从源码构建镜像：注入 vcpkg 快照
git clone https://github.com/microsoft/vcpkg vcpkg-src
git -C vcpkg-src checkout 10541e317a660f4165ba4ac2851ab54a8d4577b1
docker buildx build --build-context vcpkgsrc=./vcpkg-src \
  -f server/Dockerfile -t ghcr.io/cuihairu/memex-server:local .

# 一键起：服务端（SQLite 持久卷）+ RustFS（S3 兼容对象存储）+ 管理后台（网页控制台）
# （compose 默认也走上述构建：VCPKG_SRC 环境变量改指快照，默认 ./vcpkg-src；
#  已有镜像后直接 up 不触发构建、无需快照）
docker compose up -d
docker compose ps        # 三服务 healthy 即就绪
# 客户端连接：<宿主机>:8088（协作面对外口，对齐 tunnel 回源 tcp://192.168.5.5:8088）
# 或 <宿主机>:24360（局域网缺省口兼容）、<宿主机>:24561（文件面）
# 模型网关：http://<宿主机>:24362（平台三期，默认关——须显式 --model-port N 才监听；
# 未登记上游端点时网关空转）
# 管理后台：http://<宿主机>:8089（见下文管理后台节）
```

对象存储口令生产环境写同目录 `.env`（`MEMEX_S3_ACCESS_KEY`／`MEMEX_S3_SECRET_KEY`）再起；示例口令仅限本机试跑。服务端 S3 配置旗标优先、环境变量兜底（`MEMEX_S3_ENDPOINT`／`MEMEX_S3_BUCKET`／`MEMEX_S3_ACCESS_KEY`／`MEMEX_S3_SECRET_KEY`），桶在起面时幂等自建（含对端就绪重试）。健康探针＝文件面无鉴权 `GET /files/health`（改文件面端口时同步设容器 `MEMEX_FILES_PORT`）。

镜像发布：推 `v*` tag 或推 main 由 CI（`.github/workflows/docker.yml`）构建并推 `ghcr.io/<owner>/memex-server` 与 `ghcr.io/<owner>/memex-admin`（main＝不可变 `sha-<完整 sha>` + latest；v* 另出 semver；两镜像并行出、按镜像分 buildx gha 缓存作用域互不驱逐）。

### 公网接入：Cloudflare tunnel（TCP 8088）

```
客户端（cloudflared access tcp）→ Cloudflare 边缘：server.memex.cuihairu.site
  → 用户侧 tunnel → 回源 tcp://<回源机>:8088 → 宿主 0.0.0.0:8088 → 容器 24360
```

映射关系配在用户 Cloudflare 侧（token 与 tunnel 配置不进仓、不代管）。仓里的契约只有一条：**协作面对外发布在宿主 8088**（compose 默认即此），且 tunnel 回源地址指向真正跑 `docker compose up` 的那台机——部署机与 tunnel 配置假设的机不一致时改回源地址，端口保持 8088。

- 原始 TCP tunnel 的客户端侧：公网侧裸 TCP 客户端连不到边缘——客户端机先跑 `cloudflared access tcp --hostname server.memex.cuihairu.site --url 127.0.0.1:8088`，再把 Memex 客户端指向 `127.0.0.1:8088`；局域网内不走 tunnel，直连 `<宿主机>:8088`。
- webhook（24361）：要对外暴露须另配 ingress（HTTP 域名映射，或同 tunnel 加一条 HTTP ingress）——**仓里不代管**；hook token 是持有式凭据，能只留局域网就别暴露公网。

状态自查与排查——从回源侧往外走：

```bash
# 1) 回源机真在服务？（先全绿再怀疑 tunnel）
docker compose ps                                   # server healthy
nc -zv <回源机> 8088                                # 协作面 accept
curl -sf http://<回源机>:24561/files/health         # 文件面健康 = 200

# 2) tunnel 状态（用户侧）：Cloudflare 控制台 → Zero Trust → Networks → Tunnels，
#    或在 cloudflared 所在机：
cloudflared tunnel list && cloudflared tunnel info <tunnel>

# 3) 客户端连不通？按序走：server.memex.cuihairu.site 解析得通？→ tunnel 状态
#    HEALTHY（connector 在线）？→ cloudflared 所在机能 nc 通 <回源机>:8088？
#    → 客户端真走了 cloudflared access（裸 TCP 到不了边缘）？
#    局域网替代路：hosts 把域名强制解析到服务器 IP 直连，不过 tunnel。
```

**局域网直连：域名解析覆盖。** 默认不覆盖——公网场景域名走正常 DNS 进 tunnel。局域网内可把 `server.memex.cuihairu.site` 钉到真实服务器 IP，流量不出网段；三种做法任选：

- **客户端内置设置**：菜单 设置 → 连接设置…，「域名解析覆盖」填 `域名=IP`（`server.memex.cuihairu.site=192.168.5.5`；多条用 `;` 分隔）。只影响本机连接目标、优先于系统 DNS；清空即恢复正常解析。
- **hosts 文件**：每台局域网机器把域名指向服务器 IP——Linux/macOS `/etc/hosts`、Windows `C:\Windows\System32\drivers\etc\hosts`，行：`192.168.5.5  server.memex.cuihairu.site`。
- **内网 DNS**：内网解析器加一条 split-horizon A 记录指向服务器内网地址——零逐机配置，需掌控内网 DNS。

### 域名与访问方式

以下名字**全部可选**。一个都不配也照常能用——局域网客户端直连 `<服务器机>:<端口>`，没配映射的入口等于不对外暴露。映射关系都在用户 Cloudflare 侧（tunnel），本仓不管理——仓里只声明各名字背后的宿主端口契约。

#### server.memex.cuihairu.site — 协作面（TCP 8088）

- **用途**：桌面/移动客户端的公网入口——登录、消息、归档取回。经 Cloudflare tunnel 以 TCP 透传到回源机 **8088**（compose 映射宿主 8088 → 容器 24360）。
- **配置位置**：用户侧 Cloudflare tunnel（`… → tcp://<回源机>:8088`）；仓里契约只有一条：协作面发布在宿主 8088、回源指向真正跑 `docker compose up` 的那台机。
- **覆盖方式**（局域网直连）：客户端 设置 → 连接设置…（`域名=IP`）、hosts 文件、内网分裂 DNS 三选一——见上文「局域网直连」。
- **可选性**：不配则局域网直连 `<宿主机>:8088`；该域名只买「公网可达」。深入排查见上文 tunnel 节。

#### admin.memex.cuihairu.site — 管理后台（HTTP 8089）

- **用途**：浏览器管理台——组织、策略、留存、归档检索导出、设备/登录台账、健康。
- **配置位置**：用户侧 tunnel HTTP 映射 `… → http://<回源机>:8089`（仓里契约：compose 发布 8089；别把 8089 裸奔公网，或前置带鉴权的反代——见下文管理台节）。
- **覆盖方式**：浏览器没有客户端侧覆盖——hosts 或内网 DNS；局域网直接开 `http://<宿主机>:8089`。
- **可选性**：不配则管理台仅局域网 `http://<宿主机>:8089` 可用。

#### webhook 24361 — 暂无域名

- **用途**：入站 `POST /hook/<token>` 通知（自动化/机器人入口）。
- **配置位置**：暂无——没有任何名字映射到它。要公网暴露需另配 HTTP ingress（用户侧，本仓不代管）；hook token 是持有式凭据，能只留局域网就别暴露。
- **覆盖方式**：若未来命名，用 hosts / 内网 DNS；局域网直连 `<宿主机>:24361`。
- **可选性**：功能面而非依赖入口，不配不影响任何主链路。

#### 文件面 24561 — 暂无域名

- **用途**：`GET/POST /files/*`——文件、头像、品牌物料、群工具附件；客户端所有文件字节都走这面。
- **配置位置**：暂无；客户端在登录对话框「文件口」填端口、连 `<宿主机>:24561`。
- **覆盖方式**：若未来命名，用 hosts / 内网 DNS；默认局域网直连。
- **可选性**：文件功能必需，但身份是**端口**而非域名。若未来真要公网入口，该面——而非 RustFS——才是正确候选。

#### 文件共享与 RustFS：为什么没有 rustfs 域名

RustFS（S3 兼容对象存储）只在 **compose 网内**听 `http://rustfs:9000`。服务端是它唯一客户端：S3 凭据由服务端持有、对象字节一律由服务端代理给客户端（文件从不客户端 → 对象存储直连）。所以 `rustfs.memex.cuihairu.site` 既不需要也不应该有——把带凭据的存储端点暴露出去只会扩大攻击面。若未来确需远程取文件，公网域名的正确候选是**文件面（24561）**本身：它有鉴权且经服务端中转。

### 管理后台（端口 8089）

`memex-admin` 是独立 Drogon 进程，与服务端开同一个 SQLite 数据库（busy-timeout 并发、与 CLI 同径数据面），在 **8089** 上提供组织管理面：账号与组织树、策略下发、留存期（双审批清档）、归档消息检索导出＋在线审计留痕、设备/登录台账、健康。判权复用服务端 az 引擎与角色语义：管理面要求 **org-admin** 有效角色，读归档消息要求 **auditor** 角色（org-admin **并不**自动可读消息——被拒尝试留痕）；会话 30 分钟不活动即超时。

```bash
# 首个管理员账号引导（一次性，走服务端容器 CLI；此后由网页控制台接管）：
docker exec -it memex-server memex_server account add admin --role admin   # 确切旗标以 `memex_server account --help` 为准
docker exec -it memex-server memex_server org role grant admin org-admin

# 域名访问：admin.memex.cuihairu.site → http://192.168.5.5:8089（tunnel／反代映射到
# 宿主 8089；映射留在终端侧，局域网按既有 memex 域名同一机制解析）。控制台 API 一律
# POST＋会话 cookie＋自定义 X-Memex-Admin 头（跨站表单防护，如实标注：非完整 CSRF
# 方案；8089 别裸奔公网，或前置带鉴权的反代）。
```

### CI 部署链（runner-Docker，192.168.5.5）

链路：ci（测试）绿 → docker（构建推 ghcr）→ **deploy**（`.github/workflows/deploy.yml`，自托管 runner）：拉不可变 `sha-<完整 sha>` → 在 `/data/deploy/memex` compose up（只改写 `.env` 的 `TAG=` 行，S3 凭据等宿主配置原样保留且流程中永不打印）→ 有界轮询（30×10s）容器 healthcheck + 宿主真 HTTP 探活 `/files/health`=200 → 不过闸自动回滚上一 tag 并验证回滚体健康。手动 `workflow_dispatch` 可指定 `sha-xxx` 显式回滚（回滚币＝历史 sha 镜像，宿主保留）。compose 同样起 `memex-admin`：对既有部署升级时首次 `up` 会随服务端一并拉取 admin 镜像，控制台即出现在宿主 8089。

runner-Docker 一次性装机（在该机上执行）：

```bash
# 1) actions runner：注册时 custom label 填 memex-docker100（deploy.yml runs-on 依赖）
mkdir -p ~/actions-runner && cd ~/actions-runner
# 从 repo Settings→Actions→Runners 取 latest 包地址与 TOKEN（会过期，现取现用）
tar xzf actions-runner-linux-x64-*.tar.gz
./config.sh --url https://github.com/cuihairu/memex --token <TOKEN> --labels memex-docker100
sudo ./svc.sh install && sudo ./svc.sh start

# 2) ghcr 拉取凭据（包私有时装到 runner 用户；设为 public 包可跳过）
docker login ghcr.io   # 用户名 cuihairu，PAT 需 read:packages

# 3) 部署目录与运行面配置（TAG 行由 deploy workflow 维护，其余手管）
mkdir -p /data/deploy/memex && cd /data/deploy/memex
cat > .env <<'EOF'
MEMEX_S3_ACCESS_KEY=<生产口令>
MEMEX_S3_SECRET_KEY=<生产口令>
EOF
```

回滚演练（验收口径：故意让一次部署失败，证明回滚链路真能回 200）——dispatch 部署一个不存在的 sha：`gh workflow run deploy -f tag=sha-deadbeef`；预期：拉取即失败或起后不过健康闸，workflow 自动回滚上一 tag 并探活 200（run 日志见「回滚 … 健康」），线上服务不中断。手工修复口径：凭据从运行中容器取（`docker exec memex-server printenv | grep ^MEMEX_S3_`），不打印到外部渠道。

### 裸机 / 虚机

```bash
# 每日构建口径（Release·仅服务端）
cmake --preset server-release && cmake --build --preset server-release
./build-server/server/memex_server serve --db /var/lib/memex/memex.db \
  --port 24360 --webhook-port 0 --files-port 24561 \
  --s3-endpoint http://127.0.0.1:9000 --s3-bucket memex \
  --s3-access-key ... --s3-secret-key ...
```

RustFS 对象存储可用 `./build-server/server/memex_server storage compose`（生成 `docker compose` 文件）或直接 `docker run rustfs/rustfs:latest`（监听契约见 `server/src/storage.cpp` 注记）。

## 工程纪律

全量测试绿才允许 commit／push，push 前先 `git fetch origin && git rebase origin/main`；每个可验收增量一笔提交，不打 git tag、不发正式版本（每日构建滚动二进制是正常交付面——滚动 `nightly` Release＋`sha-`／latest 镜像 tag）、不 force push。许可上不引入 AGPL／SSPL 组件，宽松许可组件登记进 `third_party/` 清单。命名按报告第五章第六节：代号 Memex，服务名 MemexServer，数据库与日志前缀 `memex`。
