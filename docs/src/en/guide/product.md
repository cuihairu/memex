> **English** | [中文](/guide/product)

# Product Positioning

> Intranet office instant messaging system (Memex) · converged per the《建设方案评审报告 V10.0》, a one-page statement.

## One sentence

In an office environment with a physically isolated intranet and a mix of Windows and domestic Linux (UOS / Kylin) terminals, provide an instant messaging system that runs entirely on the intranet with a self-written business layer: communication and file sharing replace existing LAN tools; after login it offers an organizational structure plus message archiving and retrieval; the functional scope follows 企业微信 and is advanced in four phases.

## Dual-engine form (the product's first feature)

A single installer, two complete engines, each on its own link, sharing only the local data layer:

- **Direct mode (not logged in, default)**: zero configuration — no address to fill in, no account to register; UDP broadcast automatically discovers terminals on the same segment, with peer-to-peer messaging and file transfer. Messages are stored only on the local machine; the server keeps no records. It is positioned for temporary, nearby use within this segment (visitors passing through, borrowing a machine, instant communication in the same area); it does not pursue cross-segment communication.
- **Collaboration mode (after login)**: fill in the server address and log in; messages are relayed through the server and fully archived, unlocking the organizational structure, group chat, cloud history, retrieval and other capabilities. After login the direct engine is not shut down (cross-state interoperability with colleagues who are not logged in depends on it); such sessions are permanently labeled「未归档」in the UI.
- **Degradation**: when the server is unreachable, automatically fall back to direct mode and basic communication continues: the server is the single point for archiving and management capabilities, not the single point for communication capability.

Mobile clients are entirely collaboration mode: first startup must complete server address initialization before the main interface is entered; no login-free anonymous use is provided.

## The traceability principle (a non-negotiable premise)

**The only purpose of connecting to the server is traceability.** Any feature that "lets the sender eliminate the trace on their own" will not be built:

1. Secret chats and burn-after-reading: no conversation form of "automatically disappears after sending" exists;
2. Recall erases the archive: recall only affects the client display layer; the server keeps the original text and additionally records the recall action itself;
3. One-sided deletion of the archive on the terminal side: clearing the cache only affects the local machine, not the server records; the UI clearly distinguishes「本地缓存」from「云端归档」;
4. A traceability switch for users: archiving policy is only issued by the administration side, per department.

The direct mode's non-traceability is an inherent property of that form (it does not pass through the server), not an on-demand hidden mode; the collaboration mode's traceability is the very purpose of the build and is not exempted by any technical means.

## Boundaries and things not done

- **Meetings and collaborative documents are not self-written**: audio/video meetings and collaborative documents (online documents / spreadsheets / knowledge base) belong to separate technical domains and are procured or integrated in phase four.
- **No secondary development**: not built by modifying open-source instant messaging software; open-source components are introduced only as deployment-time invocation or permissive-license linking, and a component list is submitted at delivery.
- **The direct mode does not cross segments**: a broadcast not leaving the segment is inherent network behavior; cross-segment and cross-region communication is always carried by the collaboration mode, and no network-side modification is made for the direct mode.
- **Wording discipline**: the UI and materials uniformly say「归档与检索」, not「监控」「追踪」「审查」; recall notices only say「消息已撤回」, never「已删除」「不再保存」.

## Technical route

- Foundation: open-source components such as Qt 6, asio, protobuf, nlohmann-json, SQLite, OpenSSL, and AWS SDK (S3 interface); versions and licenses in `third_party/README.md`. The entire business layer above is written in-house and contains no secondary-development code from any open-source IM.
- Client: C++17 + Qt (the review report set the tone at C++17 + Qt, and the implementation lands in C++17; mature networking and local libraries, lowest adaptation cost on domestic systems, small packages). The media foundation (libdatachannel (MPL-2.0) or the WebRTC reference implementation (BSD-3-Clause)) is **not introduced and deferred** — current media (remote-assistance screen frames/input) goes through the server-side HTTP relay (controllable within the intranet, see R16 remote assistance). Mobile: Android (Kotlin) / iOS (Swift) / HarmonyOS NEXT (ArkTS).
- Server: C++17, single-machine centralized deployment, six modules per the review report: access gateway, accounts and devices, organizational structure, messages and archiving, policy service, file metadata service — currently implemented converged as a single process and single database (review decision item 7, with logical split reserved).
- File dual-channel (two lifecycles, not mixed): **chat files** are transferred peer-to-peer at the byte level without passing through the server — the bytes land only on the sending and receiving machines' local storage and do not enter the archive; before sending, a unified permission check goes through the server (can send / can receive / cross-department / re-forward); archiving of transfer metadata (file name, size, hash, sender and receiver, time) is **deferred** (not persisted to the database yet; the server has no transfer-metadata table); **file-surface files** (group files / personal files / file assistant / external-network uplink) have their bytes proxied through the server into object storage, and the client never directly connects to object storage. Lifecycle and degradation details are in [文件存储与生命周期](/en/guide/file-storage).
- AI (phase 3): a unified model gateway speaking the OpenAI-compatible protocol, pluggable; archived data is only allowed to be sent to local models, and external endpoints are disabled by default.
- Multi-terminal model: an account can log in on different machines; on the desktop, the same account has only one instance online, and a repeated login kicks out the previous one with a notice; each login records the device fingerprint (hash only), type, and source.

## Phases

| Phase | Objective | Delivery milestone |
| --- | --- | --- |
| Phase 1 | The communication and file sharing core (dual engine, messaging, group chat, screenshots and emoji, frequent contacts, organizational structure, file transfer, archive retrieval, admin console) | Can replace existing LAN tools, and has archiving and retrieval capabilities |
| Phase 2 | Office capabilities (approvals, schedule and to-dos, announcements, group tools, location map, remote assistance) | Has lightweight office collaboration capabilities |
| Phase 3 | AI capabilities (in-group assistant, meeting minutes, requirement organization) | Group chat content can be automatically structured |
| Phase 4 | Audio/video meetings and collaborative documents | Independently initiated, procured or integrated |
