> [English](/en/guide/delivery-matrix) | **中文**

# 交付验收矩阵（评审八-5）

> 口径铁律：**CI 绿 ≠ 全平台可交付**。CI 只在 ubuntu-latest 跑，其余平台的证据
> 到哪一档就写哪一档。本页把「平台 × 通信模式 × 账号状态 × 网络状态 × 权限角色」
> 的组合如实分档，报告引用平台能力时以本页为准，不得混写。
> 核验日期：2026-10-13。证据只取仓库内：CI 工作流定义（`.github/workflows/`）、
> ctest 测试名（83 项，ctest 名去重）、各端 README/BUGS 记录、带日期的实跑页
> （`platform-adapter.md`、`wan-drill.md` 均为 2026-10-03 记录）。

## 分档定义

| 档 | 含义 | 判据 |
| --- | --- | --- |
| ✅ 实测通过 | 仓库内自动化测试或带日期实跑记录直接覆盖该组合 | ctest 测试名／CI 工作流／文档实跑记录；**注明验证层级**（服务端逻辑／客户端 UI／JVM 单测／Node 逻辑层——层不等价，见各表注） |
| 🔧 编译验证 | 该平台有编译产物（＋安装/自检冒烟），功能无测试覆盖 | nightly 产物矩阵、`--self-test`、各端构建记录 |
| ⬛ 仅代码存在 | 代码在仓库，无该平台编译或运行证据（＝未验证，不许猜） | 无任何工作流或记录涉及 |
| — 不适用 | 设计排除，非验证档（如 R18：移动端无直连态） | 需求条目明文排除 |

平台列说明：**Linux x86-64 (Ubuntu)** 是唯一全量证据列（CI＋ctest）；**统信 UOS／
麒麟** 无实机（A12 现场待补清单见 `platform-adapter.md`），nightly 的通用 x86-64
deb/rpm 只是同架构产物，未在目标系统安装过；**Windows** 只有客户端进 nightly
（服务端不在 Windows 构建）；**Android** 无 CI 作业（本地 gradle 记录）；
鸿蒙／iOS 为附列（仓库另有交付面，评审五平台清单外）。

## 表0 · 平台 × 交付基线

| 交付物 | Linux x86-64 (Ubuntu) | Windows x64 | 统信 UOS | 麒麟 | macOS arm64 | Android | 鸿蒙 | iOS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 服务端可构建 | ✅ ci+nightly | ⬛ | ⬛ | ⬛ | 🔧 nightly | — | — | — |
| 桌面客户端可构建 | ✅ ci+nightly | 🔧 nightly | ⬛ | ⬛ | 🔧 nightly | — | — | — |
| 移动端 App 可构建 | — | — | — | — | — | 🔧 ¹ | ⬛ ² | 🔧 ³ |
| 安装包产物 | 🔧 tar+deb/rpm | 🔧 zip+setup.exe | 🔧 ⁴ | 🔧 ⁴ | 🔧 dmg×2 | 🔧 apk(debug) | ⬛ | — |
| 安装后自检冒烟（--self-test 仅服务端；客户端＝--version/--smoke） | 🔧 nightly | 🔧 nightly | ⬛ | ⬛ | 🔧 nightly | — | — | — |
| 全量自动化测试（ctest 83 项） | ✅ ci.yml | ⬛ | ⬛ | ⬛ | ⬛ | — | — | — |

¹ `assembleDebug` 本地产物（BUGS.md 记录在案）；无 CI 作业。release 变体单测偶发红＝BUG-005，2026-10-10 已修（定性后修测试面断言，双变体 84/84×2 全绿收口）。
² ArkTS HAP 编译腿无 OHOS runner——harmony.yml 明示「不在此伪造编译通过」；纯逻辑层有 CI（见下表）。
³ ios.yml（macos-26）：swift test ＋ 模拟器构建；真机未验证。
⁴ nightly 通用 x86-64 deb/rpm 与 Ubuntu 同源同架构，未在 UOS/麒麟实机安装——实机验收＝`platform-adapter.md` 现场待补清单。

