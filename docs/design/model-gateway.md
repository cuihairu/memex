> **English** | [中文](模型网关.md)

# Model Gateway Design Draft (Platform Phase Three)

OpenAI-compatible entry: intranet services (in-group intelligent assistant and other bots) call models in Chat Completions form; the gateway is responsible for endpoint registration, routing, degradation, and call audit. **Archived data only goes to the local model — the rule is written into the code, not made into configuration.**

## 0. Scope

**This increment lands**: endpoint registration (CLI), routing and degradation, call audit, archive red line, OpenAI-compatible `/v1/chat/completions`, Bearer = bot token.

**Left for later (honestly noted)**: streaming (SSE) response; multimodal; quota/rate-limiting; endpoint health probing; management UI (CLI already covers); https upstream (intranet stance is http only).

## 1. Process and Thread Model

Independent port (`--model-port`, off by default) + independent io_context + independent thread: upstream LLM calls are second-level blocking, and do not compete for the shared io_context with CollabServer/WebhookServer.
Own ServerStore **second connection** opens the same DB file (`sqlite3_busy_timeout(5s)` to cover write-lock contention).

## 2. Authentication

`Authorization: Bearer <bot token>` — the subject is the bot (`bot:<name>`); disabled 403 follows the same bot-surface semantics. In-group intelligent assistant and other internal services = bot, the same identity system as the send/receive surface.

## 3. Request and Routing

```
POST /v1/chat/completions
{"model": "<endpoint name·optional>", "messages": [...],
 "memex_archive_scope": true·optional}
```

- Candidate endpoints: `enabled=1`, by **registration order** (id order) i.e. priority order;
- **Archive red line**: requests with `memex_archive_scope:true` only route to `is_local=1` endpoints; if no local endpoint is available, 503 reject — **the request does not leave the building** (written into model_gateway.cpp, not open to configuration; this leg's fake upstream zero-hit is pinned by a test);
- An explicit `model` that hits an endpoint name is moved to the front of the candidates (still within the candidate set);
- The sent body only contains `model` + `messages` (if the endpoint has a `model` field configured, it is rewritten).

## 4. Degradation and Forwarding

| Upstream result | Behavior |
| --- | --- |
| 2xx | Pass through the response (inject `memex_endpoint` field for troubleshooting) |
| 4xx (non-408/429) | Client error does not degrade, forward status and error body as-is |
| 408/429/5xx/connection failure/timeout | Degrade to the next candidate; exhausted = 502 |

Upstream call: blocking HTTP/1.1 POST `{base_url}/chat/completions`, read to EOF (Connection: close); 30s deadline (independent io_context + steady_timer, close socket on timeout). Only `http://` is supported (intranet stance).

## 5. Call Audit

model_calls table: **one row per upstream attempt** (degradation = multiple rows, reconcilable) — caller, endpoint, model, archive_scope, prompt_chars/completion_chars (lengths, no body text), status (0 = connection failure/timeout), latency_ms, created_ms.
`memex_server model calls [N]` ledger.

## 6. CLI

```
memex_server model endpoint add <name> --url <http://host[:port][/prefix]>
    [--key <api_key>] [--model <upstream model name>] [--local] --by <account>
memex_server model endpoint list | remove <name> | enable|disable <name>
memex_server model endpoint local <name> on|off
memex_server model calls [N]
```

## 7. Test Stance

test_model_gateway.cpp: fake upstream = in-test blocking acceptor thread (accepts exactly N connections then shuts down). Legs: 401 dual-path / 404 / 405 / 400 three forms / disabled 403 / no endpoint 503 / **archive red line** (503 when only external endpoints, and external fake upstream zero-hit) / non-scope normal route to external / 5xx degradation (audit two rows 500+200) / 4xx no degradation (forward 400) / explicit model routing + endpoint model rewrite (fake upstream echo assertion) / archive_scope audit trace-leaving / all endpoints unavailable 502 (connection failure status=0 trace-leaving).
