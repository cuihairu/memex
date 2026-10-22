> **English** | [中文](../zh/design/审批与日报周报.md)

# Approval (Form + Workflow Engine) and Daily/Weekly Reports: Phase-2 Feature Detail Spec

> Status: the §1 Approval and §2 Daily/Weekly Reports minimal loops have both shipped (2026-10-08; progress note see todo Phase-2 entry one). Three design-vs-implementation differences: ① self-review rejection is pinned into the deny layer by the ExplicitDeny rule 「approval-self-decide」 (deny takes precedence over every allow — the allow layer would otherwise be pre-empted by the personal-owner rule that does not filter on action; the very first version fell into this, caught by the test's self-review 403 leg); ② approval notifications are shipped = local 30-second polling diff while the window is open (same as R27-1: newly pending decisions remind the decider, finalized decisions remind the applicant, first connect stays silent without bombing existing items, one's own withdrawal never reminds oneself; server push / notification-center persistence left for later); ③ manual weekly-report backfill is shipped = a single row written into the reports table keyed by the Monday date of the chosen week (same table as daily reports, same upsert semantics: a repeated submission for the same (author, Monday) = update, updated_ms recorded; zero server-side change; no separate week dimension — the aggregated view is visible from the same source). Cross-references: Platform-3 Organization (reporting line = approver determination and daily-report visibility data plane), Platform-1 Authorization Service (authorization decisions uniformly go through az), permission model (full trail recording), R27 Personal Task List (client-window isomorphic precedent).

## 0. Scope Ruling

Phase-2 entry one contains four blocks: Approval / Schedule & To-do / Announcement & Daily-Weekly Report / Group Tools (polls / chain-reply / group tasks). This spec details the **Approval** and **Daily/Weekly Report** blocks; Schedule & To-do is already covered in its main part by R27-1 (self-created / assigned / reminded; calendar view closed on 2026-10-10 = task-list window month calendar with due-date markers + click-day filtering); polls / chain-reply will be detailed separately under the group-tools framework.

## 1. Approval: Minimal Loop (Leave Approval First)

- **Form**: Phase 1 starts from the fixed template 「请假」's three fields (type: annual leave / personal leave / sick leave / compensatory leave, start-end time, reason); the generic form engine (custom fields / template management) is deferred, as explicitly noted.
- **Approver determination** (model-backed, not invented): **direct manager takes priority** (Platform-3 org_reporting_lines authoritative table lookup); when there is no direct manager, the org-admin base role is the fallback (effective_roles queried and decided on the spot; the time window naturally supports delegation). Transfer / co-sign / multi-level flows are deferred.
- **Flow**: pending→approved｜rejected｜withdrawn four states fixed; only pending can be decided (repeat decisions are rejected); withdrawal = applicant-exclusive and only from pending.
- **Authorization**: az verb `approval:decide` (the ExplicitDeny 「approval-self-decide」 self-review red line is pinned into the deny layer; ExplicitAllow: the applicant's direct manager is found on a live query, or effective_roles contains org-admin and the applicant has no direct manager); unmatched rules default-deny, nothing invented.
- **Trail**: approvals ledger with all fields (applicant / type / content / status / approver / opinion / application and decision timestamps); decisions are never edited or deleted; withdrawal returns to status without erasing the trail.
- **Surface**: server-side five routes under /files/approvals (POST create / GET my-applied + pending-for-me as a double array / POST decide / POST withdraw); client ApprovalDialog (isomorphic to TaskDialog, independent file-surface session: initiation form + double list + approve/reject/withdraw), grouped under the main window's 「任务」 menu.
- **Notification**: application submission and decision finalization go through the notification center (shipped: local 30-second polling diff while the window is open, same as R27-1 — first connect silently absorbs existing items, newly pending decisions remind the decider, pending→terminal state reminds the applicant, one's own withdrawal never reminds oneself, same change reminds only once; server push is deferred to notification-center persistence).

## 2. Daily/Weekly Report: Minimal Loop (Personal Daily Report First)

- **Write**: personal daily report (three free-text sections: what I did today / tomorrow's plan / blockers; template is placeholder hints only); weekly report = daily-report aggregated view (display filtered by week) + manual backfill (implementation difference: backfill writes a single row into the reports table keyed by the Monday date of the chosen week, same table and same upsert semantics as daily reports, no separate week dimension).
- **Read**: all of your own; **direct managers can read their subordinates' daily reports** (same Platform-3 data plane, az verb `report:read`, unmatched default-deny); department aggregate view deferred.
- **Trail**: reports ledger (author / date / content / update time); repeated submission on the same day = update (keep updated_ms, no revision history — same as R23-3 personal memo convention).

## 3. Scheduling

- Approval minimal loop first (store + routes + authorization + Dialog + tests);
- Daily/Weekly Report next (isomorphic small increment);
- Generic form engine / transfer / co-sign / department aggregate / group-announcement linkage scheduled later per usage feedback.
