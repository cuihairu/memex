---
layout: home

hero:
  name: Memex
  text: Intranet Office Instant Messaging System
  tagline: Dual-engine IM for a physically isolated intranet — messaging works without a server, and everything is fully archived the moment one connects; built in C++17 + Qt
  image:
    src: /logo.svg
    alt: Memex
  actions:
    - theme: brand
      text: Product Positioning
      link: /guide/product
    - theme: alt
      text: UI Prototypes
      link: /guide/prototypes
    - theme: alt
      text: Requirements and Acceptance
      link: /guide/requirements

features:
  - icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M9 7v10m6-10v10M8.5 3.5h7M12 3.5V7M5 9v6m14-6v6M4 21h16a1 1 0 0 0 1-1v-3H3v3a1 1 0 0 0 1 1Z"/></svg>'
    title: Dual-Engine Client
    details: "Two engines in a single installer: an unauthenticated direct-connect mode with zero configuration (same-subnet auto-discovery, peer-to-peer send and receive), and a signed-in collaboration mode (server relay, full archival). Switching does not restart the client, and it falls back automatically when the server is unreachable."
  - icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><ellipse cx="12" cy="5.5" rx="8" ry="2.8"/><path d="M4 5.5V18c0 1.6 3.6 2.9 8 2.9s8-1.3 8-2.9V5.5M4 12c0 1.6 3.6 2.9 8 2.9s8-1.3 8-2.9"/></svg>'
    title: Traceability Principle
    details: "Connecting to the server exists for one purpose: leaving a trail. There is no secret chat, no burn-after-reading, no recall that erases the archive, no device-side unilateral archive deletion, and no traceability switch; recall only affects the display layer and the original text is retained."
  - icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M4 21V8l7-4v17M13 21V11l7 3v7M4 21h16M7.5 9.5h.01M7.5 13h.01M7.5 16.5h.01M16.5 16.5h.01"/></svg>'
    title: Organization Structure and Search
    details: Department tree, direct supervisor, and address-book visibility policy; collaboration-mode messages are archived and searchable by person, time, and keyword, and every lookup is recorded in the audit log automatically.
  - icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 20h8m-4-4v4"/></svg>'
    title: Cross-Platform Clients
    details: Desktop client in C++17 + Qt, covering Windows, macOS, and Chinese Linux (UnionTech UOS, Kylin); mobile on Android (Kotlin, shipped), iOS (Swift, code-only + CI), and HarmonyOS NEXT (ArkTS, code-only + CI); the server is C++ deployed centrally on a single host; file byte streams take a peer-to-peer side channel and never pass through the server.
---

> **English** | [中文](/)


<script setup>
const desktopSlides = [
  { key: 'live-direct', title: 'Desktop · Unauthenticated Direct Mode', desc: 'Zero-configuration discovery of endpoints on the same subnet; retained on the local machine, not archived' },
  { key: 'layout-friends', title: 'Desktop · Friend List', desc: 'List state when no conversation is open: the chat panel stays hidden until a friend is clicked' },
  { key: 'live-collab', title: 'Desktop · Signed-In Collaboration Mode', desc: 'Persistent connection, messages fully archived, cross-mode conversations explicitly labeled' },
  { key: 'live-chat', title: 'Desktop · One-to-One Chat', desc: 'Direct one-to-one chat reaches the peer point to point: the line header is marked 「直连·仅本机」, and a banner states that messages are not archived' },
  { key: 'live-group', title: 'Desktop · Group Chat', desc: 'Group messages fan out through the server and are fully archived; the sidebar group section carries member counts and an archival marker' },
  { key: 'live-org', title: 'Desktop · Organization Structure', desc: 'Department tree, member cards, and reporting lines' },
  { key: 'live-files', title: 'Desktop · File Transfer', desc: 'Peer-to-peer direct transfer bypasses the server; resumable; metadata is archived' },
  { key: 'live-search', title: 'Desktop · Archive Search', desc: 'Search the archive by person, time, and keyword; recalled originals are retained and still retrievable' }
]
const mobileSlides = [
  { key: 'mobile-live-init', title: 'Mobile · Setup Wizard', desc: 'First use requires setting a server address, and entry is only allowed after the connectivity check passes' },
  { key: 'mobile-live-sessions', title: 'Mobile · Session List', desc: 'Mobile is entirely in collaboration mode; no anonymous use without login is provided' },
  { key: 'mobile-live-chat', title: 'Mobile · Chat', desc: 'Real person-to-person exchange (offline delivery → delivered on login → replied while online); long-press a message to copy it' },
  { key: 'mobile-live-me', title: 'Mobile · Me', desc: 'Organization profile and account card, device management, cache cleanup, and account actions; unimplemented items are honestly marked as such' }
]
</script>

## UI Preview

The UI preview covers eight desktop screens and four mobile screens, in the brand orange (#e16531), with both light and dark variants available. Desktop and mobile are split into two carousels; the button in the top-right corner switches the screenshot theme, and auto-advance, left/right arrows, keyboard arrow keys, and touch swipes are supported. (Progress on swapping in live shots: all eight desktop and all four mobile screens have been replaced with live client screenshots, one light and one dark each, and all twelve carousel slots are live; live screenshots are not promised for the HarmonyOS client. See [UI Prototypes and Preview](/en/guide/prototypes).)

### Desktop (Windows / UOS / Kylin / macOS)

<ShowcaseCarousel :slides="desktopSlides" label="Desktop UI preview" />

### Mobile (Android / iOS / HarmonyOS)

<ShowcaseCarousel :slides="mobileSlides" label="Mobile UI preview" />

Full-size single-screen images and prototype source files are in [UI Prototypes and Preview](/en/guide/prototypes).

## Quick Preview

### Product Modes

| Mode | How to enter | Path | Archive |
| --- | --- | --- | --- |
| Direct mode (default) | Install and use immediately, zero configuration | UDP broadcast discovers endpoints on the same subnet, TCP reaches the peer directly | Local machine only |
| Collaboration mode (after login) | Enter the server address and sign in | Persistent connection to the collaboration server | Fully stored on the server, searchable |

Mobile is entirely in collaboration mode: on first launch the server address must be initialized before anything else, and no anonymous use without login is provided.

### Monorepo Structure

```
memex/
├── client/               # Qt C++17 桌面客户端（Windows / UOS / 麒麟 / macOS）
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

### Build

Dependencies (asio / protobuf / nlohmann-json / sqlite3 / openssl / aws-sdk-cpp:s3 / Qt6) are managed through the vcpkg manifest, and the build entry points are CMake presets:

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

More: [Product Positioning](/en/guide/product) · [Requirements and Acceptance](/en/guide/requirements) · [Four-Phase Roadmap](/en/guide/roadmap) · [Decision List](/en/guide/decisions) · [Review Report Summary](/en/guide/report) · [Clone and Build](/en/guide/development)
