> **English** | [中文](../zh/design/文件存储与外网单向传输.md)

# File Storage and One-Way External Transfer Design (R23 Design Draft)

> Status: design draft (2026-10-04 user gave the go-ahead "write it up exactly like this"). Scope: memex file storage surface (group files / personal files / file assistant) + external network one-way inbound into the client + storage backend selection. Cross-references: todo R23, "Construction Plan Review Report V10.0", and this repo's "one-way external" security stance (data outbound is off by default).
>
> **Implementation status (2026-10-07 reconciliation, updated incrementally)**: Of the schedule in §6, **R23-1 storage abstraction layer + RustFS compose and R23-2 group files + personal files are implemented** (ceca13e / 6fd7bee / 7b69240: S3Storage abstraction + RustFS real-container e2e, AuthorizationService permission decisions, all FileServer intranet HTTP surface endpoints, two-level quota / instant transfer / pinning / reference-counted byte deletion); **R23-3 file assistant server side is implemented (127ba66: memos table + files.kind category migration, instant-transfer key includes kind, /files/memo routes POST create/edit · GET single item/list · DELETE delete, upload/list support target=inbox, inbox list = memos + files interleaved in reverse time order; the client desktop surface is implemented (5aa338a: FilesClient engine — Bearer token held in memory / memo CRUD / inbox interleaving / upload download delete all go through FileServer, never object storage direct; FileAssistantDialog session window — connection area + interleaved flow + memo create edit delete + file send receive, main window collaboration menu entry; engine-level + UI smoke double test, ctest 32/32); the Android/iOS/HarmonyOS client surfaces are implemented (bd37c87 / f50aa8f / 1fcd788, same stance per platform)**); **R23-4 external one-way uplink is implemented (9fe86e8: FileServer uplink_mode dual instances mount only /uplink/* write endpoints, the session store is shared across both surfaces so the scope double gate has a real entity, the only landing spot = inbox, full upload audit, --uplink-port explicitly enables it, off by default; test_uplink real TCP dual instances 25 assertions; external-mode client Android surface 9d316be/9c00c52 — UplinkClient as an independent class + file assistant external-mode checkbox, real-device interaction pending verification; the iOS/HarmonyOS uplink clients are low-priority fill-ins and not done)**; **R23-5 protection is implemented (8ad9987: scan.hpp UploadScanner interface + PassThrough passthrough default — the hook slot is in place with zero behavior change, hooking up clamd or a similar engine is a deployment-surface matter handled in a separate batch; UplinkPolicy — the default dangerous-extension blacklist of 24 items is in effect, the allowlist is explicitly enabled and effective whenever non-empty and uniformly rejects anything without a suffix, an independent max_upload_bytes intercepts before receipt with 413, login_secret is a deployment-level second password compared with sha256+XOR in constant time (TOTP in a separate batch); intake order = permission check → instant transfer → scan → charge → write bytes → write metadata, an instant-transfer hit skips the scan, and a 422 rejection still writes the flow log with audit though the bytes are dropped; main has four flags --uplink-ext-denylist/--uplink-ext-allowlist/--uplink-max-mb/--uplink-login-secret; test_uplink adds six legs for a total of 31 assertions, ctest 35/35. Design discretion: scanning is mounted only on the uplink surface — the external network is an untrusted entry point, while the intranet surface sits inside the trust boundary and is not scanned)**. Deviations between the implementation stance and the design (all logged in todo): the presign direct-upload endpoint is not exposed for now per the iron rule "clients never connect directly" (storage.hpp carries a tension warning note); the §5 API surface landed as `FileServer` (`/files/session|upload|download|list|quota|manage.*|/uplink/session|upload|mine|delete`); the §3 table row "external side can re-download what it uploaded" is not landed — implemented strictly per todo as "upload-only, download endpoints always 403 for external tokens" (the uplink surface exposes no download endpoint at all); providing that capability would require an added design review.

## 1. Three file surfaces

| Surface | Ownership | Visible to | Uploader |
|---|---|---|---|
| **Group files** | Group | Group members can read, group owner/admins can manage (delete/pin/quota) | Intranet client |
| **Personal files** | Individual | Self only | Intranet client |
| **File assistant** (self↔self) | Personal inbox | Self only | Intranet + **external network (the only external landing spot)** |

- The file assistant = a unified inbox for memo text + file transfer (phone-to-self / external-to-internal transfer); **anything arriving from the external network is re-forwarded by the user themselves inside the intranet before it enters a group** (safety buffer: it passes human eyes once).
- Instant transfer (hash dedup), quota, and expiration cleanup are done at the metadata layer.

## 2. Storage architecture: S3-compatible abstraction + RustFS as default

```
client(intranet/external) ──► memex server (permission/audit/one-way control)
                                │ signed URL / proxy read-write
                                ▼
                         storage backend (S3-compatible abstraction layer)
                                │
                         default RustFS (one-click via compose, Rust single binary, self-hosted)
                         compatible with MinIO / cloud S3 (just change the endpoint, optional)
```

**Three iron rules**:
1. **Clients never connect directly to object storage** — all reads and writes go through memex server (permission checks, group ownership, audit, and external one-way control all live at this layer); otherwise a naked object store is exposed = the entire security stance is bypassed;
2. **Permissions are always decided at the metadata layer; the object layer only manages bytes**;
3. Object prefix partitioning: `groups/{gid}/`, `users/{uid}/`, external uploads `inbox/{uid}/`.

Selection rationale (compared with local disk): instant transfer / multipart / resumable transfer are native to the S3 protocol (multipart upload / HEAD), scaling to large volumes is off-the-shelf, and switching backends means changing the endpoint; the cost is one more container (a single compose line). RustFS is self-hosted with zero dependencies and does not touch the cloud — which fits the "nothing leaves the intranet" stance.

## 3. External one-way inbound (data ferry)

**Iron rule: an external session has only "write" permission and never any "read intranet data" permission** — otherwise the external client becomes a data-leak channel.

| Action | Intranet client | External client |
|---|---|---|
| Browse/download group files, others' files | ✅ | ❌ bytes not handed over |
| Upload to own space | ✅ | ✅ (**the only landing spot = file assistant**) |
| View own upload records / re-download what one uploaded | ✅ | ✅ (own only) |
| Delete / manage | ✅ | Can only delete own uploads |
| Post into a group | ✅ | ❌ (the intranet user forwards from the file assistant) |

**Three landing items**:
1. **Two routing sets, two scopes**: the external API is independent (`uplink`), and tokens are issued only with the `upload` scope; download endpoints always return 403 for an external token — one-way-ness is guaranteed by **server-side permissions**, not by client goodwill;
2. **Physical surface separation**: the upload gateway is exposed on its own (tunneled outbound, Cloudflare pattern), while the intranet management surface / message surface never go outbound; the external client = "external mode" (phone/web), with features deliberately kept few (login → upload files → view own records);
3. **Audit and protection**: external uploads are fully traced (who / when / what / landing spot), plus an antivirus scan hook, a type/size allowlist, and account + second-factor login.

**Security default** (a hard user requirement): the external entry point is **off by default, enabled only through explicit configuration, and the exposure scope is stated plainly when enabled**.

## 4. Data model (metadata layer)

- File table: id, ownership (gid/uid), uploader, file name, size, hash (instant-transfer key), object key, source (intranet/external), time, status (normal/quarantined/expired);
- Upload records (external): an independent flow log (for audit);
- Quota: two levels, group and individual.

## 5. API surface (outline)

- Intranet: `/files/upload` (sign/proxy), `/files/download`, `/files/list` (by ownership), group file management (delete/pin/quota);
- External uplink: `/uplink/upload`, `/uplink/mine` (own records), `/uplink/delete/{id}` (own) — **no endpoint of any kind for downloading intranet data**.

## 6. Schedule suggestion (registered into todo R23)

1. R23-1 storage abstraction layer + RustFS compose integration (including metadata tables / quota / instant transfer);
2. R23-2 group files + personal files (full intranet functionality);
3. R23-3 file assistant (memos + file transfer, intranet);
4. R23-4 external one-way uplink (two routing sets / audit / off by default with explicit enable) + external-mode client;
5. R23-5 antivirus scan hook + allowlist.
