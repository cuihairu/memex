# Server CLI: Serve Flags

> This page documents the **full flag contract of `memex_server serve`**:
> all 40 flags, the strict-parsing rules behind them, environment-variable
> fallbacks, and the assembly consequences that turn flags into running
> faces. The parser is `parse_serve_args` (`server/src/serve_args.cpp`,
> extracted from `cmd_serve`); its acceptance tests are the `serve_args`
> ctest (`server/tests/test_serve_args.cpp`, direct parser calls, no
> process spawn). For in-app settings switches see
> [Configuration Effects](/guide/config-effects).

## Argument shape

`serve` sub-command flags are parsed from the **sub-command-stripped**
argument segment (`serve_args.hpp:51-54`):

- `argv[0]` is the **first flag** (e.g. `{"--port", "24370"}`) — no program
  name, no sub-command.
- A bare word (anything not starting with `--`) is a **positional argument**
  and is rejected with exit code 2 — the sub-argv shape has no bare words.
- `argc = 0` is legal and yields all defaults (equivalent to a bare
  `serve`).

## Strict parsing (exit code 2 contract)

Every failure below prints a reason to stderr and exits **rc 2** — nothing
falls back to a default silently (`serve_args.cpp:1-4` header note):

| Violation | Example | Rule |
|---|---|---|
| Unknown flag | `--prot` (typo of `--port`) | `serve_args.cpp:326-330` — a misspelled flag must not quietly leave a port or safety switch at its default |
| Positional argument | `serve extra` | `serve_args.cpp:99-102` |
| Missing value at tail | `serve --port` as last arg | `serve_args.cpp:74-85` — the old behavior (silent skip) let ports and safety switches drift to defaults |
| Boolean with a value | `--s3-use-ssl=true` | `serve_args.cpp:198-201` and the other boolean branches — booleans have no value to take |
| Non-strict number | `--port abc`, `--port 99999999999` | `serve_args.cpp:32-49` — only an optional `-` plus all digits; anything else, empty strings, and overflow beyond int magnitude are rejected **instead of wrapping** (std::atoi silently maps `"abc"`→0 and truncates huge values, which turns a typo into a different "valid" config) |
| Out-of-range port | `--port 0`, `--port 70000` | per-flag range checks below |
| Bad `--ldap-group-role` form | `--ldap-group-role admins` (no `=`) | `serve_args.cpp:266-270` — must be `group=role` with both sides non-empty |

**Inline form**: every value flag accepts `--flag=value` in addition to
`--flag value` (`serve_args.cpp:93-97`, codex-761). Boolean flags never take
a value in either form.

## Flag reference

### Ports and faces

| Flag | Default | Range | Effect of 0 |
|---|---|---|---|
| `--port` | 24360 | 1–65535 | — (required > 0) |
| `--webhook-port` | 24361 | 0–65535 | closes webhook intake; everything else keeps running |
| `--model-port` | 0 (off) | 0–65535 | model gateway off (default) |
| `--files-port` | 0 (off) | 0–65535 | file face off (default) |
| `--uplink-port` | 0 (off) | 0–65535 | external one-way uplink face off (default) |

Defaults live in `serve_args.hpp:16-22`; the safety-default stance is
deliberate — the file face, uplink face, and model gateway all **require an
explicit port to open**.

### In-group assistant (phase 3)

| Flag | Effect |
|---|---|
| `--assistant <bot>` | bot name that polls the four commands (`@助手/@纪要/@整理/@检索`); empty = off |
| `--assistant-token <token>` | plaintext token for that bot |

Assembly gates (`main.cpp:276-295`): the assistant additionally requires
**webhook intake open**, a token (flag or `MEMEX_ASSISTANT_TOKEN` env —
prefer the env, flag values are visible in `ps`), and the bot to already
exist via `memex_server bot add` + join. Each unmet gate prints a reason and
the assistant stays off (server still boots).

### Uplink protection (R23-5)

| Flag | Effect |
|---|---|
| `--uplink-ext-denylist <csv>` | whole-table replace of the dangerous-extension denylist (comma-separated, lowercase, no dots; suffixless files never match). Empty string clears the list (= no extension limit). Default table: 23 entries — `exe dll bat cmd com scr msi msp cpl hta jar js jse vbs vbe wsf wsh ps1 sh bash apk deb rpm` (`files_server.hpp:93-97`) |
| `--uplink-ext-allowlist <csv>` | non-empty value **enables allowlist mode**: extensions outside the list are rejected (suffixless too) and the denylist becomes moot (`serve_args.cpp:155-158`) |
| `--uplink-max-mb <n>` | per-face upload cap in MiB, ≥ 0; 0 = keep using the global upload limit (`serve_args.cpp:159-167`) |
| `--uplink-login-secret <s>` | deployment-level second password for external logins: `/uplink/session` must carry a matching `secondary` field, mismatch = 401 (`files_server.hpp:102-104`) |

### Feature keys

| Flag | Effect |
|---|---|
| `--tool-cred-secret <s>` | master key for the credential face (server-side GCM derivation). Absent = credential face not enabled |
| `--task-pull-secret <s>` | master key for the task-source pull face (PBKDF2 at the same strength). Absent = pull face not enabled = **zero outbound calls**; the config route answers 503 |

### S3 (file/uplink storage)

| Flag | Default | Notes |
|---|---|---|
| `--s3-endpoint` | empty | intranet HTTP + path-style by default (`main.cpp:66-72`) |
| `--s3-bucket` | empty | bucket is bootstrapped idempotently at startup (head-check first, 10 × 2 s retries; failure prints a reason and the face still starts with uploads answering 503, `main.cpp:74-84`) |
| `--s3-access-key` | empty | |
| `--s3-secret-key` | empty | |
| `--s3-region` | `auto` | **no environment fallback** for this one flag |
| `--s3-use-ssl` | off | boolean, no value |

