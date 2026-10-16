> **English** | [中文](群内智能助手.md)

# In-Group AI Assistant Design Spec (Platform Phase 3 · line164)

Four parts: **in-group AI assistant · meeting-minutes generation · requirement extraction · smart retrieval (retrieval-augmented)**
—one built-in orchestrator (GroupAssistantWorker): polls the group's messages and direct chats as a bot identity,
identifies commands, generates answers through the model gateway (line163), and replies to the group as a bot.
The retrieval-augmented material comes from the **trail archive** (the bot's group messages are naturally within the retrieval radius).

## 0. Scope

**This round ships**: four commands (@助手／@纪要／@整理／@检索), RAG context injection,
delivery back to the group, serve assembly (--assistant), and full-chain tests.

**Deferred (explicitly noted)**: command extensions such as @提醒/@待办; per-group command switches;
multi-turn conversation (context memory); streaming replies (follow up after the gateway's SSE).

## 1. Form and Threading

- The orchestrator = an independent thread inside serve (explicitly enabled with `--assistant <bot名>`), polling
  `pending_offline("bot:<名>")` (TEXT fan-out enqueues automatically — same
  receiving surface as external bots);
- **Replies to the group go through the loopback HTTP** `POST /bot/send` (reusing the existing
  auth/validation/delivery chain; deliver_notice requires the CollabServer online table — lock-free,
  unusable across threads, so it is not called directly); the bot token goes via
  `--assistant-token` (value goes into ps, env `MEMEX_ASSISTANT_TOKEN`
  suggested, same convention as --tool-cred-secret);
- Model calls go through the same-process model gateway loopback `http://127.0.0.1:<model-port>`,
  Bearer = same bot token.

## 2. Commands (prefix hardcoded, Chinese product convention)

| Command | Behavior | Model |
| --- | --- | --- |
| `@助手 <question>` | RAG: the group's most recent 20 archived messages as context + question | via gateway |
| `@纪要` | The group's most recent 50 messages → minutes template (key points / conclusions / to-dos) | via gateway |
| `@整理 <text>` | Structured requirement template (background / goal / function points / acceptance criteria) | via gateway |
| `@检索 <keyword>` | Archive hit list formatted and sent back to the group | **not via** |

- Group-context retrieval: `search_messages(account="bot:<名>")` — account filtering naturally
  includes "all group messages of the group" (group_members subquery) + direct chats involving the bot; filter further by
  `to_account==target group`; take the most recent within the limit;
- **All gateway requests carry `memex_archive_scope:true`** — context/material comes from archive
  data, the red line (local model only) applies verbatim: if no local endpoint exists, reply to the group explicitly with 「归档数据仅本地模型，当前无可用本地端点」;
- Reply target: triggered by a group message → reply to that group; triggered by a direct chat with the bot → reply to the sender;
- Non-command messages are not handled (skipped after ack clears the queue); after handling, `ack_offline` immediately.

## 3. Failure Conventions

| Situation | Behavior |
| --- | --- |
| Gateway unreachable / 503 (including red-line interception) | Reply to the group with an error message (including the reason), no retry |
| Upstream 4xx | Reply to the group with 「模型拒绝」 + reason |
| Loopback /bot/send failure (webhook port closed) | Logged; message already acked, no redelivery |
| Command format missing parameters | Reply to the group with a usage hint |

## 4. serve Assembly

```
memex_server serve --assistant <bot名> [--assistant-token <明文>]
    --webhook-port N --model-port M
```

- assistant enabled while the webhook port is closed → startup fails explicitly (the reply-to-group surface does not exist);
- when model-port=0, @助手/@纪要/@整理 reply 「模型未启用」; @检索 is unaffected.

## 5. Test Conventions

test_group_assistant.cpp: real CollabServer + WebhookServer + ModelGateway
Server + fake upstream (same as test_model_gateway) + real Worker (100ms polling interval).
Legs: non-command does not trigger / @检索 hits are replied to the group / @助手 prompt contains archive context + question
(fake upstream echoes for assertion) and gateway audit archive_scope=true / @纪要 template + the group's most recent
messages / @整理 template + text / direct chat replies to the sender / model unavailable replies to the group with an error /
acked messages are not processed again.
