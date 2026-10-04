---
layout: home

hero:
  name: Memex
  text: 内网办公即时通讯系统
  tagline: 物理隔离内网 · 双引擎客户端 · 全量留痕归档 · 纯自研 C++／Qt · 对标企业微信分期推进
  image:
    src: /logo.svg
    alt: Memex
  actions:
    - theme: brand
      text: 产品定位
      link: /guide/product
    - theme: alt
      text: 界面原型
      link: /guide/prototypes
    - theme: alt
      text: 需求与验收
      link: /guide/requirements

features:
  - icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M9 7v10m6-10v10M8.5 3.5h7M12 3.5V7M5 9v6m14-6v6M4 21h16a1 1 0 0 0 1-1v-3H3v3a1 1 0 0 0 1 1Z"/></svg>'
    title: 双引擎客户端
    details: 单一安装包两套引擎：未登录零配置直连态（同网段自动发现、点对点收发），登录协作态（服务端中转、全量归档）。切换不重启，服务端不可达自动回落。
  - icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><ellipse cx="12" cy="5.5" rx="8" ry="2.8"/><path d="M4 5.5V18c0 1.6 3.6 2.9 8 2.9s8-1.3 8-2.9V5.5M4 12c0 1.6 3.6 2.9 8 2.9s8-1.3 8-2.9"/></svg>'
    title: 留痕原则
    details: 连接服务端的唯一目的是留痕。不提供密聊、阅后即焚、撤回抹除归档、终端侧单方删除归档与留痕开关；撤回只作用于显示层，原文留存。
  - icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M4 21V8l7-4v17M13 21V11l7 3v7M4 21h16M7.5 9.5h.01M7.5 13h.01M7.5 16.5h.01M16.5 16.5h.01"/></svg>'
    title: 组织架构与检索
    details: 部门树、直属上级、通讯录可见性策略；协作态消息按人员、时间、关键词归档检索，查阅行为自动入审计日志。
  - icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 20h8m-4-4v4"/></svg>'
    title: 跨平台自研
    details: 桌面客户端 C++20 + Qt，覆盖 Windows、macOS 与国产 Linux（统信 UOS、麒麟）；手机端 Android（Kotlin，已落地）、iOS（Swift，代码-only＋CI）与鸿蒙 NEXT（ArkTS，代码-only＋CI）；服务端 C++ 单台集中部署；文件字节流点对点旁路，不经服务端。
---

<script setup>
const slides = [
  { key: 'desktop-direct', title: '桌面端 · 未登录直连态', desc: '零配置自动发现同网段终端，本机留存，不入归档' },
  { key: 'desktop-collab', title: '桌面端 · 登录协作态', desc: '长连接在线、消息全量归档、跨态会话显式标注' },
  { key: 'desktop-chat', title: '桌面端 · 单聊', desc: '归档起点可追溯、撤回原文留存、文件卡片与引用' },
  { key: 'desktop-group', title: '桌面端 · 群聊', desc: '公告、@提醒、群成员管理，群消息同样全量归档' },
  { key: 'desktop-org', title: '桌面端 · 组织架构', desc: '部门树、汇报链路、直属上级与可见性策略' },
  { key: 'desktop-files', title: '桌面端 · 文件传输', desc: '点对点直传不经服务端、断点续传、元数据入归档' },
  { key: 'desktop-search', title: '桌面端 · 归档检索', desc: '按人员、时间、关键词检索归档，撤回原文留存可查' },
  { key: 'mobile-init', title: '手机端 · 初始化向导', desc: '首次使用必须设置服务器地址，连通校验后才可进入' },
  { key: 'mobile-sessions', title: '手机端 · 会话列表', desc: '移动端全部为协作态，不提供免登录匿名使用' },
  { key: 'mobile-chat', title: '手机端 · 聊天', desc: '已读回执、文件卡片、归档状态随手可见' },
  { key: 'mobile-me', title: '手机端 · 我', desc: '设备管理、桌面端单点在线、留存策略展示' }
]
</script>

## 界面预览

原型设计稿覆盖桌面端七屏与手机端四屏，品牌橙（#e16531）主色，浅色与深色两套均可查看。右上角按钮切换截图主题，支持自动轮播、左右箭头、键盘方向键与触屏滑动。（实况替换进度：桌面端七屏与手机端初始化向导/会话列表已换客户端实况截图，其余见[界面原型与预览](/guide/prototypes)。）

<ShowcaseCarousel :slides="slides" />

完整的单屏大图与原型源文件见[界面原型与预览](/guide/prototypes)。

## 快速预览

### 产品形态

| 形态 | 进入方式 | 链路 | 归档 |
| --- | --- | --- | --- |
| 直连态（默认） | 安装即用，零配置 | UDP 广播发现同网段终端，TCP 点对点直达 | 仅存本机 |
| 协作态（登录后） | 填服务器地址并登录 | 与协作服务端长连接 | 全量落服务端，可检索 |

手机端全部为协作态：首次启动必须先完成服务器地址初始化，不提供免登录匿名使用。

### Monorepo 结构

```
memex/
├── client/               # Qt C++20 桌面客户端（Windows / UOS / 麒麟 / macOS）
│   ├── app/              # 入口与主窗口、系统集成（界面层并入 app）
│   ├── engine/direct/    # 直连引擎：UDP 发现、点对点消息与文件
│   ├── engine/collab/    # 协作引擎：长连接、消息同步、离线与补传
│   ├── core/             # 共享内核：本地库、会话、联系人、文件传输
│   └── tests/            # 桌面端单测
├── server/               # C++ 协作服务端（单进程实现评审报告六模块职能）
├── common/               # 双端共用：协议定义、序列化、公共工具
├── apps/                 # 手机端：android（已落地）/ ios（代码-only＋CI）
│                         #   / harmony（代码-only＋CI）
├── admin/                # 管理后台（规划，README 占位）
├── docs/                 # 文档站（VitePress）、评审报告、原型
└── third_party/          # 第三方组件清单与许可核验（依赖经 vcpkg 管理）
```

### 构建

依赖（asio / protobuf / nlohmann-json / sqlite3 / openssl / aws-sdk-cpp:s3 / Qt6）经 vcpkg manifest 管理，构建入口为 CMake Presets：

```bash
# 准备 vcpkg（一次性）
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT="$HOME/vcpkg"

# 开发构建（Release · 服务端+客户端+测试）
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

更多内容：[产品定位](/guide/product) · [需求与验收](/guide/requirements) · [四期规划](/guide/roadmap) · [决策清单](/guide/decisions) · [评审报告摘要](/guide/report) · [克隆与构建](/guide/development)
