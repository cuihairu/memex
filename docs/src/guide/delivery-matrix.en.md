> **English** | [中文](delivery-matrix.md)

# Delivery Acceptance Matrix (review eight-5)

> Iron rule of scope: **CI green ≠ deliverable on every platform**. CI runs only on
> ubuntu-latest; evidence for any other platform is written at whichever tier it actually
> reaches. This page grades the combinations of 「平台 × 通信模式 × 账号状态 × 网络状态 ×
> 权限角色」 honestly, and any report that cites platform capability takes this page as
> authoritative — tiers must not be mixed.
> Verification date: 2026-10-13. Evidence is taken only from inside the repo: CI workflow
> definitions (`.github/workflows/`), ctest test names (83 items, ctest names deduplicated),
> each end's README/BUGS records, and the dated live-run pages (`platform-adapter.md` and
> `wan-drill.md`, both recorded 2026-10-03).

## Tier definitions

| Tier | Meaning | Criterion |
| --- | --- | --- |
| ✅ Live-tested pass | An in-repo automated test or a dated live-run record directly covers the combination | ctest test name / CI workflow / documented live-run record; **state the verification level** (server logic / client UI / JVM unit test / Node logic layer — levels are not equivalent, see the notes under each table) |
| 🔧 Build verified | The platform has build artifacts (+ install/self-check smoke), but no test coverage of the functionality | nightly artifact matrix, `--self-test`, per-end build records |
| ⬛ Code exists only | Code is in the repo, with no build or run evidence for the platform (= unverified, no guessing) | No workflow or record touches it at all |
| — Not applicable | Excluded by design, not a verification tier (e.g. R18: mobile has no direct mode) | Explicitly excluded by a requirement item |

