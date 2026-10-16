> **English** | [中文](settings-consolidation.md)

# Settings Page Consolidation: A Single Settings Entry and Configuration-Source Convergence (Design 2026-10-15)

> Status: **design draft** (2026-10-15 user directive: "produce the design first; do not implement this round"). Scope: desktop settings consolidation (a single settings page) + server-address configuration-source convergence + migration compatibility. The six-item function inventory is honestly registered in todo「企业内网办公 IM 缺口盘点」§5 (verified 2026-10-15).

## 0. Current-state inventory (facts read from the code; verify first, then design)

### 0.1 Current settings entry points

The settings menu (`client/app/main_window.cpp:588-665`) has 9 items and 6 independent dialogs, unrelated to one another: Personal Profile… / Notification Preferences… / Send read receipts (checkbox) / Theme… / Brand Assets… / Connection Settings… / Shortcuts… / Away & Lock Screen… / Network & Remote… / Start at login (checkbox); the task-provider settings are hidden inside the task window (`client/app/task_dialog.cpp:454`). The rail gear pops the same menu (`main_window.cpp:590`).

### 0.2 Server address "are there really multiple copies?" — verified conclusion

**There is only one copy on disk; the entry UI is scattered across 14 places.**

- **Storage is already single-sourced**: the four keys `host/port/files_port/account` of `QSettings("memex","collab")`. The only write point = the login-collaboration dialog (`main_window.cpp:3867-3871`, entry「登录协作态…」`main_window.cpp:468`). The only exception: the approval dialog writes back `files_port` (`client/app/approval_dialog.cpp:190`) — a second write point, to be removed during consolidation.
- **Defaults are already single-sourced**: `client/app/collab_defaults.hpp` (`default_collab_host()` :15, `default_collab_port()` :19, `default_files_port()` :24, `apply_host_rules()` :34, `resolve_conn_host()` :51).
- **The entry UI is scattered (this is what needs consolidating)**: besides the login dialog, 13 tool dialogs each carry a「server address」entry box, which on open reads `settings.value("host", default_collab_host())` to prefill, is editable, and takes effect only for that session without persisting: `task_dialog.cpp:161`, `brand_settings_page.cpp:91`, `assist_dialog.cpp:247`, `office_map_dialog.cpp:229`, `report_dialog.cpp:66`, `approval_dialog.cpp:93`, `group_tools_dialog.cpp:179`, `emoji_pack_dialog.cpp`, `group_vault_dialog.cpp`, `group_memo_dialog.cpp`, `group_server_dialog.cpp`, `group_pack_dialog.cpp`, `group_ci_dialog.cpp`, `file_assistant.cpp`, `audit_dialog.cpp`.
- **「Connection Settings…」does not handle the address**: that dialog edits only the `host_overrides` domain-resolution overrides (`main_window.cpp:3886-3920`) and has no server-address field — a user cannot find an address entry point from the menu. This is the root of the "scattered" impression: changing the address forces you through the semantically unclear「登录协作态…」.

### 0.3 Full configuration-key panorama (only two QSettings stores across the whole app)