## 表1 · 平台 × 通信模式

| 通信模式 | Linux x86-64 (Ubuntu) | Windows x64 | 统信 UOS | 麒麟 | macOS arm64 | Android | 鸿蒙 | iOS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 直连态收发（免登录·同网段 P2P＋UDP 发现） | ✅ direct_chat/discovery/ipmsg | ⬛ | ⬛ | ⬛ | ⬛ | — ⁵ | — ⁵ | — ⁵ |
| 协作态单聊（登录·中转·归档） | ✅ collab_login/collab_chat | ⬛ | ⬛ | ⬛ | ⬛ | ✅ ⁶ | ✅ ⁷ | ✅ ⁸ |
| 群聊（扇出/公告/解散） | ✅ group | ⬛ | ⬛ | ⬛ | ⬛ | ✅ ⁶ | ✅ ⁷ | ⬛ |
| 聊天文件直传（点对点不经服务端） | ✅ direct_file/file_authz | ⬛ | ⬛ | ⬛ | ⬛ | — ⁶ | — ⁶ | — |
| 文件面（服务端持久化·配额/秒传） | ✅ files_api/uplink | ⬛ | ⬛ | ⬛ | ⬛ | ✅ ⁶ | ⬛ | ⬛ |
| 跨态互通（未归档标识·归档起点） | ✅ cross/mode_switch | ⬛ | ⬛ | ⬛ | ⬛ | — ⁵ | — ⁵ | — ⁵ |
| 三级通知（普通/重要/紧急确认收悉） | ✅ notify（桌面） | ⬛ | ⬛ | ⬛ | ⬛ | ✅ ⁶ | ✅ ⁷ | ⬛ |

⁵ R18：直连态／免登录能力仅存在于桌面端，移动端全部为协作态——设计排除，非验证档。
⁶ Android：实测层级＝JVM 单测 95 项（真 socket 假服务端；debug 变体），**非真机 UI 端到端**；聊天文件直传为桌面端专属能力（Android README 口径），文件面走 FilesClientTest。
⁷ 鸿蒙：实测层级＝纯 TS 逻辑层 Node 22 单测（harmony.yml CI 全绿，wire 与桌面端字节级交叉验证）；**HAP 内真实运行未验证**（无 OHOS runner）。
⁸ iOS：实测层级＝MemexKit core 单测 53 项（真 socket 假服务端，ios.yml macos-26）；应用 UI 未验证。

## 表2 · 平台 × 账号状态

| 账号状态 | Linux x86-64 (Ubuntu) | Windows x64 | 统信 UOS | 麒麟 | macOS arm64 | Android | 鸿蒙 | iOS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 免登录直连态（桌面专属，R1） | ✅ discovery/mode_switch | ⬛ | ⬛ | ⬛ | ⬛ | — ⁵ | — ⁵ | — ⁵ |
| 正常登录/登出（桌面含双态切换不重启，R5） | ✅ mode_switch/accounts | ⬛ | ⬛ | ⬛ | ⬛ | ✅ ⁶ R17 向导/R18 | ✅ ⁷ | ⬛ |
| 单点互踢（KICK，R4） | ✅ accounts | ⬛ | ⬛ | ⬛ | ⬛ | ✅ ⁶ | ✅ ⁷ | ⬛ |
| 新设备待审批（T3.4） | ✅ org | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |
| 设备停用（T3.3） | ✅ authz_audit | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |
| LDAP 绑定账号（目录判登，544） | ✅ ldap | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |
| LDAP 影子账号（auto_provision，544） | ✅ ldap | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |

LDAP 行是服务端判登面（`test_ldap` 16 腿，Linux CI）：任何客户端登录被绑账号都
走同一路由，但各端**未**在 LDAP 场景单独验证过，故非 Linux 列一律 ⬛。

