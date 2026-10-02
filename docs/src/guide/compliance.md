# 合规与命名一致性（T5.3）

> 对应验收项 A22（组件许可清单）与 A23（系统标识一致性）。核验日期：2026-10-03。

## A22 组件许可核验记录

第三方组件经 vcpkg manifest（仓库根 `vcpkg.json`）单一事实源声明，
清单见 `third_party/组件清单.md`。本轮核验结论：

| 核验点 | 结论 |
| --- | --- |
| 强传染性许可（AGPL／SSPL 等） | 无——清单仅覆盖 asio（BSL-1.0）、protobuf（BSD-3-Clause）、nlohmann/json（MIT）、vcpkg（MIT）、Qt（LGPL-3.0） |
| Qt 链接方式 | LGPL-3.0 动态链接（`x64-linux-dynamic` triplet），不静态链接、不修改 Qt 源码 |
| 源码 vendoring | 仓库不 vendor 第三方源码，全部由 vcpkg 按 baseline 获取 |
| 依赖升级路径 | 提升 `builtin-baseline` 一并更新清单版本列；`overrides` 当前空（无个别锁版） |

交付时本清单随包一并提供，即为 A22 交付物。

## A23 系统标识一致性核查

对照评审报告 V10.0 第五章第六节「表 14 系统标识与命名清单」逐项核查当前产物：

| 规范项 | 规范值 | 仓库现状 | 结论 |
| --- | --- | --- | --- |
| 英文名与代号 | Memex | README、文档站 title、协议 package `memex.protocol.v1` | ✓ |
| 代码仓库名 | memex | GitHub `cuihairu/memex`；服务端产物 `memex_server`、客户端 `memex_client` 二进制与 `memex-server`/`memex-client` 包名 | ✓ |
| 数据库与日志前缀 | memex、[MEMEX] | 服务端默认库 `memex-server.db`、客户端本地库 `memex-local.db`、日志行前缀 `[MEMEX]`（`server/src/server.cpp`） | ✓ |
| Windows 服务名 | MemexServer | 文档站（development.md、report.md）与服务端入口注释 `MemexServer 入口` 口径一致；服务端运行期日志以 `[MEMEX] MemexServer 监听` 输出 | ✓ |
| 内网域名／主机名 | memex.corp.local（示例）、memex-srv | 评审报告部署章给出同口径示例 | ✓ |
| 客户端安装包命名 | `MemexClient-x.y.z-*` | 随 T4.7 系统集成落地时执行，当前 nightly 产物为 `memex-client-linux-x64-<日期>-<短SHA>.tar.gz` | 待 T4.7 |
| 表述口径 | 「归档与检索」，不用「监控」「追踪」「审查」 | 界面归档提示条用「消息不进归档」，README 与文档站同口径 | ✓ |

核查结论：除 T4.7 安装包命名随系统集成落地外，其余标识与命名清单一致，无偏差项。