| Configuration domain | Store (org/app) | Keys | Current edit entry |
| --- | --- | --- | --- |
| Connection address | memex/collab | host, port, files_port, account | Login-collaboration dialog (the only write point) |
| Domain-resolution overrides | memex/collab | host_overrides | Connection Settings… |
| Read receipts | memex/Memex (default store) | receipts/send_read, receipts/peer/\<peer\> | Settings-menu checkbox + session right-click (`main_window.cpp:3170-3191`) |
| Watch reminders | memex/Memex | watch/remind_minutes, watch/expire_hours | **No UI** (manual INI editing only, `main_window.cpp:206-219`) |
| Screenshot shortcut | memex/Memex | shortcuts/screenshot | Shortcuts… (`main_window.cpp:2038`) |
| Theme | memex/Memex | appearance/theme_mode | Theme… (`theme.cpp:48,404`) |
| Notification preferences | memex/Memex | notify/* | Notification Preferences… (NotificationCenter::show_settings) |
| Away & lock screen | memex/Memex | away_lock/* | Away & Lock Screen… (lock_screen.hpp:16) |
| Network-segment blacklist / remote control | memex/Memex | net_blacklist/*, remote_control/* | Network & Remote… (net_guard.hpp:38-44) |
| Task-provider credentials | TaskProviderStore encrypted on disk | — | Button inside the task window (task_dialog.cpp:454) |
| Start at login | autostart .desktop file | — | Settings-menu checkbox (`main_window.cpp:3704`) |
| Brand assets | Server-side profile (not persisted locally) | — | Brand Assets… |

### 0.4 R17 initialization wizard current state (the same-source baseline with "Connection")

- Mobile is already implemented: HarmonyOS `InitPage.ets` (persists only after the PING→PONG connectivity check passes, preferences store `memex_init`, `AppState.ets:57,68-75`); Android `InitActivity.kt` + `PrefsInitStore.kt` (keys `server_host`/`server_port`, `PrefsInitStore.kt:29-30`).
- Desktop has **no** forced first-launch wizard (the direct mode can be used without collaboration, per R1); the only address-edit entry = the login-collaboration dialog.
- An existing same-source baseline: todo:452「客户端默认服务器地址……首次启动向导与默认配置同源」(the default value `server.memex.cuihairu.site`, `collab_defaults.hpp:15`).

## 1. Goals (user directive 2026-10-15)

1. A single settings page: a two-column layout with categories on the left and content on the right.
2. Categories must include at least: Connection (server address, port, connectivity check, initialization wizard — merged with R17), Appearance & Brand, Shortcuts, Task Provider, Away/Status, and Notifications & Message State (read receipts, watch reminders).
3. The server address has only one entry point; other places that reference it must not each store a copy.
4. Not one capability of the existing 5 settings pages is lost — they are merely gathered under a unified entry.
5. The R17 phone initialization wizard and the「Connection」category share the same configuration source; two separate sets are not allowed.

## 2. Target layout (category layout ruled)

**Form**: a standalone `SettingsDialog` (QDialog, about 880×560), consistent with the existing dialog ecosystem, not intruding on the main-window rail layout. Left column `QListWidget` categories (icon + name), right column `QStackedWidget` content pages; the bottom status line keeps the existing `status_` save-feedback pattern.

**Category list (six, with the expanded items = their migration source)**:

1. **Connection** — server address / port / files port / account (migrated from the login dialog, `main_window.cpp:3840-3871`) + domain-resolution overrides (migrated from「Connection Settings…」, `main_window.cpp:3886-3920`) + a **connectivity-check** button (reusing R17's same probe: PING→PONG must pass before saving is allowed, with the probe logic aligned to the mobile side's `InitPage.ets:59`) + an R17 initialization-wizard description line (guidance on the mobile wizard sharing the same source as this page).
2. **Appearance & Brand** — theme (`ThemeSettingsPage` content migrated) + start at login (checkbox migrated) + brand assets (the org-admin fetch/save form migrated, `brand_settings_page.cpp`).
3. **Shortcuts** — screenshot shortcut rebinding + conflict detection (`ShortcutSettingsDialog` content migrated, `shortcut_settings.cpp`).
4. **Task Provider** — provider selection + credentials + passphrase gate (`TaskProviderSettingsDialog` content migrated; the encrypted TaskProviderStore on disk is unchanged).
5. **Away & Status** — away password / timeout / enable (`AwayLockSettingsDialog` content migrated) + network-segment blacklist + remote-control pairing password (`NetRemoteSettingsDialog` content migrated; "Status" includes the local machine's network safety state).
6. **Notifications & Message State** — notification preferences (NotificationCenter settings panel migrated) + the global send-read-receipts switch (the menu checkbox migrated; the session-level override stays on the session right-click, `main_window.cpp:3170-3191`) + a **watch-reminder threshold/expiry UI** (new interface: `watch/remind_minutes` and `watch/expire_hours` can currently only be edited by hand in the INI — add the interface, defaults 10 minutes / 24 hours unchanged).

**Not absorbed**: Personal Profile… (a personal signature is an account profile, not a local-machine setting) keeps its independent menu item.

**Menu consolidation**: the settings menu shrinks to two items,「设置…」and「个人资料…」; the rail gear pops「设置…」(the same-source menu at `main_window.cpp:590` is repointed to the unified page).

## 3. Configuration-source convergence plan (a single entry point)

**Storage is untouched**: the two QSettings stores stay as they are (the memex/Memex default store + memex/collab). Renaming keys = a migration burden, not worth it; what is being converged is the **UI write points**, not the keys.

**Iron rule for UI write points**: each key's write point = the settings page's corresponding category, unique across the whole app.

- `host/port/files_port`: **the only write point is the Connection category**. The login-collaboration dialog drops the host/port/files_port fields and keeps only account/passphrase (the address is read from configuration and shown read-only), so「登录协作态」returns to pure login semantics; the `files_port` write-back at `approval_dialog.cpp:190` is removed (switched to reading configuration).
- The 13 tool dialogs'「server address」entry boxes: all become **read-only connection badges** (showing the resolved `host:port`) plus an「edit in settings」button that jumps to the Connection category of the settings page; nothing is persisted and no configuration is changed. Each dialog's account/passphrase fields stay — those are files-surface session authentication credentials, not configuration.
- `host_overrides`: merged into the Connection category (the original「Connection Settings…」dialog retires).
- Code-level consolidation: extend `collab_defaults.hpp` into a connection-configuration read/write wrapper (`collab_conn_host()/collab_conn_port()` etc.); reading connection configuration goes only through it, and writing only through the settings-page save path. `resolve_host_rules` stays a pure function (`test_host_override` as before).

**R17 same-source constraint (no two sets)**:

- The mobile wizard's persistence (Android `PrefsInitStore` keys `server_host/server_port`; HarmonyOS preferences `memex_init`) and desktop's `QSettings("memex","collab")` `host/port` are **semantically identical**: the same key meaning (address + port), the same defaults (the `default_collab_host()` constant as a shared source), the same validation (PING→PONG).
- If desktop's Connection category ever adds a first-launch wizard, it **must reuse memex/collab's host/port keys**; a second key name is forbidden. A future mobile settings page likewise reuses `PrefsInitStore`; starting a separate one is not allowed.
- Acceptance stance: the same configuration is visible and editable in both the desktop settings page and the mobile wizard, and a change in one shows up in the other the next time it opens (each persists on its own device, with no synchronization requirement — "same source" means the key meaning and default values share a source, not cross-device sync).

## 4. Migration compatibility

- **Existing dialog classes are not deleted**: the five pages (away/brand/net/shortcut/task_provider) keep their build and save logic as-is; the settings-page content pages directly reuse their `build_ui` and `save_settings` programmatic seams (those seams were designed for testing anyway). Short term, "the category page embeds the original dialog body", with the test seams untouched; a common widget can be extracted later in phase two as appropriate.
- **Test compatibility**: `test_mode_switch` (NetRemoteSettingsDialog and others, `test_mode_switch.cpp:810`), `test_brand_settings`, `test_task_provider_settings`, `test_theme`, `test_net_guard`, `test_watch_reminder`, `test_host_override` all stay green as before; a new `test_settings_consolidation`: the settings page opens with six categories, saving the Connection category persists memex/collab `host`, the tool dialogs' badges are read-only, and the old menu items are removed.
- **Data compatibility**: no key renames → zero migration; old INI files work directly.
- **Entry compatibility**: no shortcuts are kept for the old menu keys (the menu items are shallow anyway, so one step suffices);「登录协作态…」stays but its semantics narrow to pure login.

## 5. Phasing (this round is design only; no implementation schedule)

- **Phase one**: the settings shell + six categories embedding the existing dialog bodies; the Connection category takes over the host/port/files_port/host_overrides write points; the login dialog loses its address fields; the 13 tool dialogs switch to read-only badges.
- **Phase two**: the watch-reminder threshold UI, the connectivity-check probe, and polish on the embedded theme/notification panels.