## 表3 · 平台 × 网络状态

| 网络状态 | Linux x86-64 (Ubuntu) | Windows x64 | 统信 UOS | 麒麟 | macOS arm64 | Android | 鸿蒙 | iOS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 同网段在线（常规收发） | ✅ | ⬛ | ⬛ | ⬛ | ⬛ | ✅ ⁶ | ✅ ⁷ | ⬛ |
| 断线重连＋离线补投（msg_id 去重） | ✅ gateway/collab_resend/msg_faults | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ ⁹ | ⬛ | ⬛ |
| 服务端不可达→降级直连（A11） | ✅ mode_switch（真服务端进程杀死腿） | ⬛ | ⬛ | ⬛ | ⬛ | — ⁵ | — ⁵ | — ⁵ |
| 跨网段协作态（A13 跨 VLAN 实测） | ⬛ ¹⁰ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |
| 弱网（netem 80ms±20ms·丢包 2%） | ✅ wan-drill 2026-10-03 实跑 | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |

⁹ Android JVM 覆盖收发/ACK/去重/KICK；断线重连场景未列入单测。
¹⁰ 协作态不做网段限制是设计口径（服务端中转），回环测试不代表真跨网段——跨 VLAN
实机核查清单已备（`wan-drill.md` A13 节），**未执行**，全列 ⬛ 不虚标。

## 表4 · 平台 × 权限角色

| 权限角色 | Linux x86-64 (Ubuntu) | Windows x64 | 统信 UOS | 麒麟 | macOS arm64 | Android | 鸿蒙 | iOS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 普通成员（default-deny 白名单） | ✅ authz/files_api | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |
| 群主（建群/拉人/解散/群文件） | ✅ group | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |
| org-admin（后台管理·每请求重算） | ✅ admin_api | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |
| auditor（归档检索导出·留痕） | ✅ admin_api/visibility | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |
| LDAP 显式映射角色（表外零授权） | ✅ ldap | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |

角色行验证的是服务端判权面（Linux CI）：客户端 UI 的角色对话框（审计/审批/报表
等）另有对应 ctest（`audit_dialog`/`approval_dialog`/`report_dialog` 等），同样只在
Linux。非 Linux 平台连判权都未跑过，整列 ⬛。

## 汇总（240 格）

| 档 | 格数 | 占比 |
| --- | --- | --- |
| ✅ 实测通过 | 40 | 16.7% |
| 🔧 编译验证 | 14 | 5.8% |
| ⬛ 仅代码存在（未验证） | 153 | 63.8% |
| — 不适用（设计排除） | 33 | 13.8% |

实测的 40 格集中在 Linux 列与移动端逻辑层（Android JVM／鸿蒙 Node／iOS core）；
**全部 240 格中没有任何一格的实测证据来自 Windows、UOS、麒麟、macOS 本机**。

## 缺口清单（升级路径）

1. **Windows/macOS 功能面**：nightly 只证明「能编译能自检」， 表1–表4 全部功能格
   ⬛。升级路径＝对应平台 runner 上跑功能子集（协作收发＋文件＋账号状态）。
2. **UOS/麒麟实机（A12）**：`platform-adapter.md` 现场待补清单三条（安装→直连/
   收发/文件/高 DPI、X11＋Wayland 各一次、降级提示逐项）。补测通过后本页 UOS/
   麒麟列才可升档。
3. **跨网段实机（A13）**：`wan-drill.md` 核查清单未执行，现全列 ⬛。
4. **Android**：无 CI 作业（本地 gradle 手跑）；release 变体 BUG-005 已修（2026-10-10，
   双变体 84/84×2 全绿）；断线重连、新设备审批、设备停用未入单测。
5. **鸿蒙 HAP**：编译腿与真机功能面待 OHOS runner／实机。
6. **iOS**：模拟器构建之外，UI 与真机未验证。
