> **English** | [中文](../zh/design/机器人平台.md)

# Bot Platform (bot) Design Draft

Modelled on the Telegram Bot developer experience: create bots in the admin console (CLI), issue tokens, and let developers get send/receive working with "a few lines of code" (curl / shell / any HTTP client). Intranet context: message receipt uses **long-polling pulls** (`GET /bot/updates?timeout_ms=N`, a real suspension, see §3); webhook callbacks to developer-hosted services are deferred.

## 0. Scope

**Landed in this pass**:

- bot account system: CLI creation (`bot add`), an independent token, joining groups / being direct-messaged;
- the **pull** side of the two message transport modes: `POST /bot/send` to send + `GET /bot/updates` to pull + `POST /bot/ack` to acknowledge;
- developer experience: a curl example plus `examples/echo_bot.sh` (prints whatever it receives);
- permission boundary: disabled 403, group sending requires the bot to be a member;
- trace-leaving stance: bot messages go through the same chain as ordinary TEXT/NOTICE and are **fully written into the trace archive**.

**2026-10-11 addendum**: real-suspension long polling closed — `GET /bot/updates?timeout_ms=N` suspends on an empty queue until a new message enqueues or the timeout expires (§3). Zero new threads: the suspension timer hangs off the shared io_context, and the wake source is the enqueue hook fired after `ServerStore::queue_offline` truly inserts a new row (same thread as every enqueue point, lock-free); capped at 30 s so a connection can never hang forever.

**2026-10-11 addendum (second batch the same day)**: fine-grained permission allowlists closed (§2.1) — group allowlist (non-empty list ⇒ the bot's group memberships ⊆ the list; allow/deny changes also drop the bot from groups outside the list; joining a group outside the list is refused at the store layer) plus department allowlist (a direct-message target's department must fall inside a whitelisted subtree, and an unassigned department is denied on the strict side); an empty list means unlimited (zero behaviour change for existing bots); inbound is not filtered — the allowlist governs whom the bot can act on proactively, not who can reach the bot.

**Deferred (noted honestly, no fake green)**:

