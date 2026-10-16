> **English** | [中文](/guide/requirements)

# Requirements and Acceptance

> R entries = requirements and acceptance criteria; A entries = phase-one acceptance standards. The task-level breakdown (T0–T6, at acceptable grain) is in the repository root's [todo.md](https://github.com/cuihairu/memex/blob/main/todo.md) (outside the main line there is also the R23–R27 appended batch).

## The traceability principle (iron rule)

**The only purpose of connecting to the server is traceability.** The following features are permanently excluded — not scheduled, no interface reserved, not offered as a configurable option:

- Secret chat / burn-after-reading: no conversation form of "automatically disappears after sending" exists;
- Recall erasing the archive: recall only affects the client display; the server keeps the original text and records the recall event (who, when, which message was recalled);
- One-sided deletion of the archive on the terminal side: clearing the local cache does not affect the server records; the UI distinguishes「本地缓存」from「云端归档」;
- A traceability switch for users: archiving policy is only issued by the administration side, per department.

Derived rules: archiving is judged in units of **messages** (not in units of sessions); the only traceability gap of cross-state sessions is explicitly exposed with the「未归档」label and cannot be dismissed; media content is not stored to disk, but the metadata recording "this communication happened" must be archived.

## Requirements list (R1–R19)

| ID | Requirement | Priority | Acceptance criteria | Phase |
| --- | --- | --- | --- | --- |
| R1 | Usable without login, no configuration needed | Required | After installation, with no address filled in and no registration, terminals on the same segment are automatically discovered and messages can be sent and received | 1 |
| R2 | Direct-mode messages do not pass through the server | Required | Peer-to-peer transfer; the server keeps no records at all; verifiable by packet capture | 1 |
| R3 | Centralized retention of chat records after login | Required | Collaboration-mode messages are uniformly stored on the server; administrators can search by person / time / keyword; terminal deletion does not affect retention; no feature exists that lets the sender eliminate archived content | 1 |
| R4 | Login on different machines and multi-device control | Required | An account can log in on different computers; on the desktop only one instance of the same account is online — a repeated login kicks out the previous one and it receives a notice; each login records the device type and identifier | 1 |
| R5 | Both states integrated into one client | Required | A single installer; not logged in = direct mode, switching to collaboration mode after login; switching requires no restart and loses no local history | 1 |
| R6 | LAN file sharing | Required | Directory-level sharing with permission differentiation; file bytes do not pass over the public network | 1 |
| R7 | Archive scope identifiable | Required | Cross-state sessions are clearly labeled「未归档」in the UI and the label cannot be dismissed; the archive start point is traceable | 1 |
| R8 | Domestic operating systems | Important | At least cover UnionTech UOS and Kylin Linux, with messaging / files / UI display all working normally (currently only verified in an Ubuntu 24.04 sandbox; real-device testing on UOS / Kylin still to be added — the on-site list is in [平台适配](/en/guide/platform-adapter)) | 1 |
| R9 | Cross-segment and multi-region, large files | Important | Cross-segment and cross-region communication is carried by the collaboration mode; direct mode is limited to this segment; whole-directory sending and resumable transfer supported | 1 |
| R10 | Functional scope references 企业微信 | Important | Accepted item by item against the phase-two list; no requirement to achieve it all at once | 2+ |
| R11 | Intranet AI capabilities | Planned | In-group assistant, meeting minutes, requirement organization; model deployed on the intranet; archived data is only sent to local models | 3 |
| R12 | Server high availability | Important | Direct mode can still communicate when the server fails; a backup and recovery plan exists | 1 |
| R13 | Communication experience | Important | Built-in screenshot and annotation sent directly; custom emoji pack import; frequent contacts pinned with quick lookup | 1 |
| R14 | Organizational structure with reporting relations and visibility | Important | Member profiles show the direct superior; directory visibility scope set per department; sensitive fields can be hidden | 1 |
| R15 | Office location map | Planned | Floor plans locate workstations; searching a name highlights it on the map; visibility limited by campus floor | 2 |
| R16 | Remote assistance | Planned | Request remote view / control; the controlled party grants explicit authorization, the process stays continuously visible, the whole session is traced; default is IT operations only (current media goes over the file-surface HTTP relay; the WebRTC media foundation is not implemented and deferred) | 2 |
| R17 | First mobile connection must initialize the server address (appended) | Required | On first mobile launch the initialization wizard is entered; the first step is domain / server address setup, and only after filling it in and passing the connectivity check can the main interface be entered; before initialization no chat-capable interface appears | 1 |
| R18 | No anonymous chat on mobile (appended) | Required | Direct mode / login-free capability exists only on desktop; mobile is entirely collaboration mode; no「免登录体验」/「附近设备直连」-style entry appears in the mobile UI | 1 |
| R19 | Client theme switching (appended) | Important | Light / dark dual themes: follow the system + manual switching in settings, the choice is persisted; colors are tokenized with a multi-theme extension slot reserved, brand orange as the primary color | 1 (after main line) |

