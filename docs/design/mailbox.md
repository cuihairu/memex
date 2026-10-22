> **English** | [中文](../zh/design/信箱.md)

# Mailbox: Unified Inbox (Design Draft, Not Implemented)

> Status: design draft (2026-10-12 user decision "build per this draft"), this round is design only, no code changes — implementation will be scheduled after the user has reviewed it. In one sentence: **one entry collects three types of items (warning + notification + announcement)**.
> Use only established vocabulary (mailbox/warning/notification/announcement/read/receipt), do not coin new concepts.

## I. One Entry, Three Types of Items

| Category | What it collects | Source (reuse, no new build) | Reminder and status |
|---|---|---|---|
| **Warning** | Server-side events such as health/disk/backup failure/login anomaly; anomalies reported by bots via webhook | admin event source (P3 wired, see §V) + webhook entry | Strong reminder + persistent unread red dot + **can be marked handled** (handling action and handler leave a trace) |
| **Notification** | @me, task due, approval result, shared change | Notification center (task reminder 30s polling for due todos), approval notification via notification center (todo approval line 2026-10-08 first entry), @me and session surface existing semantics | Strong reminder + receipt **reminder only once**, follows existing semantics (T4.10 notification subsystem three-level tiering + personal notification preference notify_prefs) |
| **Announcement** | Department or all-hands broadcast (i.e. the announcement in R24 group knowledge sharing) | R24 announcement (group announcement R24-1 closed, department/all-hands broadcast form is its extension) | Weak reminder + **read-confirmation trace-leaving** (who/when confirmed is persisted and queryable) |

The three types of items share one inbox view; the item body stays in its respective source, the mailbox only manages "entering the box, unread, read, handled" (§II boundary ①).

## II. Three Hard Boundaries

1. **The mailbox = view + read status, not a new channel.** Reuse existing sources such as the notification center, webhook, admin event source, R24 announcement, etc.; **building a third push link is forbidden** — the mailbox surface only adds "item status" (unread/read/handled, per account per item), does not move the data body, does not copy message content.
2. **"Clear/mark read" only touches mailbox status, never touches the archive.** The trace-leaving principle iron rule (todo §zero) and the R3 acceptance stance (provide no function by which the sender can erase archived content) have the highest priority: deleting/clearing in the mailbox = removing from my view + status trace-leaving; message archive rows, announcement rows, warning ledger rows are all untouched; any mailbox operation must not touch the archive.
3. **Three surfaces same source.** Desktop/phone/admin read the same server-side item status; phone banner taps into the same mailbox; **the single source of unread count is the server**, the three surfaces display the same count, no two sets of unread counts are allowed.

## III. Layout Finalized (user decision 2026-10-12)

Default **one inbox + type filter** (warning/notification/announcement three filter tabs), **not split into "related to me / global broadcast" two columns** — splitting into two columns adds an entry on the phone, violating the three-surface-same-source minimal-entry stance.

## IV. Phasing and Testable Acceptance

| Phase | Content | Testable acceptance |
|---|---|---|
| **P1** | Inbox view + unread count + mark read: server-side item status surface (per account per item unread/read), desktop inbox view + type filter | One item created for each of the three types all enter the box; unread count is correct across restart (server is authoritative); after marking read the count decreases and archive row count is unchanged (R3 assertion); warnings can be marked handled and the handler leaves a trace |
| **P2** | Announcement read-confirmation trace-leaving + notification receipt merged with existing reminder strategy: announcement confirmation (who/when) persisted and queryable; receipt reminder-only-once semantics merged into T4.10 tiering | The same notification refreshed repeatedly does not re-pop (reminder-only-once assertion); announcement read-confirmation trace-leaving is queryable by person/by item; reminder behavior is consistent with notify_prefs existing switches (no second set of switches) |
| **P3** | Wire up admin system warning source: server health/disk/backup failure/login anomaly enter the warning category; webhook bot reports enter warnings | Create disk-full/backup-failure events → warning enters the box + red dot persists until handled; login anomaly (consecutive password mismatch) enters warnings; non-admins by default cannot see system details (decision ② semantics) |

## V. References to Existing Components (per-item provenance)

- **Notification center**: `client/app/notify_center.hpp` (task reminder 30s polling for due todos → IMPORTANT strong reminder; approval notification via notification center — todo approval line 2026-10-08 first entry); window-close background resident 30s polling persistence (docs/src/guide/prototypes.md task reminder section).
- **Reminder strategy**: T4.10 notification subsystem (todo first-phase task breakdown) — three-level urgency tiered push (normal = in-app session message, important = desktop notification strong reminder, urgent = pinned popup requiring confirmation of receipt) + personal notification preference `client/app/notify_prefs` (whether to popup per level, do-not-disturb hours). The mailbox follows the same tiering and preferences, does not coin a new reminder vocabulary.
- **webhook**: T4.10 webhook integration (token auth, per-group/per-person independent webhook, JSON payload = target/title/content/urgency/optional jump); server-side `server/src/webhook.cpp` (`--webhook-port 24361` port). Bot reports enter the warning category via this entry (P3), no new reporting surface is built.
- **R24 announcement**: todo R24 section (design = docs/design/群组知识共享.md), group announcement R24-1 closed; the mailbox announcement category collects its department/all-hands broadcast form (extension follows with P2).
- **admin web-ification kickoff**: todo "admin management console" section (kickoff 2026-10-11) — the warning category's event source and handling actions align with admin web-ification (P3).
- **Trace-leaving iron rule**: todo §zero trace-leaving principle + R3 acceptance stance (the basis for this draft §II boundary ②).
- **Phone**: iOS line NotificationManager three levels (normal in-app/important banner + sound/urgent banner + sound + requires-confirmation category, todo iOS line 2026-10-04) — the mailbox entering the phone follows the same tiering, no separate levels are set.

## VI. Pending User Decision

1. **Whether announcements are mandatory all-hands confirmation** — read-confirmation trace-leaving is record-keeping or a sending-surface strong constraint (whether unconfirmed persistently nags);
2. **Whether warnings allow non-admins to see system details** — default admin-only visible, or all-hands visible but details (disk path/log excerpt) desensitized;
3. **Whether warnings push to the phone banner** — or only desktop + admin visible (phone only receives notification and announcement categories).
