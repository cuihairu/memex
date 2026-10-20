# File Storage and Lifecycle

> Convention page for the two lifecycles of the R23 file plane (group files / personal files / file assistant / external uplink) and chat files (2026-10-13, landing review five's "file lifecycle convention alignment").
> Decision background: see [决策清单 #7](/en/guide/decisions) (centralized deployment · storage volume split implementation — this page is the sub-convention of #7's storage volume split as it lands on the file plane; the decision list has no dedicated file-storage entry); the full design text is in the repo at `docs/design/文件存储与外网单向传输.md`.

## Two Kinds of Files

"Files" in the product have **two independent lifecycles** with different conventions; do not mix them:

| | Chat files (direct, ephemeral) | File-plane files (server-persisted) |
| --- | --- | --- |
| Scenario | Files / images / directories exchanged within a conversation | Group files, personal files, file assistant (inbox / memos), external uplink |
| Byte path | Peer-to-peer direct transfer, **never through the server** | Proxied through the server into object storage; **the client never connects to object storage directly** |
| Metadata | Authorization via the server (FILE_AUTHZ only decides and returns the result); transfer metadata into the archive **deferred (not persisted yet)**; direct mode stores only locally | files table (dedup-by-hash / reference counting / two-tier quotas / pinning); bytes live in the object volume |
| Switch | Always on (the direct-mode baseline) | Explicitly enabled via `--files-port`, off by default (uplink has its own `--uplink-port`) |
| Authorization | Unified server-side authorization before sending (can-send / can-receive / cross-department / re-forward; if the server is unreachable the local side rejects — files are a permission plane and cannot bypass authorization) | FileServer authorizes per request (quarantined / expired files denied reads, personal files owner-only, group files by role) |

## Chat Files: Content Not Archived, Metadata Archiving Deferred

The design convention is that collaboration mode archives **transfer metadata** (file name, size, hash, sender/receiver, time) — answering "what was transferred" — and **does not include file content**; content lands only on the sender's and recipient's local machines. Current state: authorization before sending goes through the server (FILE_AUTHZ only decides and returns the result, session.cpp:805-833); transfer metadata is not yet persisted (the archive has no corresponding table) — that step is **deferred**. In direct mode neither messages nor files enter the server archive (conversations are labeled 「仅存本机 · 不入归档」).

## File Plane: Acceptance Order and Interruption Semantics

Upload acceptance order: **authorization → dedup-by-hash → scan (uplink plane) → quota deduction → byte persistence → metadata persistence** — neither bytes nor metadata are externally visible before acceptance completes.

- **Upload interrupted** (connection drop / timeout): no bytes are persisted. Object storage writes happen only after the request body is fully received; an interruption = no bytes and no row, and the client can simply retransmit — no half-written objects.
- **Byte persistence failure** (storage unavailable): 502 「对象存储写入失败」 + quota rollback; no metadata row is written.
- **Metadata persistence failure**: the server deletes the already-persisted bytes (rollback); no orphans are left.
- **The only orphan window**: the process crashes after bytes are persisted but before metadata is committed — leaving an unreferenced object. `files verify` reports an "orphan object" warning as-is (not judged an error); whether to clean up is an operations decision — the tool does not delete on your behalf (see [备份恢复 · 一致性核对](/en/guide/backup-restore)).

## Degradation Conventions

The message plane and the file plane share **zero dependencies**: chat messages do not go through object storage, and object storage being unavailable **does not affect message send/receive or archiving**.

- **Storage backend not configured**: file-plane uploads return 503 「存储后端未配置」; the metadata plane (lists / quotas / memos) remains fully usable.
- **Storage backend unreachable**: uploads return 502 「对象存储写入失败」 (quota already rolled back) and downloads fail; the client can retry, and no manual intervention is needed once storage recovers.
