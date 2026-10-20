# Review Report Summary

The full text of the《内网办公即时通讯系统建设方案评审报告 V10.0》 is in the repository at [`docs/建设方案评审报告-V10.0.md`](https://github.com/cuihairu/memex/blob/main/docs/建设方案评审报告-V10.0.md) (about 1200 lines). This page extracts the conclusions engineering relies on directly.

## Key review conclusions

- **Self-written business layer, no secondary development**: no secondary development based on open-source instant messaging software, and no integration of strongly infectious license components such as AGPL / SSPL; the foundation uses open-source components such as Qt, asio, protobuf, and SQLite (list in `third_party/README.md`), introduced only as deployment-time invocation or permissive-license linking, with a component list submitted at delivery.
- **Client stack: C++17 + Qt**: the evaluation dimensions included media capability. WebKitGTK as packaged by mainstream distributions does not build WebRTC by default, so Tauri-style routes cannot establish media connections on domestic Linux terminals; the Qt route has the lowest adaptation cost on domestic systems and small packages. WebRTC is the planned media pipeline and has not been introduced; current remote assistance relays JPEG frames through the server-side HTTP surface (see [Product Positioning](product.md)).
- **Dual-engine client**: a zero-configuration direct mode when not logged in plus a server-backed collaboration mode when logged in; the two engines are each complete, do not share communication state, and only converge through a shared core (local message database, session list, contact view, file transfer module).
- **Six server modules**: access gateway, accounts and devices, organizational structure, messages and archiving, policy service, file metadata service; single-machine centralized deployment with logical split reserved. Chat file byte streams bypass the server peer-to-peer (metadata only enters the archive); R23 file-surface files (group files / personal files / file assistant / external-network uplink) have their bytes proxied through the server into object storage, and the client never directly connects to object storage.
- **Archiving and traceability**: archiving is in units of messages; recall only affects the display layer, the server keeps the original text and records the recall event; cross-state sessions are explicitly exposed with the「未归档」label; no capability exists that lets the sender eliminate the trace.
- **Naming conventions**: codename Memex, service name MemexServer, database and log prefix `memex`, intranet domain memex.corp.local.

## Deployment and implementation recommendations (phase one)

- About 14 weeks: pilot deployment (weeks 1–4) → direct-mode features → collaboration mode and archiving → admin console → phased rollout (weeks 13–14, full acceptance).
- Network admission: UDP 2425 (within the report's fixed segment 2425–2436, only 2425 is in active use; 2426–2436 is a reserved segment) / TCP 2426–2437, ports fixed to cooperate with network-side policy.
- The phase-two office location map and remote assistance are separately initiated for acceptance; remote assistance must verify four items: authorization confirmation, visible process, operation traceability, and the per-department switch.

## Items awaiting review panel decisions

Chapter 11 lists fifteen items for the review panel to decide (plus one optional confirmation on FeiQ compatibility). These have been organized into the[决策清单](/en/guide/decisions) and are being advanced per the "report recommendation" column.
