> **English** | [中文](/guide/wan-drill)

# Cross-Region Access and Disconnection Re-delivery Verification (T5.4)

> Corresponds to acceptance items A13 (cross-subnet communication), A15 (multi-region access), and A16 (disconnection archiving rules). Verification date: 2026-10-03.

## A16 Disconnection Archiving Rules — Automated + Degraded-Network Drill

- Regular acceptance: the `collab_resend` test (process-level, real server) covers buffering during the outage → automatic reconnect → dedup-by-msg_id re-delivery → archive with no duplicates and no gaps.
- Degraded-network drill: `scripts/wan_drill.sh <test_collab_resend path>` adds `netem delay 80ms 20ms loss 2%` on the loopback interface and re-runs the acceptance above; passing proves re-delivery and dedup remain correct under packet loss / high latency.
- 2026-10-03 execution record: `build/client/test_collab_resend` passed (`collab resend tests: all passed`, exit code 0); the qdisc was restored after the drill.

## A13 Cross-Subnet Communication — Environment Requirements and Manual Check List

Mas environment constraints: collaboration-mode messages are relayed by the server with **no subnet restriction**; direct mode uses UDP broadcast discovery, limited to the local subnet (broadcasts not reaching across VLANs is expected), so cross-subnet traffic must go through collaboration mode — this is the design convention, not a defect.
On-site check list:

1. Two terminals on different VLANs, both logged into the same server: exchange TEXT in both directions, retrievable both ways via the server's `messages` CLI;
2. Two terminals on the same VLAN: direct-mode login-free send/receive works, `peers` announcements visible;
3. The same two terminals in direct mode across VLANs: the discovery table is empty and direct send returns peer unreachable (as expected).

## A15 Multi-Region Access — Deployment Prerequisites and Manual Check List

The server is deployed as a single centralized instance in the headquarters machine room; remote regions access it via dedicated lines / encrypted tunnels; the client's only configuration item is the server address (message port defaults to 24360, changeable via `serve --port`). Manual checks:

1. Remote terminal login → send/receive → org structure delivery → `messages` history search (all four pass);
2. Under dedicated-line jitter injection (netem 100ms±50ms / 1% packet loss), login and message send/receive lose nothing and duplicate nothing (the wan_drill approach can be reused);
3. Server unreachable → the client keeps showing the 「消息不进归档」 banner and switches to direct mode → after recovery the backlog is re-delivered into the archive.

This sandbox has no cross-region physical links, so A13/A15 are delivered as check lists for on-site execution; A16 has passed automatically under a degraded network.
