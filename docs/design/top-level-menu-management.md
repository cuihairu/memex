> **English** | [中文](../zh/design/一级菜单管理.md)

# Top-Level Menu Management (Interface Settings)

> Status: design draft — not implemented (2026-10-04 user decision, design first; the code surface has no such config, it will be wired with the increment when landing). Positioning: the show/hide configuration surface for the left-side top-level menu. Cross-references: personal task list and external tool integration (UI positioning, task list = top-level menu).

## Rules

- **Top-level menu show/hide is configurable**: the settings page opens "Interface Settings · Top-Level Menu Management", and per-item switches decide which top-level menus are displayed.
- **Chat (session) cannot be turned off** — it is the core surface, the switch is locked to always-on; the rest (contacts/task list/files/group tools/settings etc.) can all be turned off.
- All on by default; turning off only affects entry show/hide, functionality and data are untouched (turning back on restores).
- Unified semantics: desktop left bar = mobile top-level Tab, driven by the same config; ordering follows user config (chat is always first).