## Phase-one acceptance standards (A1–A23)

| ID | Acceptance item | Standard |
| --- | --- | --- |
| A1 | Usable without login or configuration | On a new terminal, after installation with no address filled in and no account entered, terminals on the same segment can be discovered and a conversation started |
| A2 | Direct mode does not pass through the server | Packet capture verifies that direct-mode messages generate no server traffic; the server keeps no related records at all |
| A3 | Mode switching | After entering the server address and logging in, switching to collaboration mode works normally without a restart, and local history is preserved and merged normally |
| A4 | Multi-device login control | After the same account logs in on a second computer, the first is taken offline and receives a notice; desktop and mobile can be online simultaneously |
| A5 | Login record completeness | The console can query an account's full login records, including device type, device name, time, and source |
| A6 | Archive completeness | After a collaboration-mode session's local records are deleted on the terminal, or after the sender recalls a message, the administrator can still search out the original message content; the recall action itself (who, when, which message was recalled) is also queryable |
| A7 | Cross-state session labeling | Sessions with not-logged-in terminals show the「未归档」marker, and the marker cannot be dismissed |
| A8 | Archive start accuracy | For a session that turned from cross-state into collaboration mode, the archive start point shown in the console matches the actual login time |
| A9 | Policy switches take effect | After turning off「允许与未登录设备通信」, cross-state sessions cannot be established; after turning off「允许免登录使用」, a not-logged-in client cannot enter the main interface |
| A10 | File transfer and sharing | File and folder transfer completes, resumable transfer works; shared directories are accessed per permissions and over-privileged access is denied |
| A11 | Server degradation behavior | When the server is unavailable, the client shows a clear notice and falls back to direct mode; same-segment messaging and file transfer remain usable |
| A12 | Domestic system coverage | On UnionTech UOS and Kylin terminals, messaging, file transfer, and UI display all work normally (real-device testing still to be added — currently only verified in an Ubuntu 24.04 sandbox; the on-site list is in [平台适配](/en/guide/platform-adapter)) |
| A13 | Cross-segment communication | Collaboration-mode messages between terminals on different VLANs send and receive normally; direct mode is limited to this segment with no cross-segment requirement |
| A14 | Server recovery | A database recovery drill completed per the plan, verifying that the process is executable and the data is complete |
| A15 | Multi-region access | Remote regions access the same server over dedicated lines, with login, messaging, organizational structure, and history retrieval all working normally |
| A16 | Disconnection archiving rule | While the server is unreachable, both parties' collaboration-mode messages are held locally; after the connection is restored they are re-sent with de-duplication by message identifier, leaving the archive with no duplicates and no gaps |
| A17 | Screenshot and annotation | The screenshot area is correct in multi-monitor and high-DPI environments; doodle, arrow, and mosaic all work; after confirming, sent directly into the conversation |
| A18 | Emoji and frequent contacts | A custom emoji pack can be imported and sent in a conversation; frequent contacts are fully preserved after logging in on a different machine |
| A19 | Reporting relations | The direct superior is viewable in member profiles; the superior chain can be traversed level by level upward; bulk import takes effect and can validate error entries |
| A20 | Directory visibility | Hidden departments and members do not appear in the directory and cannot be searched; restricted departments can only see their own department's directory |
| A21 | Media foundation availability | Screen capture for the screenshot feature works on target terminals (Windows, UOS, Kylin), with a clear degradation notice for unsupported environments; verification that a WebRTC connection can be established is **deferred** (the WebRTC media foundation is not introduced; current remote-assistance media goes over the server-side HTTP relay, see R16) |
| A22 | Component license list | A complete third-party component list is provided at delivery and verified to contain no strongly infectious license components such as AGPL / SSPL; exceptions remain deployed as separate processes with verification records kept (2026-10-13 re-verification: nine components in the vcpkg manifest — asio / protobuf / nlohmann-json / sqlite3 / openssl / aws-sdk-cpp:s3 / qtbase / drogon / crashpad, of which drogon is still to be registered in `third_party/组件清单.md`; verification records are in [合规与命名一致性](/en/guide/compliance)) |
| A23 | System identifier consistency | The installer name, Windows service name, database and log prefixes, and intranet domain all use the Memex identity |

The item-by-item mapping of tasks to acceptance is in the「验收」line of every task in [todo.md](https://github.com/cuihairu/memex/blob/main/todo.md).
