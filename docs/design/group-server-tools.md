> **English** | [中文](../zh/design/群服务器工具.md)

# Group Server Tools: Agent + Load Status + Remote Session (R26 Design Draft)

> Status: design draft (2026-10-04 user requirement). Positioning: the **server form** of the memex group tools framework (R25) — shared servers run agents, joining a group grants the right to view and access. Cross-references: todo R26, group tools framework (R25: authorization model/trace-leaving/credential surface directly reused), croupier ops session (plan C, heavy-ops boundary item), **cockpit (implementation reference item, user-designated)**.

## 0. Reference cockpit (implementation alignment)

cockpit has already walked through the same-type items; R26 references directly and does not rebuild:

| R26 item | Reference cockpit ready-made item | What is borrowed |
|---|---|---|
| Agent registration/heartbeat/presence (R26-1) | cockpit agent (registration/heartbeat/presence) | Registration protocol, heartbeat liveness, offline determination and cleanup semantics |
| Load status panel (R26-2) | cockpit overview/AgentTable | Status red/green light, metric set, list interaction |
| Remote session (R26-3) | cockpit guacd session (SSH/RDP/VNC) | guacd gateway form, proxy chain, protocol coverage |
| Credential injection (R26-4) | cockpit safe (GetRemoteCredential automatic injection) | Credential matching (agent/host/protocol/port), private_key priority, session parameter injection chain |
| SSH key/agent access | **persona (user-designated reference item)**: core/src/crypto (ssh_generate/ssh_import), ssh-agent | SSH key generation/import, agent-style access pattern (no manual key moving) |
| Trace-leaving | cockpit audit log | Action audit field semantics |

Differences (not copied): cockpit is an ops panel (people manage machines), memex is **group semantics** (joining a group grants permission, result card returns to the group); memex does not do recording/approval (that belongs to croupier).

## 1. Model

- **memex agent**: a lightweight resident process, installed on shared servers (web/DB/test machines etc.); registers with the memex server (owning group/asset group), heartbeat reporting.
- **Joining a group grants permission** (same as R25): joining a group = seeing that group's server list + status; leaving loses it.
- **Three types of capability**:
  1. **Status surface**: CPU/memory/disk/load/process overview, agent periodic reporting, list red/green light;
  2. **Remote session**: SSH/RDP/VNC initiate and use immediately (web-side proxy or local protocol invocation);
  3. **Action trace-leaving**: who/when/which machine/protocol/duration — enters the group log (same source as the audit surface).

## 2. Security Stance

- **Server credentials only stored server-side** (same encryption surface as R24 vault), client zero-credential; session credential is a one-time short ticket;
- **Every access leaves a trace** (optional: session recording extension slot — heavy requirement belongs to croupier, memex first leaves a trace without recording);
- High-risk actions (restart service/change config) do not enter this tool surface — memex only does "view + connect", operation-type goes through croupier approval flow.

## 3. Boundary with croupier

| | memex group server tools (R26) | croupier ops session (plan C) |
|---|---|---|
| Scenario | Shared-server shared use (dev/test machines) | Production ops (login server/dev login) |
| Permission | Granted on joining the group | Ticket + two-person review approval |
| Trace-leaving | Access record | **Full audit + recording** |
| Credential | Server-side encryption surface | Safe form + short-ticket injection |

No duplicate build: memex does not do recording/approval; croupier does not do group semantics.

## 4. Landing Items (todo R26)

- R26-1 memex agent: registration/heartbeat/load reporting (CPU/memory/disk/load), Linux first
- R26-2 group server panel: list red/green light + status detail (R25 status view instantiation)
- R26-3 remote session: SSH first (RDP/VNC after), one-time short ticket + access trace-leaving
- R26-4 credential surface: server credentials enter encrypted storage, client zero-credential
