# Memex · 内网办公即时通讯系统

内网物理隔离环境下的办公即时通讯系统，对标企业微信的沟通、办公、组织与归档检索能力。纯自研：不基于开源即时通讯软件二次开发，不集成 AGPL／SSPL 等强传染性许可组件。客户端 C++17 + Qt，服务端 C++。

- 产品定义与分期依据：[docs/建设方案评审报告-V10.0.md](docs/建设方案评审报告-V10.0.md)
- 产品定位一页纸：[docs/src/guide/product.md](docs/src/guide/product.md)
- 需求与任务清单（R1–R19、四期拆解、验收口径）：[todo.md](todo.md)
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
├── client/               # Qt C++17 客户端（Windows / UOS / 麒麟）
│   ├── app/              # 入口与主窗口、系统集成（托盘、通知、开机启动）
│   ├── engine/direct/    # 直连引擎：UDP 发现、点对点消息与文件
│   ├── engine/collab/    # 协作引擎：长连接、消息同步、离线与补传
│   ├── core/             # 共享内核：本地库（SQLite）、会话、联系人、文件传输
│   └── ui/               # 界面层：会话、群聊、通讯录、检索、设置
├── server/               # C++ 协作服务端（单台集中部署）
│   ├── gateway/          # 接入网关：长连接与消息路由
│   ├── account/          # 账号与设备：认证、设备台账、桌面端单点在线
│   ├── org/              # 组织架构：部门、成员、直属上级、权限
│   ├── archive/          # 消息与归档：落库、检索、导出（只追加，不抹除）
│   ├── policy/           # 策略服务：按部门下发开关
│   └── filemeta/         # 文件元数据服务（文件字节流旁路，不经服务端）
├── common/               # 双端共用：协议定义、序列化、公共工具
├── admin/                # 管理后台（第一期第四阶段起）
├── docs/                 # 文档站（VitePress）、评审报告、产品文档、原型
└── third_party/          # 第三方组件清单与许可核验（A22）
```

模块边界对应评审报告第四章：两套引擎各自完整、不共享通信状态，只通过共享内核（本地消息库、会话列表、联系人视图、文件传输模块）交汇；服务端六模块与数据模型见报告第五章。

## 构建

```bash
# 服务端与公共库（C++20，依赖 asio）
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

# 客户端（需 Qt 6）
cmake -B build -S . -DMEMEX_BUILD_CLIENT=ON
cmake --build build
```

## 工程纪律

- 提交门禁：全量测试绿才允许 commit／push；push 前 `git fetch origin && git rebase origin/main`。
- 每个「可验收增量」一笔提交，不打 tag、不发 release、不 force push。
- 组件级许可纪律：不引入 AGPL／SSPL 组件；宽松许可组件登记进 `third_party/` 清单。
- 命名规范按报告第五章第六节：代号 Memex，服务名 MemexServer，数据库与日志前缀 `memex`。