Platform column notes: **Linux x86-64 (Ubuntu)** is the only column with full evidence
(CI + ctest); **UnionTech UOS / Kylin** have no real machines (the A12 on-site checklist is
in `platform-adapter.md`), and the nightly generic x86-64 deb/rpm are merely same-architecture
artifacts never installed on the target systems; **Windows** has only the client in nightly
(the server is not built on Windows); **Android** has no CI job (local gradle records);
HarmonyOS / iOS are appended columns (the repo has other delivery surfaces, outside the
review's five-platform list).

## Table 0 · Platform × delivery baseline

| Deliverable | Linux x86-64 (Ubuntu) | Windows x64 | UnionTech UOS | Kylin | macOS arm64 | Android | HarmonyOS | iOS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Server buildable | ✅ ci+nightly | ⬛ | ⬛ | ⬛ | 🔧 nightly | — | — | — |
| Desktop client buildable | ✅ ci+nightly | 🔧 nightly | ⬛ | ⬛ | 🔧 nightly | — | — | — |
| Mobile app buildable | — | — | — | — | — | 🔧 ¹ | ⬛ ² | 🔧 ³ |
| Installer artifacts | 🔧 tar+deb/rpm | 🔧 zip+setup.exe | 🔧 ⁴ | 🔧 ⁴ | 🔧 dmg×2 | 🔧 apk(debug) | ⬛ | — |
| Post-install self-check smoke (`--self-test` server only; client = `--version`/`--smoke`) | 🔧 nightly | 🔧 nightly | ⬛ | ⬛ | 🔧 nightly | — | — | — |
| Full automated test suite (83 ctest items) | ✅ ci.yml | ⬛ | ⬛ | ⬛ | ⬛ | — | — | — |

¹ Local `assembleDebug` artifact (on the record in BUGS.md); no CI job. The occasional red
release-variant unit test = BUG-005, fixed 2026-10-10 (after triage, the test-side assertion
was fixed, and both variants closed out all green at 84/84×2).
² The ArkTS HAP compile leg has no OHOS runner — harmony.yml states plainly 「不在此伪造编译通过」;
the pure logic layer does have CI (see the table below).
³ ios.yml (macos-26): swift test + simulator build; real hardware unverified.
⁴ The nightly generic x86-64 deb/rpm share source and architecture with Ubuntu and have never
been installed on real UOS/Kylin machines — on-hardware acceptance is the on-site checklist in
`platform-adapter.md`.

## Table 1 · Platform × communication mode

| Communication mode | Linux x86-64 (Ubuntu) | Windows x64 | UnionTech UOS | Kylin | macOS arm64 | Android | HarmonyOS | iOS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Direct-mode send/receive (no login · same-subnet P2P + UDP discovery) | ✅ direct_chat/discovery/ipmsg | ⬛ | ⬛ | ⬛ | ⬛ | — ⁵ | — ⁵ | — ⁵ |
| Collaboration-mode 1:1 chat (login · relay · archival) | ✅ collab_login/collab_chat | ⬛ | ⬛ | ⬛ | ⬛ | ✅ ⁶ | ✅ ⁷ | ✅ ⁸ |
| Group chat (fan-out / announcement / disband) | ✅ group | ⬛ | ⬛ | ⬛ | ⬛ | ✅ ⁶ | ✅ ⁷ | ⬛ |
| Chat file direct transfer (peer-to-peer, not via the server) | ✅ direct_file/file_authz | ⬛ | ⬛ | ⬛ | ⬛ | — ⁶ | — ⁶ | — |
| File plane (server-side persistence · quota / instant upload) | ✅ files_api/uplink | ⬛ | ⬛ | ⬛ | ⬛ | ✅ ⁶ | ⬛ | ⬛ |
| Cross-mode interoperability (unarchived marking · archival starting point) | ✅ cross/mode_switch | ⬛ | ⬛ | ⬛ | ⬛ | — ⁵ | — ⁵ | — ⁵ |
| Three-level notification (normal / important / urgent with acknowledgement) | ✅ notify (desktop) | ⬛ | ⬛ | ⬛ | ⬛ | ✅ ⁶ | ✅ ⁷ | ⬛ |

⁵ R18: direct mode / no-login capability exists only on the desktop; mobile is entirely in
collaboration mode — excluded by design, not a verification tier.
⁶ Android: the live-test level is 95 JVM unit tests (real socket against a fake server; debug
variant), **not real-device UI end-to-end**; chat file direct transfer is a desktop-only
capability (per the Android README), and the file plane is covered by FilesClientTest.
⁷ HarmonyOS: the live-test level is pure-TS logic-layer unit tests on Node 22 (harmony.yml CI
all green, byte-level cross-verification of the wire against the desktop); **real execution
inside the HAP is unverified** (no OHOS runner).
⁸ iOS: the live-test level is 53 MemexKit core unit tests (real socket against a fake server,
ios.yml macos-26); the app UI is unverified.

## Table 2 · Platform × account state

| Account state | Linux x86-64 (Ubuntu) | Windows x64 | UnionTech UOS | Kylin | macOS arm64 | Android | HarmonyOS | iOS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| No-login direct mode (desktop only, R1) | ✅ discovery/mode_switch | ⬛ | ⬛ | ⬛ | ⬛ | — ⁵ | — ⁵ | — ⁵ |
| Normal login/logout (desktop includes the no-restart dual-mode switch, R5) | ✅ mode_switch/accounts | ⬛ | ⬛ | ⬛ | ⬛ | ✅ ⁶ R17 wizard/R18 | ✅ ⁷ | ⬛ |
| Single-point kick (KICK, R4) | ✅ accounts | ⬛ | ⬛ | ⬛ | ⬛ | ✅ ⁶ | ✅ ⁷ | ⬛ |
| New device pending approval (T3.4) | ✅ org | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |
| Device disabled (T3.3) | ✅ authz_audit | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |
| LDAP-bound account (directory decides login, 544) | ✅ ldap | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |
| LDAP shadow account (auto_provision, 544) | ✅ ldap | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |

The LDAP rows are the server-side login-decision surface (`test_ldap`, 16 legs, Linux CI):
any client login of a bound account goes through the same route, but each end has **not** been
verified separately in the LDAP scenario, so every non-Linux column is ⬛.

## Table 3 · Platform × network state

| Network state | Linux x86-64 (Ubuntu) | Windows x64 | UnionTech UOS | Kylin | macOS arm64 | Android | HarmonyOS | iOS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Same-subnet online (normal send/receive) | ✅ | ⬛ | ⬛ | ⬛ | ⬛ | ✅ ⁶ | ✅ ⁷ | ⬛ |
| Disconnect-reconnect + offline resend (msg_id dedup) | ✅ gateway/collab_resend/msg_faults | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ ⁹ | ⬛ | ⬛ |
| Server unreachable → degrade to direct mode (A11) | ✅ mode_switch (real-server-process-killed leg) | ⬛ | ⬛ | ⬛ | ⬛ | — ⁵ | — ⁵ | — ⁵ |
| Cross-subnet collaboration mode (A13 cross-VLAN live test) | ⬛ ¹⁰ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |
| Weak network (netem 80ms±20ms · 2% packet loss) | ✅ wan-drill 2026-10-03 live run | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |

⁹ Android JVM covers send/receive/ACK/dedup/KICK; the disconnect-reconnect scenario is not in
the unit tests.
¹⁰ That collaboration mode has no subnet restriction is the design decision (server relay), and
a loopback test does not represent a true cross-subnet run — the cross-VLAN on-hardware
checklist is prepared (`wan-drill.md`, section A13) but **not executed**, so every column is ⬛
rather than overstated.

## Table 4 · Platform × permission role

| Permission role | Linux x86-64 (Ubuntu) | Windows x64 | UnionTech UOS | Kylin | macOS arm64 | Android | HarmonyOS | iOS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Regular member (default-deny whitelist) | ✅ authz/files_api | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |
| Group owner (create / invite / disband / group files) | ✅ group | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |
| org-admin (back-office management · re-evaluated per request) | ✅ admin_api | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |
| auditor (archive search/export · trail) | ✅ admin_api/visibility | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |
| LDAP explicitly mapped role (zero authorization outside the table) | ✅ ldap | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ | ⬛ |

The role rows verify the server-side authorization surface (Linux CI): the client UI's role
dialogs (audit / approval / reports, etc.) have their own ctests (`audit_dialog` /
`approval_dialog` / `report_dialog`, etc.), also Linux only. Non-Linux platforms have not even
run authorization, so the whole column is ⬛.

## Summary (240 cells)

| Tier | Cells | Share |
| --- | --- | --- |
| ✅ Live-tested pass | 40 | 16.7% |
| 🔧 Build verified | 14 | 5.8% |
| ⬛ Code exists only (unverified) | 153 | 63.8% |
| — Not applicable (design exclusion) | 33 | 13.8% |

The 40 live-tested cells concentrate in the Linux column and the mobile logic layers
(Android JVM / HarmonyOS Node / iOS core); **across all 240 cells, not one piece of
live-test evidence comes from a local Windows, UOS, Kylin, or macOS machine**.

## Gap list (upgrade paths)

1. **Windows/macOS functionality**: nightly only proves "it compiles and self-checks"; every
   functional cell in Tables 1–4 is ⬛. Upgrade path = run a functional subset on the
   corresponding platform runner (collaboration send/receive + files + account states).
2. **UOS/Kylin real machines (A12)**: the three on-site checklist items in
   `platform-adapter.md` (install → direct / send-receive / files / high DPI, one X11 and one
   Wayland session, degradation notices item by item). Only after they pass can the UOS /
   Kylin columns on this page be upgraded.
3. **Cross-subnet real hardware (A13)**: the `wan-drill.md` checklist has not been executed;
   the column is currently all ⬛.
4. **Android**: no CI job (local gradle run by hand); the release-variant BUG-005 is fixed
   (2026-10-10, both variants 84/84×2 all green); disconnect-reconnect, new-device approval,
   and device disabling are not in the unit tests.
5. **HarmonyOS HAP**: the compile leg and the real-device functional surface await an OHOS
   runner / real hardware.
6. **iOS**: beyond the simulator build, the UI and real hardware are unverified.
