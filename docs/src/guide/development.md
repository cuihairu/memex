# 克隆与构建

## 环境要求

- CMake 4.x、C++20 编译器（GCC 15 或 MSVC）
- [vcpkg](https://github.com/microsoft/vcpkg)：所有第三方依赖经 manifest 管理
- Linux 编译 Qt 客户端所需的系统开发包（vcpkg 不代管 X11/GL 等系统库；Ubuntu 参考：
  `autoconf autoconf-archive automake libtool`（gperf 等 autotools 端口源码构建所需，缺则 qtbase 依赖链在 gperf 处失败）；
  `libx11-dev libx11-xcb-dev libxext-dev libxfixes-dev libxi-dev libxrender-dev libxrandr-dev libxcursor-dev libxinerama-dev libxkbcommon-dev libxkbcommon-x11-dev libxcb1-dev libxcb-cursor-dev libxcb-icccm4-dev libxcb-util-dev libxcb-image0-dev libxcb-keysyms1-dev libxcb-randr0-dev libxcb-render0-dev libxcb-render-util0-dev libxcb-shape0-dev libxcb-shm0-dev libxcb-sync-dev libxcb-glx0-dev libxcb-xfixes0-dev libxcb-xinerama0-dev libxcb-xkb-dev libxcb-xinput-dev libgl1-mesa-dev libglu1-mesa-dev mesa-common-dev libfontconfig1-dev libfreetype-dev libdbus-1-dev libicu-dev`；
  与 CI 工作流的安装清单一致）
- 文档站：Node 22 + pnpm

## 依赖管理（vcpkg manifest）

仓库根 `vcpkg.json` 声明全部第三方依赖（asio、protobuf、nlohmann-json、qtbase），
版本由 `builtin-baseline` 钉在 vcpkg 上游某一次提交；个别需要锁版本的包写进
`overrides`（当前为空占位）。CI 与每日构建把 vcpkg 钉在同一 commit，
启用 GitHub Actions 二进制缓存（`x-gha`）+ 安装树缓存，跨 run 复用编译产物。

依赖清单：

| 依赖 | 用途 | 说明 |
| --- | --- | --- |
| `asio` | 服务端网络 I／O | header-only，默认安装 |
| `nlohmann-json` | 协议 JSON 编解码 | 默认安装（≥3.11.3） |
| `qtbase` | 客户端界面框架 | `client` feature，启用 `glib`／`xkb`／`xkbcommon-x11` |
| `gcovr` | CI 覆盖率汇总 | vcpkg 无此 port，由 CI 系统包提供（宿主工具，不随产物分发） |

默认（不启用 feature）只装服务端依赖，带 Qt 桌面的构建启用 `client` feature——
preset 通过 `VCPKG_MANIFEST_FEATURES` 声明，所以每日构建的服务端产物不会拖 Qt 编译，
清单仍是唯一入口。服务端 preset 另用安装树 `vcpkg_installed-server/`，
与带 Qt 的 `vcpkg_installed/` 并存，两边互不剪枝。

Qt 的 `glib` feature 不是可选项：Linux 桌面 Qt 的 xcb 平台插件引用 glib
事件分发器符号，缺该 feature 时桌面端启动即中止。

Triplet 统一用 `x64-linux-dynamic`（社区 triplet）：Qt 以 LGPL-3.0 **动态链接**
是既定合规口径，而 vcpkg 默认 triplet（`x64-linux`）对 qtbase 是静态构建，
不能用于本项目。

## vcpkg Bootstrap（一次性，随后直接用 preset）

```bash
# 1. 获取 vcpkg（建议放在固定目录，如 ~/vcpkg）
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
cd ~/vcpkg
./bootstrap-vcpkg.sh        # Linux/macOS
# .\bootstrap-vcpkg.bat     # Windows

# 2. 导出环境变量（写入 ~/.bashrc / ~/.zshrc / 用户环境变量）
export VCPKG_ROOT=~/vcpkg
```

> **CI/每日构建** 已内置 `lukka/run-vcpkg@v11`（自动 clone、bootstrap、
> 二进制缓存），无需手动干预。

## 构建与测试（Preset 一条命令）

```bash
git clone https://github.com/cuihairu/memex.git
cd memex
export VCPKG_ROOT=~/vcpkg   # 若未写入 shell rc

# 开发构建：Release · 服务端+客户端+测试
cmake --preset dev          # 首次配置按 manifest 自动装包（qtbase 编译较久）
cmake --build --preset dev
ctest --preset dev
```

Preset 一览（`CMakePresets.json`，Ninja 生成器，vcpkg 安装树统一在 `vcpkg_installed/`）：

| Preset | 用途 | 构建目录 | 配置 |
| --- | --- | --- | --- |
| `dev` | 本地开发 | `build/` | Release · 服务端+客户端+测试 |
| `ci` | CI 门禁 | `build-ci/` | Debug · `--coverage` · 全量 |
| `server-release` | 每日构建 · 服务端 | `build-server/` | Release · 仅服务端 |
| `client-release` | 每日构建 · 客户端 | `build-client/` | Release · 服务端+客户端 |

各 preset 独立构建目录，互不污染缓存；vcpkg 安装树由 manifest 统一管理，
带 Qt 的构建共用 `vcpkg_installed/`，服务端 preset 用 `vcpkg_installed-server/`。

二进制缓存：本机默认 `~/.cache/vcpkg/archives`（可用 `VCPKG_DEFAULT_BINARY_CACHE`
换目录）。manifest 未变化时，二次配置从缓存恢复编译产物，秒级完成。

## 文档站

```bash
cd docs
pnpm install
pnpm docs:dev      # 本地开发
pnpm docs:build    # 构建到 src/.vitepress/dist
pnpm docs:preview  # 本地预览构建产物
```

## 工程纪律

- 提交门禁：全量测试绿才允许 commit／push；push 前 `git fetch origin && git rebase origin/main`。
- 每个「可验收增量」一笔提交；不打 tag、不发 release、不 force push。
- 组件级许可纪律：不引入 AGPL／SSPL 组件；宽松许可组件登记进 `third_party/` 清单。
- 命名规范：代号 Memex，服务名 MemexServer，数据库与日志前缀 `memex`。

## 模块边界

Monorepo 结构与模块边界对应评审报告第四章／第五章：两套引擎各自完整、不共享通信状态，只通过共享内核交汇；服务端六模块（网关、账号与设备、组织架构、消息与归档、策略、文件元数据）。完整说明见仓库 [README](https://github.com/cuihairu/memex#readme)。
