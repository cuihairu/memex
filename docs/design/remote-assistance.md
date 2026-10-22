> **English** | [中文](../zh/design/远程协助.md)

# Remote Assistance: Screen Viewing + Controlled-Party Authorized Operations (Protocol Surface + Media Relay)

> Status: minimal loop implemented (2026-10-08, continuing todo 「三、第二期」 entry four). The model layer (Platform-11 Security Domain, blueprint §二十七/§五) landed earlier: the three tables assist_policies／assist_sessions／assist_audits, five states requested→approved→active→closed (or denied), five granularity permission bits view/keyboard/mouse/clipboard/file_transfer, two red lines with no switch (consent = only the controlled party themselves can approve/reject/revoke at any time; audit = every state transition automatically logged), the department allow switch off by default with strict chain-wise upward traversal, and the CLI `assist` subcommand family. This round = protocol surface (FileServer HTTP) + media transport (minimal loop through server-side in-memory relay).

## 0. Scope Ruling

- **This round does**: full-chain HTTP for the session lifecycle (request → explicit confirmation → start → end) + screen-frame relay (controlled party pushes frames / requester pulls frames, server caches only the latest frame in memory) + input-event relay (requester sends / controlled party pulls and clears) + dual-role client window (initiate assistance / receive assistance) + protocol tests and dialog tests.
- **Not done (deferred, explicitly noted)**: the WebRTC P2P media foundation (the minimal loop uses server-side HTTP relay; bandwidth is controllable in the intranet scenario; direct P2P is left to the WebRTC integration round), the media surface for the clipboard/file-transfer permission bits (model-layer permission bits are ready; the route surface only wires view/mouse/keyboard), real keyboard/mouse injection on the controlled side (the minimal loop applies mouse movement via QCursor::setPos, a portable primitive; deep injection belongs to the platform layer), the full org-admin audit surface (CLI `assist audit` already exists).

## 1. Authorization and State Gates (reusing the model layer; the route layer invents no rules of its own)

- Department allow switch: `assist_policy_resolve` strict on both parties + off by default (implemented in the store layer's assist_request).
- Party-of-the-session authorization lives in the route layer: each of requester/target has their own action surface (approve/reject = the controlled party themselves; start/end = either party; push frame = controlled party; pull frame / send input = requester); the az role matrix does not take part in the party-of-the-session surface.
- Media gate: the session must be active + the corresponding bit set in granted_mask (frames = view; mouse-class input = mouse bit; key input = keyboard bit). Terminal state = immediate disconnect (controlled party's end = revocation takes effect at once), and the media slot is cleared right away (screen content is never retained).

## 2. Media Relay (server-side memory, never persisted)

- Frame slot: `{session_id → {frame_seq, frame_b64, input_events[]}}`, lock-free on the single io_context thread; the controlled party pushes frames overwriting (the seq auto-increment semantics are maintained by the client; the server stores only the latest); **screen content is never persisted** — the audit records only session metadata (the audit red line's convention is state transitions to begin with, not content).
- Input-event queue: the requester appends, the controlled party pulls and immediately clears (FIFO, no redelivery).
- A closed/denied session erases its slot immediately (after the controlled party's end, if the requester pulls again the 404 semantics = the session is terminal).

## 3. Routes (/files/assist/*)

- POST /request {target, perms}: department switch strict on both parties; self-assistance / ghost / illegal perms → 400; on success returns a ra-<hex> session id.
- POST /respond {id, approve, perms}: only the controlled party themselves; the approved set on approve ⊆ the requested set (can narrow, never expand); deny is terminal.
- POST /start {id} / POST /end {id, reason?}: party of the session; a controlled party's end while active = revocation.
- GET /sessions: items involving oneself (as requester or controlled), reverse order = continuous visibility of the process + a ledger surface.
- GET /audit?id=: a party reads the transition chain of their own session.
- POST /frame {id, jpeg_b64, seq} (controlled party) ／ GET /frame?id= (requester).
- POST /input {id, kind, x, y, key} (requester) ／ GET /input?id= (controlled party, pull-and-clear).

## 4. Client

- FilesClient: ten methods and ten signals (six lifecycle + four media).
- AssistDialog, dual roles in one window: the initiating area (controlled party's account + permission checkboxes + initiate ／ session list 〔status〕／ start/end ／ audit chain); the controlled area (pending requests + approved-set checkboxes 〔default = requested set; the server still validates ⊆〕／ approve/reject; a continuously visible indicator during sharing + end at any time = revocation; a frame-pushing timer captures the screen QScreen→JPEG; an input-pulling timer, and received mouse movements are applied via QCursor::setPos). Programmatic entry points: synthetic frame pushing (lets offscreen tests bypass real screen capture), readable permission checkboxes.
