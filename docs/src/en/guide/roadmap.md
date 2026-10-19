> **English** | [中文](/guide/roadmap)

# The Four-Phase Plan

| Phase | Objective | Delivery milestone |
| --- | --- | --- |
| Phase 1 | The communication and file sharing core: dual engine, messaging, group chat, screenshots and emoji, frequent contacts, organizational structure, file transfer, archive search, admin console | Can replace existing LAN tools, and has archiving and retrieval capabilities |
| Phase 2 | Office capabilities: approvals, schedule and to-dos, announcements, group tools, location map, remote assistance | Has lightweight office collaboration capabilities |
| Phase 3 | AI capabilities: in-group assistant, meeting minutes, requirement organization | Group chat content can be automatically structured |
| Phase 4 | Audio/video meetings and collaborative documents | Independently initiated, procured or integrated |

## Phase-one task stages

The task-level breakdown is in [todo.md](https://github.com/cuihairu/memex/blob/main/todo.md); this is a stage overview:

- **Stage 0 · Engineering skeleton**: a CMake-aggregated monorepo with the protocol skeleton, server skeleton, and Qt client skeleton, with all ctest legs green.
- **Stage 1 · Direct mode (phase-one main line)**: UDP broadcast discovery, peer-to-peer messages, file and directory transfer, local history.
- **Stage 2 · Collaboration mode (phase-one main line)**: accounts and devices, access gateway and message archiving, dual-state switching, offline re-send, organizational structure.
- **Stage 3 · Admin console (phase-one main line)**: account and organization management, policy issuance, archive search and export, audit.
- **Stage 4 · Group chat, cross-state, and experience**: group chat, cross-state interoperability and the「未归档」label, screenshots and emoji plus frequent contacts, mobile initialization wizard (R17 / R18), theme switching (R19, started after the main line), the notification subsystem (webhook ingestion + three-tier severity-graded push + per-person notification preferences, T4.10).
- **Stage 5 · Platform adaptation and acceptance**: UOS / Kylin adaptation, backup-recovery drill, license verification, cross-region joint debugging.
- **Stage 6 · Platform matrix extension**: macOS artifacts (client dmg / server pkg-in-dmg, delivered with the three-platform installer matrix in daily builds); mobile Android (Kotlin, **landed**: R17/R18 login + session-list send/receive + three-tier push banner vibration, T6.3); iOS (Swift, code-only + CI verification, T6.4; real-device APNs awaits real devices); HarmonyOS NEXT desktop (Qt, **environment-blocked, code side closed out**: the hvigor / hdc / ohpm toolchain, signing certificates, and real devices are all unready; the code side was closed out on 2026-10-10 per the "code only" order, and real-device acceptance will start separately once the environment unlocks, T6.2); HarmonyOS phone edition (ArkTS, code-only + CI logic verification, with the build leg awaiting the OHOS toolchain, T6.6).

## Phase two and beyond (the eight phase-two items have landed and closed; phase three and beyond are refined after separate initiation)

- Approvals (forms + process engine), schedule and to-dos, announcements and daily/weekly reports, group tools (bots / polls / chains / group tasks)
- Conversation audit (view permissions, view logs, compliance traceability)
- Office location map (floor plans, drag-and-drop point editor, search-and-locate; visible only on one's own floor by default)
- Remote assistance (screen viewing + authorized operations by the controlled party; currently relays JPEG frames over server-side HTTP, with the WebRTC media foundation not implemented and deferred; explicit confirmation by the controlled party, continuously visible process, full traceability, per-department switch; unattended use is not provided)
- Bot platform (benchmarked against Telegram Bot; planned for now, implementation scheduled separately): a bot account with its own token (created in the admin console, joins groups / is direct-messaged); dual message modes — webhook callback to a developer service + a long-polling SDK (intranet scenarios); developer experience benchmarked against "send and receive in a few lines of code" (official HTTP API / SDK + curl examples + local debugging tools); permission boundaries (what a bot can reach, which messages it can see; bot messages likewise enter the traceable archive); reuses the same message delivery surface and token authentication as the notification subsystem (prerequisite: the phase-one notification subsystem T4.10 lands first).
- AI: model gateway (OpenAI-compatible, routing, fallback, call audit; archived data only to local models), in-group intelligent assistant, meeting-minutes generation, intelligent search
