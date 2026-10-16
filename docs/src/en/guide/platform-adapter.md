> **English** | [中文](/guide/platform-adapter)

# Domestic-OS Adaptation Verification (T5.1)

> Corresponds to acceptance items A12 (domestic-OS coverage) and A21 (media base).
> Verification date: 2026-10-03.

## Verification runnable in the current environment (done)

| Item | Environment | Method | Result |
| --- | --- | --- | --- |
| Message send/receive | Ubuntu 24.04 + offscreen (CI scope, `QT_QPA_PLATFORM=offscreen`) | `ctest -R "direct_chat\|collab_chat"` | All green |
| File transfer | Same as above | `ctest -R direct_file` | All green |
| UI launch and display | Ubuntu 24.04 + Xvfb (local walkthrough; CI has no Xvfb) | `xvfb-run ./build/client/memex_client` stayed up 15s without a crash, direct engine started listening | Pass |
| Screenshot capture step | Ubuntu 24.04 + offscreen | T4.4 screenshot and annotation have landed (offscreen verification in `client/tests/test_screenshot.cpp`); the multi-monitor + high-DPI live test required by A17 and the degradation notice on unsupported environments still need checking on the target environment | Partial (offscreen passes; real hardware pending) |
| Windows/UOS/Kylin media base (WebRTC) | — | Phase-2 item, not in phase-1 scope now | Not started |

## X11 / Wayland degradation strategy (code scope)

- The Qt xcb platform plugin is the main path; under a Wayland session `QT_QPA_PLATFORM` can
  force `xcb` (XWayland), and when the platform is found unreachable Qt falls back
  automatically instead of crashing — if the notice wording needs to be converged, see the
  notice channel of T4.7 system integration.
- Screenshot capture (T4.4) must degrade with an explicit notice in an unsupported
  environment: the button is greyed out and the reason stated; silent failure is not allowed.

## Pending on-site checks (needs real UnionTech UOS / Kylin machines)

A12's literal requirement is live testing on UnionTech UOS and Kylin terminals — this sandbox
has no such machines, so the checklist below is left for on-site testing; after it passes,
this page's 「待实机」 markers get cleared:

1. UOS / Kylin desktop client: run the installer → direct discovery / send-receive →
   files → window display scaling (high DPI);
2. one run each of a Wayland session and an X11 session on the same machine;
3. for the unsupported items (screenshot, tray, notification), confirm item by item that the
   degradation notice appears and nothing crashes.