- webhook callbacks to a developer service (the bot calls back the developer's URL after receiving a message — a resident developer service in the intranet needs callback retry / signing and similar support);
- bot-reported proactive events (callbacks such as membership changes).

## 1. Identity model: a bot is a pseudo-account `bot:<name>`

No users table migration: neither `group_members` nor `offline_messages` has **any foreign key**, so the pseudo-account `bot:<name>` works as-is —

- adding a bot to a group = insert straight into `group_members(group_id, 'bot:<name>', joined_ms)`;
- TEXT fan-out (session.cpp) calls `queue_offline` per group member, and if a member is `bot:<name>` it lands in the bot's offline queue automatically — **zero change**;
- a direct message with `to=bot:<name>` is enqueued directly (the TEXT direct-message path does not validate account existence) — zero change;
- a bot name can never be confused with a real account in the same namespace: the `bot:` prefix is reserved, and CLI creation validates that the name is non-empty and contains no `:`.

The bots table manages only **credentials and switches**:

```sql
CREATE TABLE IF NOT EXISTS bots (
  name TEXT PRIMARY KEY,        -- without the "bot:" prefix
  token_hash TEXT NOT NULL UNIQUE,
  created_by TEXT NOT NULL,
  created_ms INTEGER NOT NULL,
  disabled INTEGER NOT NULL DEFAULT 0);
```

The plaintext token is printed once, at `bot add`; only a sha256 digest is stored (same stance as webhook).

The usable-scope allowlists (fine-grained permission, §2.1) live in two more tables — also foreign-key free (same stance as group_members), cleared along with `bot remove`:

```sql
CREATE TABLE IF NOT EXISTS bot_group_allow (
  name TEXT NOT NULL,
  group_id INTEGER NOT NULL,
  created_ms INTEGER NOT NULL,
  UNIQUE(name, group_id));
CREATE TABLE IF NOT EXISTS bot_dept_allow (
  name TEXT NOT NULL,
  department_path TEXT NOT NULL,
  created_ms INTEGER NOT NULL,
  UNIQUE(name, department_path));
```

## 2. Sending: POST /bot/send

Bearer-token authentication (shared with the webhook port, the T4.10 delivery surface):

```
POST /bot/send
Authorization: Bearer <token>
{"target": "group:5" | "alice", "text": "构建完成 ✓"}
```

| Situation | Status code |
| --- | --- |
| token missing / invalid | 401 |
| bot disabled | 403 |
| target missing / text empty | 400 |
| group does not exist / account does not exist | 404 |
| group outside the bot's group allowlist (when the list is non-empty) | 403 |
| account outside the bot's department allowlist (when the list is non-empty) | 403 |
| sending to a group while the bot is not a member | 403 |

Placement of the two allowlist 403s: for a group target, after the group-existence 404 and before the membership 403 (organizational configuration takes precedence over group-level membership); for an account target, before `deliver_notice` (the list does not govern ghost accounts — the account-existence 404 keeps its existing semantics inside `deliver_notice`).

On success the `deliver_notice` delivery chain is reused (NOTICE envelope): **`deliver_notice` gains a sender parameter** (the default `kNoticeSender` keeps notification behaviour unchanged; the bot passes `"bot:<name>"`) — msg_id derivation, offline enqueue, **archive from=bot:<name>**, online fan-out, and favourite-contacts are all unchanged. A bot message carries title = the bot's display name (`bot:<name>`) and content = text, and the archived body is the same source as what clients render (compose_notice_text).

### 2.1 Allowlist of usable scope (fine-grained permission)

Stance: **a list only tightens when non-empty; empty means unlimited** — an existing bot that configures no list has zero behaviour change (`bot_group_allowed` / `bot_account_allowed` always pass when the list is empty). The list does not store delete semantics (deny merely removes the entry), and ghost accounts are not under the list's jurisdiction (they pass through to the 404).

- **Group allowlist** (`bot_group_allow`): the invariant is that when the list is non-empty the bot's group memberships ⊆ the list. After allow/deny, a unified sweep runs: groups not in the list have the bot dropped from them (`bot_prune_group_membership`, EXISTS guard = sweep only when the list is non-empty, an empty list leaves memberships alone); deny additionally exits that group explicitly (a list returning to empty means unlimited and does not sweep, so without an explicit exit the bot would linger in the group and still send and receive); `bot_join_group` refuses groups outside the list at the store layer (CLI `bot join` refuses the same way). So "an admin removes a group from the list afterwards" takes effect immediately — the bot leaves the group on the spot and the group's messages no longer enter the bot's queue (fan-out only reaches enrolled members).
- **Department allowlist** (`bot_dept_allow`): a direct-message target account's `department_path` must equal a whitelisted path or fall inside its subtree — "/"-segment boundary prefix matching (so "研发" cannot accidentally swallow "研发部", the same stance as PolicyRow's level-by-level upward walk); an unassigned department is denied on the strict side (same precedent as cross-department files); the path must already exist (allow does not implicitly create a department).
- **Inbound is not filtered (noted honestly)**: the allowlist governs whom the bot can **act on proactively** (send/join), not **who can reach the bot** — inbound direct-message delivery and TEXT fan-out staying untouched is the foundation the bot surface stands on (the in-group assistant and the echo demo both depend on it), and the allowlist does not block inbound.

CLI: `memex_server bot allow|deny <name> --group <群号> | --dept <部门全路径>` maintains the lists (CLI `bot list` shows an allowlist summary in the trailing column: `群{…} 部门{…}`, displayed as `-` when empty).

## 3. Receiving: GET /bot/updates + POST /bot/ack

The offline queue is the bot's inbox (TEXT fan-out enqueues automatically, see §1):

