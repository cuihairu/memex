> **English** | [中文](/guide/av-collab)

# Audio/Video Conferencing and Collaborative Document Base

Audio/video (LiveKit) and collaborative documents (OnlyOffice) are deployed as **separate
processes**, keeping a process boundary with the memex collaboration server: media bytes and
document bytes never enter the memex archive store; meeting metadata (room / members /
start-end) and document-session metadata are traced through a callback interface (webhook →
archive; interface reserved, client entry points to follow).

## Deployment

```bash
# 音视频：LiveKit（内嵌 TURN，对称 NAT 可达）
docker compose -f scripts/deploy/meet-compose.yml up -d
# 协同文档：OnlyOffice DocumentServer
docker compose -f scripts/deploy/docs-compose.yml up -d
```

| Service | Address | Notes |
|---|---|---|
| LiveKit HTTP API | `http://host:7880/` | Create / list / delete rooms (API key authentication) |
| LiveKit RTC/TCP | `host:7881` | Media fallback when UDP does not get through |
| LiveKit TURN | `host:3478` (UDP/TCP; TLS needs a certificate, enabled behind the production reverse proxy) | Symmetric-NAT traversal, embedded implementation |
| OnlyOffice | `http://host:8800/` | Editor, API, `/healthcheck` |

## Verification

```bash
bash scripts/deploy/av_docs_drill.sh
```

Drill contents: compose configuration validation → bring up → create/list/delete-room APIs
all pass → TURN Allocate succeeds (the traversal credential chain) → OnlyOffice health check
→ automatic teardown. Pass criterion: the script exits 0 (all 6 steps green).

## Choice

- Conferencing: LiveKit (Apache-2.0, self-hosted SFU, embedded TURN saves one component).
  Fallback: Jitsi (Apache-2.0, the jvb + jicofo + meet trio, heavier to operate).
- Documents: OnlyOffice (AGPL-3.0, see below).
  Fallback: Collabora (MPL-2.0, no network clause).

## Licensing and compliance

OnlyOffice is AGPL-3.0: run the official image **as-is** (no modification, no redistribution),
and the memex binary links none of its code (the process boundary is the license boundary).
Delivery includes the `third_party/README.md` registry plus the image digest
(`docker inspect`); upstream source: https://github.com/ONLYOFFICE/DocumentServer. If the
AGPL process is unacceptable, switch to the Collabora alternative.

## Pre-launch hardening checklist

- [ ] Replace all demo credentials (`livekit.yaml` keys, the OnlyOffice `JWT_SECRET`,
      the TURN static account); deliver keys through the intranet KMS / vault.
- [ ] Put LiveKit / OnlyOffice behind a TLS reverse proxy (certificates issued by the
      intranet CA), and turn off `ALLOW_PRIVATE_IP_ADDRESS` in OnlyOffice.
- [ ] Wire the conference webhook (room start/end, member join/leave) into the memex archive
      (metadata archived; the audio/video streams themselves are archived in minutes form,
      per the traceability decision).
- [ ] Wire the document callbacks (save / version) into the archive: who edited which
      document and when (metadata only, no body text; body versions are managed by
      DocumentServer itself).
- [ ] Client entry points: the 「发起会议」 and 「协同编辑」 buttons inside a session
      (xdg-open the room URL / document URL), keeping the process boundary (no embedded SDK).
