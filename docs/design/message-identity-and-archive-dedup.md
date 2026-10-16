> **English** | [中文](消息标识与归档去重.md)

# Message Identity and Archive Dedup Design (BUG-007 Whole-System Design Spec)

> Status: **all shipped** — batches 1+3 on 2026-10-10, batch 2 (desktop) on 2026-10-11 (the fix record is logged under BUG-007 in BUGS.md; this spec, drafted 2026-10-10, was BUG-007's design deliverable for "registered but not fixed, requires a whole-system design"). Cross-references: BUG-007 in BUGS.md (the original wording of the symptom and localization), T2.2 (message identity dedup), T2.3 (archive), T2.5 (broken-connection resend), Platform-9 (Sync State and seq continuation).

## 0. Root Cause and the Full Set of Collision Vectors

The current msg_id = `sha256(from + ":" + seq)` (two places in session.cpp: TEXT/NUDGE), while (from, seq) does not uniquely identify a message. Four collision vectors:

| # | Vector | Status |
| --- | --- | --- |
| V1 | Same account re-logins on the same device, seq resets per connection (Android／iOS／HarmonyOS seqGen are all session instance fields) | Confirmed by two 2026-10-09 walkthrough recordings (archive swallowing rows) |
| V2 | Same account coexisting across devices (T2.1 cross-type coexistence: desktop + phone online at the same time); each device counts seq independently, so the same seq collides | Not erupted; same root cause |
| V3 | A new device / data-wiped first login starts seq at 1, colliding with any historical message of the same account (desktop Platform-9 only guarantees same-device continuation; a fresh install has no history to continue) | Not erupted; same root cause |
| V4 | seq wraparound | A theoretical uint64 case, not counted |

Consequence chain: the server-side archive `INSERT OR IGNORE` silently swallows the row (audit/roaming/retrieval become distorted; the trail principle is damaged) + the sender's ACK comes back as normal (the illusion of acceptance); **the delivery surface is unaffected** (an online delivery does not go through archive dedup; a consumed offline slot can be re-enqueued). Desktop has another layer: the local DB's `UNIQUE(from_id, seq)` (local_store.cpp:72) makes the receiver side likewise unable to see a new message locally when a seq is reused — the server has archived it, the ACK has returned, but this client's list is missing the row forever.

## 1. Design-Surface Facts (code-verified, collected 2026-10-10)

- **Three msg_id generation points**: TEXT (session.cpp:201) and NUDGE (session.cpp:659) = sha256(from:seq) (the collision surface); NOTICE (webhook.cpp:477) = sha256(sender:target:ts:random salt) (server-initiated, inherently cannot collide, not on the collision surface).
- **The sender side depends on the derived formula**: the desktop client derives the same-formula msg_id locally the moment it sends (collab_engine derive_msg_id, requirement batch ⑦); delivery/read receipts, recall (recall.msg_id), and read reports all hit local rows by it.
- **Resend idempotency is an incidental property of the current derived formula**: desktop T2.5 resend rebuilds the frame with the original seq and original ts_ms (flush_pending_sends); the whole chain depends on "same (from,seq) → same msg_id → INSERT OR IGNORE idempotency"; any change must preserve that property.
- **Receiver-side dedup keys have already forked**: the desktop local DB has the dual key UNIQUE(from_id,seq) + a msg_id partial unique index; Android／iOS／HarmonyOS dedup by msg_id.
- **Two ACK directions**: the sender's acceptance ACK(seq) (returned unconditionally); the receiver's ACK(msg_id) clears offline_messages (UNIQUE(msg_id,to_account), the blob stores the final frame).
- **seq lifecycle**: desktop = continues from the local DB after login (next_local_seq, Platform-9); Android (ChatSession.kt:81) / iOS (ChatSession.swift:49) / HarmonyOS (session.ts:65) = resets per connection — V1's principal culprit.
- **The messages table has no seq column**, msg_id is UNIQUE; store_message's bool return value is ignored at both places in session.cpp (which is the defect itself).

## 2. Direction Trade-off: Why Neither Intuitive Fix Is Complete

**Pure A (the server always issues a unique msg_id, with a monotonic component or a timestamp)**: the server cannot distinguish "a resend of the same (from,seq)" from "a new message reusing a dead seq" — deduping by (from,seq) swallows V1/V2/V3 as today (current state); always issuing a new id means desktop resend archives an extra row and redelivers once on every resend (violating T2.5's acceptance "archive with no duplicates and no losses"), and receiver-side dedup by msg_id is likewise defeated. **A alone is not viable.**

**Pure B (client-side seq persisted across connections)**: closes V1, but V3 (a fresh install has no history to continue) and V2 (each device counts its own) cannot be closed inherently — unless the server issues numbers centrally (a major protocol change). B only reduces frequency; it is not a root fix.

**Conclusion: the root cause is "the server treats (from,seq), which can repeat, as a unique key and stays silent when it collides." A complete fix must give the server a means of discrimination on a key collision — content comparison is the criterion the server already has and the only reliable one.**

## 3. Chosen Scheme: Content Comparison on Colliding IDs to Disambiguate + Re-hash with a New Salt (server-autonomous and complete, zero schema migration)

When TEXT/NUDGE are archived (both places in session.cpp changed identically):

1. Compute `msg_id = sha256(from:seq)` as usual, call store_message and **check the return value**; on false, first SELECT the existing row by msg_id to distinguish "key collision" from "DB error" (a DB error keeps today's behavior and is accepted as normal; archive failures have their own event ledger, out of scope here);
2. On a key collision, compare the existing row's (to_account, type, text, ts_ms) against the incoming frame:
   - **All identical → genuine resend / timeout re-send**: keep today's whole chain (offline queue OR-IGNORE without re-enqueueing, online delivery as normal, ACK(seq) returned as normal) — the resend-idempotency property is preserved verbatim, zero changes to the desktop resend chain;
   - **Any difference → a new message reusing a dead seq (this defect's case)**: re-hash with a new salt, `msg_id = sha256(from:to:ts_ms:random salt)`, then re-run store_message + queue_offline + online delivery with the new id; ACK(seq) is returned unchanged;
3. Group messages go through the same path (to=group:N, the same archive write); no branch needed.

Reliability of the criterion: a genuine resend must have identical content and identical ts — the desktop resend rebuilds the frame keeping the original ts_ms, and a timeout re-send (delivery_timer_ timing out into a resend) comes from the same PendingSend source; "same sender + same seq + same millisecond + same recipient + same text but in fact a new message" is engineeringly indistinguishable and need not be distinguished — to any observable surface it is the same message.

### 3.1 Ruling on the Re-hash Derivation: Timestamp + Random Salt (not a server-side monotonic component)

Choose **timestamp + random salt** (the same formula as NOTICE, the webhook.cpp:477 precedent); four reasons:

- **Consistency within the domain**: the server already has one ts+salt derivation chain (the notification surface); no second derivation formula is created;
- **Zero persistent state**: a monotonic component needs an independent counter row + atomic UPSERT + recovery across restarts; timestamp + salt is stateless;
- **Nobody consumes monotonicity**: nowhere in the DB is there a path that sorts by msg_id (archive sorts by id/ts_ms, the offline queue by id, receipt chains by exact match) — monotonicity is a zero-benefit constraint;
- **Collision probability**: sha256(millisecond timestamp + 64-bit salt) is engineeringly zero.

### 3.2 Four-Surface Coordination

- **ACK**: the sender's acceptance ACK(seq) is unchanged; the Ack message **gains an optional msg_id field** (proto additions only, backward compatible) returning the final id, so the desktop sender-side row's msg_id can be corrected (§4.3); old clients ignore the new field with no loss.
- **Dedup by msg_id (receiver side)**: Android／iOS／HarmonyOS are inherently compatible (the re-hashed id is a new value and does not collide); in the desktop local dual keys, (from_id,seq) will block a re-hashed row — the accompanying fallback is in §4.2.
- **Archive**: collision with differing content → both rows exist (the old row keeps its id, the new row gets a new id, each with its own message_events event chain, not polluting each other); collision with identical content → idempotent single row (current state). Retrieval/export/audit: zero changes.
- **Redelivery**: the offline_messages blob stores the final frame (including the final id), so redelivery is self-consistent; the receiver's ACK(msg_id) queue clearing is unchanged; T2.5 resend hits the content comparison → idempotent as before.

## 4. Client-Side Accompanying Work in the Same Batch

### 4.1 Mobile-Side seq Persistence (answering "same batch?": yes)

Align Android (per-account on-disk seqGen, written the moment a number is issued, the same mechanism as PrefsInitStore) / iOS / HarmonyOS to the desktop's existing Platform-9 behavior. The value is not in correctness (§3 is already server-autonomous and complete) but in **degrading the re-hash path into a fallback**: the normal flow keeps the sha256(from:seq) derivation, so the sender side's existing dependencies on the derivation (receipt/recall hits) do not pass through the ACK correction path for the great majority of messages.

### 4.2 Desktop Receiver-Side Fallback for (from_id,seq) Interception

Before handle_text persists, check the (from_id,seq) slot: if it is occupied and the existing row's msg_id ≠ the incoming frame's msg_id (= a re-hashed new message blocked locally) → re-store using next_local_seq(from) — handle_notice already has the same precedent for seq=0 notifications (collab_engine.cpp:912-915); copy it as-is; if msg_id is identical, dedup as before (a genuine re-delivery).

### 4.3 Desktop ACK Consumption

handle_ack receiving a non-empty Ack.msg_id → update the local row's msg_id (new store setter); afterwards delivery/read receipts and recall hit by the new id. Old desktop versions do not consume it → recall/receipts miss re-hashed messages (the server finds no such id, silently no-ops, leaves no dirty state); the residual during the upgrade window is accepted honestly.

## 5. Phased Rollout Order (after authorization) and Compatibility

1. **Batch 1 · server** (autonomous and complete): the return-value check + comparison + re-hash at both places in session.cpp; Ack.msg_id returned; §6 server legs. **Zero migration** for old DBs (zero schema changes to the messages table; the comparison SELECTs on demand, no backfill, no rebuild); old clients are fully compatible (changing msg_id values does not affect any existing consumer — the receiver stores the frame's original value and there is no derivation-formula validation). ✅ Landed 2026-10-10 (test_msg_dedupe ①-⑤).
2. **Batch 2 · desktop**: §4.2 fallback + §4.3 consumption. ✅ Landed 2026-10-11 (test_collab_resend ⑥⑦ + pure library-layer legs).
3. **Batch 3 · mobile**: §4.1 seq persistence (Android→iOS→HarmonyOS). ✅ Landed 2026-10-10 in the same batch as batch 1 (seq ledgers on all three platforms + per-platform continuation legs).

Mixed old/new coexistence: the server change is transparent to all existing clients; the client change is transparent to old servers (an old server does not return the new Ack field, and the client does nothing on an empty value). Proto only adds optional fields; no breaking change. At rollout, correct the "server-assigned rule" wording in memex.proto:360 and the two implementation comments simultaneously (the doc side moves with the batch).

## 6. Verification (written in with each batch after authorization)

- **Server**: ① replay V1 — same account re-logins and sends the same seq with different content → both rows exist in the archive, the new row's msg_id ≠ sha256(from:seq), ACK(seq) returned as normal; ② resend idempotency — resending the same (from,seq) with the same to/text/ts → the archive still has one row, the offline queue is not re-enqueued, online redelivery is sent as normal; ③ the Ack.msg_id return leg; ④ the group-message colliding-id leg; ⑤ the DB-error (non-collision false) acceptance-behavior-unchanged leg.
- **Desktop**: ⑥ the receiver-side re-hashed row is locally visible leg (§4.2); ⑦ the ACK-consumption local-row correction leg (§4.3).
- **Mobile**: ⑧ per-platform seq persistence legs (continuation-after-relogin assertions — Android JVM unit tests / iOS swift test / HarmonyOS node:test).
- **Real-device walkthrough**: rerunning the two 2026-10-09 recorded scenarios (zhangsan re-logins and sends again → the server's messages table has both rows, lisi's send/receive surface has no gaps) as final acceptance.

## 7. Residuals and Honest Boundaries

- ~~Before ACK consumption is deployed, old desktop misses recall/receipts for its own sent re-hashed messages: silently no-op, leaves no dirty state.~~ ✅ Batch 2 closed the books on 2026-10-11: new desktop consumes Ack.msg_id and corrects the local row; old desktop within the upgrade window still misses (silently no-op, leaves no dirty state).
- ~~Before §4.2 is deployed, old desktop's local list is missing received re-hashed rows: the server archive is searchable, but invisible locally — a residual during the upgrade window.~~ ✅ Batch 2 closed the books on 2026-10-11: new desktop re-assigns a local seq for received re-hashed rows and persists them visibly; old desktop within the upgrade window still has the missing row (the server archive is searchable).
- An extreme "same content, same millisecond, same seq" message that is actually new is judged as a resend: engineeringly indistinguishable, equivalent on any observable surface; accepted.