```
GET /bot/updates[?timeout_ms=N]
Authorization: Bearer <token>
→ {"ok":true,"updates":[{"msg_id":"…","from":"alice","to":"bot:echo",
   "text":"你好","ts_ms":1697…}]}

POST /bot/ack  {"msg_id":"…"}   → {"ok":true}
```

updates decodes the protobuf blobs of `pending_offline("bot:<name>")` one by one into JSON (only TEXT is decoded; the other types such as NOTICE are skipped as-is without decoding — the bot surface only commits to text). **Long polling**: `timeout_ms=N` (milliseconds, 0 is the default = pull-and-return immediately as before) plus an empty queue suspends the request until the bot has a new message enqueued (returns early, guaranteed non-empty) or it expires (returns an empty array); truncated at 30000 ms, and a non-numeric value gives 400. The implementation adds zero threads — the suspension timer hangs off the single-threaded io_context shared with CollabServer, and the wake is the enqueue hook fired when `ServerStore::queue_offline` truly inserts a row (`sqlite3_changes` > 0; a key collision with IGNORE does not wake), on the same thread as every enqueue point, so register/cancel are race-free; during a suspension the connection reference is held by the suspension table, and if the client disconnects the write-back simply fails at expiry or wake and the case closes naturally. A non-empty queue with the parameter, or no parameter at all, both pull-and-return immediately.

After ack the message is cleared from the queue (at-least-once delivery plus ACK clearing, the same semantics as TEXT offline re-delivery).

## 4. CLI (admin console)

```
memex_server bot add <name> --by <账号>        # token printed once here only
memex_server bot list                          # trailing column is the allowlist summary
memex_server bot remove <name>
memex_server bot disable|enable <name>
memex_server bot join <name> <群号>            # after joining it can receive group messages / send to the group
memex_server bot leave <name> <群号>
memex_server bot allow|deny <name> --group <群号>    # group allowlist (§2.1)
memex_server bot allow|deny <name> --dept <部门全路径>  # department allowlist (§2.1)
```

## 5. Developer experience: running it in a few lines

```bash
# create (admin console)
memex_server bot add echo --by admin1
#   token: bot_xxx… (this time only)

# send and receive (developer)
TOKEN=bot_xxx…
curl -s -H "Authorization: Bearer $TOKEN" \
     -H 'Content-Type: application/json' \
     -d '{"target":"alice","text":"hello"}' http://<服务器>:<port>/bot/send
curl -s -H "Authorization: Bearer $TOKEN" http://<服务器>:<port>/bot/updates
curl -s -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
     -d '{"msg_id":"…"}' http://<服务器>:<port>/bot/ack
```

`examples/echo_bot.sh`: prints whatever it receives (updates polling plus ack plus replying verbatim; jq may be missing, so it degrades to grep parsing).

## 6. Test stance

Modeled on test_webhook.cpp: a real store plus a real WebhookServer (port=0) plus HTTP round trips. Legs: wrong token 401 / disabled 403 / ghost group 404 / non-member group send 403 / after joining 200 and archive from=bot:<name> / updates decodes TEXT fields / after ack a next pull is empty / TEXT group fan-out reaches the bot queue (a logged-in user sends a group message and the bot's updates pulls it) / direct message to=bot:… enqueues / updates without Authorization 401. Five long-polling legs (illegal timeout 400 / updates over POST 405 / non-empty queue pull-and-return immediately / empty queue suspend and wake / expiry returns empty / two suspensions wake together). Seven allowlist legs (default with an empty list: group send 200 / allow a group → a group outside the list is dropped plus send 403 where the allowlist stance precedes membership plus 200 for an in-list group / joining a group outside the list is refused / deny a group → left plus non-member 403 plus the group's messages no longer enqueue / allow a department → inside the subtree 200, outside the tree 403, unassigned 403, ghost 404 / deny a department → 403, and restoring to 200 once the list returns to empty / bot remove clears the lists along with it and leaves no residue).