The file face only opens for real when `--files-port > 0` **and** all four
S3 values are set (`main.cpp:70`); port given but S3 incomplete prints the
missing-variable hint and the face answers 503 (`main.cpp:87-91`).

### LDAP / AD (optional enterprise identity)

Absent `--ldap-host` = LDAP off — login resolution stays fully local and
lightweight deployments behave byte-for-byte as before (`ldap.hpp:9-10`).
Standing red lines (`ldap.hpp:11-16`): directory unavailable = login
rejected (fail-closed, never falls back to local passwords); passwords only
ever travel inside the bind request (never logged/stored); groups outside
the explicit mapping grant nothing; local accounts always work as the
recovery path.

| Flag | Default | Notes |
|---|---|---|
| `--ldap-host` | empty (off) | |
| `--ldap-port` | 389 | 1–65535 |
| `--ldap-timeout-ms` | 4000 | ≥ 0 |
| `--ldap-user-base` | empty | e.g. `ou=people,dc=corp` |
| `--ldap-user-attr` | `uid` | AD commonly `sAMAccountName` |
| `--ldap-memberof-attr` | `memberOf` | OpenLDAP needs the memberof overlay |
| `--ldap-bind-dn-template` | empty | direct-bind template, `{u}` = username (OpenLDAP style) |
| `--ldap-bind-dn` | empty | service-account DN, search-then-bind (AD style) |
| `--ldap-bind-password` | empty | prefer env `MEMEX_LDAP_BIND_PASSWORD` — flag values show up in `ps` (`serve_args.cpp:239-246`) |
| `--ldap-group-role <g=r>` | empty | explicit group→role mapping, **repeatable**; unknown groups are ignored, and without an `admin` entry in the table nobody ever gets admin (`serve_args.cpp:261-271`) |
| `--ldap-disabled-attr` / `--ldap-disabled-rule` / `--ldap-disabled-value` | empty | optional disabled-detection triple; AD defaults: `userAccountControl` / `1.2.840.113556.1.4.803` / `2` (`serve_args.cpp:250-260`) |
| `--ldap-kick-disabled <n>` | 1 | directory-disabled × existing sessions: 0 = only refuse new logins, non-0 = kick existing sessions |
| `--ldap-recheck-seconds` | 60 | session recheck interval, ≥ 0; 0 = never recheck |
| `--ldap-ssl` | off | boolean — ldaps:// whole-connection TLS (636 stance), certificate hostname check always on |
| `--ldap-starttls` | off | boolean — connect on 389 then upgrade TLS; **mutually exclusive with `--ldap-ssl`** (`ldap.cpp:540`) |
| `--ldap-allow-plain` | off | boolean — explicit plaintext-bind opt-in (default rejects it as a red line; tests/controlled intranets only, startup logs it) |
| `--ldap-tls-insecure` | off | boolean — skip certificate verification for self-signed certs (startup warns) |
| `--ldap-auto-bind` | off | boolean — existing local account without a binding: directory auth success auto-binds it (default off = explicit CLI binding only) |
| `--ldap-auto-provision` | off | boolean — account missing: directory auth success creates a shadow account with a random unknowable local password (LDAP-only login) |

## Environment fallbacks (flags always win)

Empty flag values fall back to environment variables; **a flag on the
command line beats the env even if a debug script rewrote the env**
(`serve_args.cpp:332-334`). Env vars keep secrets out of `ps`/logs in
container deployments:

| Variable | Fills |
|---|---|
| `MEMEX_S3_ENDPOINT` / `MEMEX_S3_BUCKET` / `MEMEX_S3_ACCESS_KEY` / `MEMEX_S3_SECRET_KEY` | the four S3 values (`serve_args.cpp:339-342`) |
| `MEMEX_LDAP_HOST` / `MEMEX_LDAP_USER_BASE` / `MEMEX_LDAP_BIND_DN_TEMPLATE` / `MEMEX_LDAP_BIND_DN` / `MEMEX_LDAP_BIND_PASSWORD` / `MEMEX_LDAP_MEMBEROF_ATTR` | the six LDAP strings (`serve_args.cpp:346-355`) |
| `MEMEX_LDAP_GROUP_ROLE` | comma-separated `group=role` list, appended to flag mappings (`serve_args.cpp:356-370`) |
| `MEMEX_ASSISTANT_TOKEN` | assistant token when `--assistant-token` is absent (`main.cpp:280-284`) |

One asymmetry worth knowing: a **malformed entry inside
`MEMEX_LDAP_GROUP_ROLE`** (no `=`, or empty sides) is **silently skipped**
(`serve_args.cpp:363-366`), while the same malformed shape on the flag is
rc 2. The flag path is the strict one — prefer it when mappings matter.

## Verification

The `serve_args` ctest (`server/tests/test_serve_args.cpp`) drives
`parse_serve_args` directly and covers: defaults, every port boundary
(0 / negative / out-of-range / overflow / garbage → rc 2), uplink table
replacement and `max-mb` math, S3/LDAP env fallbacks with flag-wins,
group→role mappings from both flag and env, and every strict-parse leg
(unknown flag / positional / missing value / boolean-with-value / inline
form). Run it with `ctest -R serve_args` in the server build tree.

> Verification date: 2026-11-15 (flag inventory re-extracted from
> `serve_args.cpp` via `grep -oP 'arg == "--\K[a-z0-9-]+'` = 40; port and
> range facts re-checked against the source lines cited above).
