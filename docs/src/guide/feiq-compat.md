# FeiQ / IPMSG Compatibility

> Requirement (2026-10-11 user directive): when a FeiQ client exists on the same LAN segment, memex appears in its online list and the two can exchange direct messages.
> Convention: FeiQ compatibility is a **bypass** (off by default, explicitly enabled in the settings page); it does not change any behavior of memex's direct/collaboration dual mode; advanced IPMSG features such as file transfer, encryption, and broadcast group messaging are not implemented.

## Switch and Entry Point

The "Network & Remote" settings page gains a "FeiQ compatibility" switch (`net/ipmsg_enabled`, off by default). Enabling takes effect immediately: the same UDP 2425 port immediately re-sends an IPMSG `BR_ENTRY` broadcast, so the FeiQ online lists see each other; once disabled, IPMSG packets are silently dropped and the socket only carries memex JSON discovery.

## Protocol Conventions

The same UDP 2425 port demultiplexes by **first byte**: `{` = memex JSON announcement (the original discovery channel, behavior unchanged); a digit = IPMSG packet (`ver:packet_no:user:host:command:additional`, command field in hexadecimal).

| Plane | Behavior |
|---|---|
| Online list | Periodically sends `BR_ENTRY` alongside (on the same tick as the JSON announcement); a received FeiQ `BR_ENTRY` always gets an `ANSENTRY` reply (protocol handshake — without a reply, the other side's online list will not contain me); `BR_EXIT` removes the row on receipt |
| Identity | The local header's user = collaboration account (falls back to device name when not logged in); host is always `memex` — an IPMSG packet with host=memex is another memex instance (already registered via the JSON channel), so no row is created on the IPMSG side, preventing duplicate rows from same-machine mutual visibility |
| Direct message receipt | `SENDMSG` always gets a `RECVMSG` reply (the original packet number is written back into the additional field; the peer's delivery checkmark depends on this); the message body is decoded per the `UTF8OPT` bit (set = UTF-8, otherwise GB18030 — a superset of GBK, so real FeiQ bytes decode losslessly) |
| Direct message send | Encode as GBK/CP936 first (real FeiQ's Windows ANSI code page convention); characters outside GBK such as emoji fall back to UTF-8 with `UTF8OPT` set (not GB18030 — bytes encodable in the superset still garble in old FeiQ; better to let the newer peer with the bit set decode per the bit) |
| Receipt | Sends carry `SEND_CHECKOPT`; the peer's `RECVMSG` writes the local `text_delivered` back with the same number — the packet number is the caller's send sequence number, so the UI delivery state aligns directly |
| Unknown commands | Silently ignored (newer feature packets get no reply and do not crash) |

Encoding/decoding is a pure-function plane (`client/engine/direct/ipmsg.{hpp,cpp}`); send/receive and character sets all live here, while the network side (discovery) only does demux and routing; out-of-list options, ciphertext (the `SECRETOPT` bit is defined but has no consumer — ignored on receipt; message bodies are always decoded per the `UTF8OPT` bit), and advanced behaviors such as file attachments are not implemented.

## UI and Persistence

FeiQ peers enter the direct-connection online list in the form `ipmsg:user@host` (the metadata row is labeled 「飞秋 · UDP」); direct message send/receive follows the same path as memex direct messages: local SQLite persistence (`source=direct`, not archived to the server), unread badges, system notifications, and cross-mode closed loops are all reused.

## Acceptance Evidence (2026-10-12, local machine)

- `test_ipmsg` (ctest name `ipmsg`): encode/decode round-trips and malformed rejection (field count / base / JSON); mock FeiQ interop (GB18030/GBK Chinese bidirectional, RECVMSG original-packet-number receipt, emoji→UTF8OPT fallback, `BR_EXIT` row removal, silent on disable, memex marker does not create duplicate rows); direct-connection engine full chain (`send_text` demux → receipt closes the ticket, inbound message persisted and surfaced).
- `ss -ulpn`: with compatibility enabled the client listens on UDP 2425 (the FeiQ/IPMSG fixed port); see the snapshot in the next section.

## Known Boundaries (honest notes)

- **Real FeiQ client testing not done — no environment available** — the local machine (Linux) has no installable FeiQ client; interop evidence stops at protocol-convention cross-checking (constants and packet formats were settled by cross-checking against the FeiQ source ipmsg.h and compilelife/feiq's feiqlib) and mock FeiQ interop. Until a real machine runs it, treat "mutual visibility and messaging with real FeiQ" as **unverified**.
- When memex↔FeiQ interop runs on the same machine (both bound to 2425), with SO_REUSEADDR unicast only reaches the last-bound socket — real scenarios (FeiQ on another machine) are unaffected; the test mock avoids this via a bind-order contract (see the header comment in `test_ipmsg.cpp`).
- FeiQ's file transfer, encryption, and away-mode auto-reply are not implemented; the corresponding option bits are ignored on receipt.
