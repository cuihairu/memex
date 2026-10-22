> **English** | [中文](../zh/design/群工具框架.md)

# Group Tools Framework Design: CI/CD / Packaging / Config Export (R25 Design Draft)

> Status: design draft (2026-10-04 user requirement). Positioning: the **tool capability surface** built into groups — joining a group grants the capability, all operations leave a trace. Complements the bot plan: group tools = first-party built-in UI and actions, bots = developers integrating via token to process messages. Cross-references: todo R25, group knowledge sharing (R24 vault = tool credential storage surface).

## 1. Core Model

- **Group = authorization domain**: admins configure tools for the group (whitelist action list), **joining a group immediately grants the group's tool capabilities, leaving loses them** — permissions follow the group, no independent authorization system is built.
- **Tool = plugin-style configuration**: one type of tool = several actions (action) + several views (status/list). Built-in first batch: CI/CD (view status/trigger build), packaging tool (one-click artifact), config export.
- **Action semantics**: client clicks a button → **server proxies the call** to external systems (GitHub Actions/API/internal services) → result returns to the group (message card + tool panel status).

## 2. Trace-leaving (iron-rule alignment)

Every tool action leaves a trace: **who/when/which tool/what action/parameters/result** — enters the group log (same source as the audit surface). A trigger like "click run" must be traceable back to a person (user's explicit ask: on click-down you can see who clicked run).

## 3. Security Stance

- **External credentials only stored server-side** (CI token/interface key → reuse R24 vault encryption surface), the client sees only buttons, not keys;
- Tool action whitelist system (tools only expose declared actions, no free-parameter escape); parameter validation is server-side;
- Destructive actions (cancel build/delete artifact) can be configured for secondary confirmation.

## 4. Relationship to Existing Surfaces

| Surface | What it manages | Relationship |
|---|---|---|
| Group tools (R25) | First-party tool actions + status views | Authorized on joining the group |
| Bot plan (bot) | Developer integration to process messages | Complementary, no overlap |
| Group vault (R24) | Shared credentials | Tool credentials use the same encryption surface |
| Three-level push | Notification | Build complete/fail → tool result card can go through push |

## 5. Landing Items (todo R25)

- R25-1 group tools framework: tool configuration (whitelist actions), authorize-on-join/lose-on-leave, action trace-leaving, server-side proxy call skeleton
- R25-2 CI/CD tool: pipeline status view + trigger run/packaging (who triggered leaves a trace), result card
- R25-3 packaging tool + config export tool (first-batch action-type examples)
- R25-4 credential surface: tool external credentials enter encrypted storage, client zero-credential, destructive action secondary confirmation
