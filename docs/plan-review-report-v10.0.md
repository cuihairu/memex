> **English** | [中文](zh/建设方案评审报告-V10.0.md)

> **Translation note**: This is a summary-level English rendering of a 1204-line Chinese report. It preserves the full heading hierarchy, all numbered requirements, all phase/batch tables with acceptance criteria, the risk and red-line sections, and the module and boundary definitions, but it is not a line-by-line translation. The Chinese original is authoritative.

# Construction Plan Review Report: Intranet Office Instant Messaging System (Memex)

> **Technology selection and construction plan review · Internal material**
> Version: V10.0 (review draft) | System English name and code name: Memex | Prepared by: Information Technology Department | Report date: October 2026

---

## Contents

1. [I. Project Background and Requirements Analysis](#i-project-background-and-requirements-analysis)
2. [II. Benchmark Function Inventory and Phased Planning](#ii-benchmark-function-inventory-and-phased-planning)
3. [III. Why Not to Rebuild on an Open-Source Solution](#iii-why-not-to-rebuild-on-an-open-source-solution)
4. [IV. Overall Architecture Design](#iv-overall-architecture-design)
5. [V. Solution Design](#v-solution-design)
6. [VI. AI Capability Planning (Phase Three)](#vi-ai-capability-planning-phase-three)
7. [VII. Companion LAN File-Sharing Plan](#vii-companion-lan-file-sharing-plan)
8. [VIII. Network Topology and Deployment Planning](#viii-network-topology-and-deployment-planning)
9. [IX. Risk and Compliance Notes](#ix-risk-and-compliance-notes)
10. [X. Implementation Plan and Recommendations](#x-implementation-plan-and-recommendations)
11. [XI. Items for the Review Board to Decide](#xi-items-for-the-review-board-to-decide)
12. [Appendix A: Device Fingerprint Implementation Points](#appendix-a-device-fingerprint-implementation-points)
13. [Appendix B: Reference Projects and Sources](#appendix-b-reference-projects-and-sources)

---

## Executive Summary

This report answers three questions: **in an isolated intranet environment, can off-the-shelf open-source software replace the current LAN communication tool; if not, how should we build our own; and to what extent, in how many steps.**

The conclusion: **off-the-shelf open-source software cannot meet the requirements; adopt a plan with a self-written business layer on open-component foundations; scope the functionality across four phases — Phase 1 delivers the communication and file-sharing core, Phase 2 delivers office capabilities, Phase 3 delivers AI capabilities, and Phase 4 pursues conferencing and collaborative documents as conditions allow.**

**Table 1: Core conclusions at a glance**

| Item | Content |
| --- | --- |
| Technical route | Self-written business layer (open-component foundations); no second-generation development of an open-source IM |
| Licensing discipline | No integration of strongly contagious components (AGPL/SSPL etc.); open components are introduced only by deployment invocation or permissive-license linking; a component list is submitted at delivery |
| Media foundation (target architecture; not delivered) | Remote-assist screen streaming, audio/video conferencing, and screen sharing are planned to share one channel, with WebRTC as the unified standard foundation; no self-developed transport layer |
| Client form | One installer containing two engines: zero-configuration direct mode when not logged in; server-collaboration mode after login |
| Archiving boundary | Collaboration-mode messages are fully persisted server-side; direct-mode messages stay on the local machine only (an inherent property of that mode) |
| Trace-leaving principle | **The sole purpose of connecting to the server is to leave a trace**; no secret chats or burn-after-reading, and no feature that lets a sender erase the archive |
| Account model | Multi-device login supported; on desktop, one account allows a single online instance — a repeat login kicks the previous one |
| Deployment form | A single centralized collaboration server, no regional edge nodes; multi-region access via dedicated line or encrypted tunnel; direct mode is confined to the local network segment and positioned as temporary, nearby use |
| Functional scope | Modeled on WeChat Work's scope, split into four phases; audio/video conferencing and collaborative documents are recommended NOT to be self-developed |
| AI access | The model gateway uniformly uses the OpenAI-compatible protocol; both local models and external endpoints can be registered; archived data may go to local models only |
| Communication experience | Screenshot and annotation, sticker packs, frequent contacts, reporting lines, office floor maps, and remote assist are all included in the phased list |
| Phase 1 effort | About 20–25 person-weeks (roughly 2.5 months with 2 people) |
| Three-phase total | About 38–48 person-weeks (excluding Phase 4 and hardware procurement) |
| System identity | English name and code name Memex; formal Chinese name remains 「内网办公即时通讯系统」; the identity list is in Chapter V, Section 6 |

The only change in V10.0: **the system's English name and code name are fixed as Memex**, and a new Section 6, "System Identity and Naming Conventions," was added to Chapter V to pin down the naming rationale, the identity list, and one wording stance to avoid. Naming is not decorative here — it must satisfy four constraints at once: not share a name with an already-evaluated-and-excluded open-source project; not resemble commercial office-communication products; not imply secrecy or surveillance; and be directly usable as an identifier.

Three positions established in V9.0 carry over, all on the theme of "the technical foundation and licensing boundary of media capability":

1. **WebRTC is the planned media-transport foundation for the whole system; it is not implemented in the current delivery** (new Chapter IV, Section 10). Remote-assist streaming, conferencing media, and screen sharing are planned to share one channel, with WebRTC as the standard transport base and no self-developed transport layer. The section also draws WebRTC's capability boundary — **it is a pipe and contains none of the capabilities at either end**: screen capture, input injection, authorization confirmation, multi-party forwarding (SFU), and operation auditing are all outside it. This also fixes the planned scope of the 3–4 person-week remote-assist estimate.
2. **Component-level licensing discipline**, with the remote-assist selection argument revised accordingly (new Chapter III Section 4; Chapter II Section 4). What is excluded is **integration of strongly contagious (AGPL/SSPL) components**, not all open source — invocation models, deployed media servers, and linking of permissively licensed protocol libraries are all allowed, with a third-party component list submitted at delivery. Remote assist was re-argued: besides RustDesk (excluded for AGPL), **MeshCentral, Guacamole, and FreeRDP are three Apache-2.0 options excluded not for licensing but for mismatch with this plan's authorization-interaction and visibility requirements**.
3. **A "media capability" evaluation dimension added to client selection** (Chapter V Section 3). The original comparison covered only development efficiency, package size, and domestic-OS fit. Verification found that **WebKitGTK as packaged by mainstream distributions does not compile WebRTC by default, so the Tauri route cannot establish media connections on domestic Linux terminals** — and domestic systems are a mandatory coverage target. The three candidates' conclusions were adjusted accordingly, with additions to the technical-risk table and Phase 1 acceptance criteria.

Two positions from V8.0 carry over:

1. **"Trace-first" made explicit, with explicitly unavailable features listed** (Chapters I, II). One frequently misread wording is clarified: the "secret chat (burn-after-reading)" mentioned in the plan refers to **a feature of the current LAN tool**, cited to illustrate the expectation gap at rollout — **not a capability this plan will build**. Four feature classes are therefore explicitly excluded: secret chat/burn-after-reading; recall that also erases the server archive; one-sided archive deletion on the terminal; and any user-facing trace switch. **The purpose of connecting to the server is to leave a trace; any feature that lets the sender erase the trace is mutually exclusive with that purpose.** This also explains in reverse why direct mode's "no server, no archive" must remain an inherent property of the form rather than an on-demand hidden mode.
2. **Direct mode's scope converges to the local network segment** (Chapter VIII). Login-free direct mode is positioned as **temporary, nearby use within the local segment** — visitors, borrowing a machine briefly, instant Q&A within one office area. Boundary: **direct mode does not pursue cross-segment auto-discovery, and no network-side changes will be made for it**; cross-segment and cross-region communication is handled entirely by collaboration mode. The earlier "local server doubles as discovery node" option is deleted. Direct mode's usable range is decided entirely by its inherent nature (UDP broadcast does not leave the segment) and **needs no external coordination to deploy**.

Two positions from V6.0 carry over:

1. **Seven communication-experience features added to the inventory and phasing** (Chapter II): screenshot and annotation, stickers, frequent contacts, direct manager and reporting lines, contact visibility, office floor maps, remote assist — assigned to Phase 1 or 2 by dependency, each with implementation points and effort. Remote assist and floor maps involve permission control and site security and are separately covered in Chapters II and IX.
2. **The AI model access layer is a pluggable model gateway** (Chapter VI Section 5): unified OpenAI-compatible protocol, local models and external API endpoints both registrable; but archived data goes to local models only, external endpoints disabled by default and requiring explicit registration with an audit record. This lets Phase 3's software engineering proceed before hardware arrives, decoupling procurement lead time from development.

Three positions from earlier versions carry over:

1. **The technical route converges to "self-written business layer."** Open-source IM products generally use strongly contagious licenses (AGPL-3.0); modifying one and offering it as a service would force disclosing our own code, embedding a server-free subsystem into a server-centric client is an architectural conflict, and the modification effort is no less than writing from scratch. Chapter III details this.
2. **Scope expands from "three core features" to "modeled on WeChat Work."** Chapter II decomposes WeChat Work into an assessable list, orders four phases by dependency and effort, and marks two items recommended not to be self-written. **This converts "match WeChat Work" into a path that can be approved, accepted, and risk-controlled.**
3. **The account model is corrected to "multi-device login control."** An account can log in on different computers (machine changes allowed), but on desktop only one instance per account may be online — a repeat login forces the previous client offline; every login records device type and is retained. Chapter IV Section 5 details this.

**Table 2: Phase overview**

| Phase | Content | Effort | Delivery marker |
| --- | --- | --- | --- |
| Phase 1 | Communication and file-sharing core: dual-engine client, messaging, group chat, screenshot and stickers, frequent contacts, org structure and reporting lines, file transfer, message archiving and search | ~20–25 person-weeks | Can replace the current LAN tool, with archiving and search |
| Phase 2 | Office capabilities: approvals, calendar and to-dos, announcements, group tools, office floor map, remote assist, advanced permissions | ~11–14.5 person-weeks | Lightweight office collaboration |
| Phase 3 | AI capabilities: in-group assistant, meeting minutes, requirement structuring | ~6.5–8.5 person-weeks | Group-chat content auto-structured |
| Phase 4 | Audio/video conferencing and collaborative documents | Recommend separate project or open-source integration | Decided by Phase 2/3 operations |

**The review board is asked to confirm fifteen decisions** (full list in Chapter XI): (1) the self-written-business-layer route; (2) the four-phase plan and boundaries; (3) conferencing and collaborative documents not self-written (procure or integrate instead); (4) the "desktop single-instance, mobile coexistent" login policy; (5) whether logged-in users may communicate with non-logged-in colleagues (affects archive completeness); (6) chat retention period, **6 months recommended**; (7) single centralized server with no regional edge nodes (recommended); (8) archiving rules during server/line outages (local stash with post-recovery back-upload recommended).

(9) Whether the model gateway may register external API endpoints (default no; if allowed, non-archive data only, with full audit); (10) whether to start GPU procurement evaluation; (11) whether to implement network-layer admission control; (12) remote-assist exposure scope (IT operations only, with controlled-party explicit authorization and full tracing); (13) floor-map coverage and visibility (per campus and floor); (14) contact-visibility policy (all-visible by default, executives' details hidden as needed); (15) the source of direct-manager data (admin-console manual maintenance with bulk import recommended first).

---

## I. Project Background and Requirements Analysis

This chapter states the environmental constraints, decomposes requirements into acceptable items, recounts their convergence across rounds of discussion, and sets the design principles that run through the plan.

### 1. Project background

The intranet currently relies on a LAN IM tool for daily communication and file transfer. Three characteristics:

**Physically isolated from the public internet.** No internet access; any commercial messenger needing external servers, accounts, or online verification is unusable. This also dictates the AI approach — models must be deployed inside the intranet.

**Mixed terminal operating systems.** Windows and domestic Linux (UnionTech UOS, Kylin) are mixed; the client must cover both, and domestic adaptation issues usually outnumber Windows ones.

**The current tool is insufficient.** Usable for chat and transfer, but lacking org structure, message search, and office collaboration — clearly weaker than WeChat Work or Feishu-class products. Hence the goal "modeled on WeChat Work's scope."

Constraint: **feature parity with commercial products, implemented entirely on the intranet, independent of external services.**

### 2. Requirements list

**Table 3: Requirements and acceptance criteria**

| No. | Requirement | Priority | Acceptance criteria |
| --- | --- | --- | --- |
| R1 | Usable without login or server configuration | Must | After install, with no address entered and no account registered, auto-discovers same-segment terminals and exchanges messages; behavior consistent with FeiQ |
| R2 | Non-logged-in messages bypass the server | Must | Direct-mode messages are point-to-point; the server holds no record; verifiable by packet capture |
| R3 | After login, chat records are centrally retained and admin-searchable | Must | Collaboration-mode messages all land server-side; admins search by person, time, keyword; terminal-side deletion does not affect server retention; **no feature lets a sender erase archived content (burn-after-reading, recall-as-delete)** |
| R4 | Machine-change login and multi-device control | Must | An account logs in on different computers; one desktop online instance per account, the earlier one kicked with notice; each login records device type and identifier |
| R5 | Both modes in one client | Must | Single installer; direct form when not logged in, collaboration form after entering the server address and logging in; switching preserves local history |
| R6 | LAN file sharing | Must | Directory-level sharing with permission distinctions; file bytes never traverse the public internet |
| R7 | Archive scope must be identifiable | Must | Cross-mode sessions (one side not logged in) are clearly marked as not archived in the UI; the archive start point is traceable |
| R8 | Domestic OS support | Important | At least UnionTech UOS and Kylin Linux |
| R9 | Cross-segment and multi-region communication, large-file transfer | Important | Cross-segment/cross-region handled by collaboration mode: multi-region via dedicated line to one server, cross-region messages deliverable; direct mode stays in-segment, no cross-segment requirement; whole-directory send and resumable transfer supported |
| R10 | Scope modeled on WeChat Work | Important | Accepted item-by-item per the phase list, not all at once |
| R11 | Intranet AI | Planned | In-group assistant, meeting-minute generation, requirement structuring; models deployed intranet; data never leaves |
| R12 | Server high availability | Important | If the server fails, direct mode still works; operations has backup and recovery plans |
| R13 | Communication experience (screenshot, stickers, frequent contacts) | Important | Built-in screenshot with annotation sends directly; custom sticker import; frequent contacts pinnable and quickly findable |
| R14 | Org structure with reporting lines and contact permissions | Important | Member profiles show the direct manager; contact visibility settable by department; executives' details individually hideable |
| R15 | Office floor map | Planned | View floor plans and locate colleagues' seats; name search highlights on the map; visibility limited per campus and floor |
| R16 | Remote assist | Planned | Request to view or control a colleague's terminal; controlled party's explicit authorization required, process continuously visible to them, fully traced |

**Explicitly not provided** (single-listed because several are ubiquitous and popular in commercial products and could be mistaken for omissions). Not providing them is a matter of purpose, not capability — **the sole purpose of connecting to the server is to leave a trace; any feature that "lets the sender erase the trace" conflicts directly with that purpose:**

1. **Secret chat / burn-after-reading.** No "auto-delete after send" or "server doesn't keep" conversation form exists; all collaboration-mode conversations are archived, with no user opt-out.
2. **Recall that erases the archive.** Recall is kept but acts only on the client display layer — the server keeps the original content and send time and additionally records the recall action itself (who, when, which message). **Recall is "I don't want you to see it anymore," not "the system no longer retains it."**
3. **One-sided terminal deletion of the archive.** Employees may clear local cache; this does not affect server records; the UI must distinguish "local cache" from "cloud archive."
4. **A user-facing trace switch.** Retention policy is issued per department by management only; the client offers no off switch.

### 3. Requirement evolution and this version's convergence

The requirements converged over multiple rounds; the evolution matters for understanding the plan's shape. Initially "abolish accounts entirely, identity fully bound to hardware" — logically incompatible with centralized retention (without accounts the server cannot attribute messages). Then "login-free by default, account as optional enhancement," a single-channel design where even non-logged-in users went through the server — but then "login-free" no longer meant "server-free," contradicting the FeiQ-like expectation. **This version settles on the dual-engine form**, with boundaries clarified item by item:

1. **Not logged in = zero-configuration direct mode.** No server, UDP broadcast auto-discovery, point-to-point delivery — the expected "FeiQ form."
2. **Logged in = server collaboration.** Enter the server address; messages enter server archiving; org structure, cloud history, and search become available.
3. **Both forms in one client** (explicitly required, not two programs).
4. **Accounts support machine-change login** (explicitly required), with control: different device classes coexist, same class kicks, one online, device type recorded.
5. **Experience and org-info capabilities filled in**: six classes added (remote assist, screenshot, stickers, frequent contacts, org structure with superiors after login, office floor map), all included in phasing; contact visibility derived as an additional requirement.
6. **Direct mode converged to the local segment**: UDP broadcast does not cross segments (inherent network-device behavior, not a configuration issue); positioned as temporary, nearby use; no network-side changes; cross-segment/cross-region needs are met by logging into collaboration mode. The boundary is decided by natural property, not by a scheduled engineering task.
7. **Trace is the sole purpose; evasion-prone features are rejected**: "connecting to the server is for the trace" — secret chat, burn-after-reading, recall-as-delete, one-sided terminal deletion are all excluded. Direct mode's non-recording is an inherent property of its form; collaboration mode's recording is the purpose of the build; the former is not an on-demand hidden feature, the latter admits no technical exemption.

### 4. Design principles

**Principle 1, dual engines in parallel.** Two complete communication engines coexist in the client, each with its own links; they share no communication state, only the local data layer — eliminating "two-channel state sync," the most concentrated defect source in this class of system. The engines' scopes separate naturally: direct mode relies on UDP broadcast, so its scope is the local segment and temporary nearby use; cross-segment and cross-region always go collaboration.

**Principle 2, the archiving boundary is decided by the mode.** Collaboration-mode messages all land in the DB; direct-mode messages stay local. The admin-visible archive boundary is exactly "content produced after the employee logged in" — stable, predictable, explainable. **This boundary must be honestly communicated, not papered over technically.** Hard constraint: **once produced, the archive cannot be erased by the sender.** Recall changes only the client display; local cache clearing does not touch the archive; no user-facing trace switch. Anything conflicting is not built — if the trace can be self-exempted, the purpose collapses.

**Principle 3, cross-mode intercommunication must be explicitly marked.** Logged-in and non-logged-in users may intercommunicate, but that link bypasses the server and cannot be archived; such sessions must be visibly marked "not archived," with a server-issuable policy switch to prevent the channel being used to evade retention.

**Principle 4, self-write only what should be self-written.** Conferencing and collaborative documents are separate technical domains — high cost, hard quality guarantees; procure mature products or integrate open components within the allowed scope. This is the precondition for "modeled on WeChat Work" to be feasible.

**Principle 5, sensitive capabilities default tight.** Remote assist, contact visibility, and site maps take the most conservative default, loosened by management as needed. This does not contradict "parity with commercial products" — parity is of capability, not default permission. Remote assist is off for ordinary employees by default; contacts are all-visible by default but tightenable; the site map is visible only for one's own floor by default.

---

## II. Benchmark Function Inventory and Phased Planning

The core chapter. WeChat Work spans communication, office, collaboration, and conferencing with wildly differing effort; this chapter decomposes it into an assessable module list with dependencies and effort, then orders the phases.

### 1. Inventory method

Three rules: only inventory features meaningful on an intranet (payments, mini-programs, external-customer management that depend on the public-internet ecosystem are out); mark each module's dependencies (e.g. "message archiving" precedes "message search" and "AI conversation analysis"); mark the recommended implementation (self-write / integrate open source / procure), with the latter two excluded from self-development effort.

### 2. Function module inventory

**Table 4: Function modules modeled on WeChat Work**

| Domain | Module | Notes | Depends on | Implementation |
| --- | --- | --- | --- | --- |
| Communication | 1:1 chat | Text, image, file, quote, recall (recall affects the client display layer only; server keeps the original and the recall record) | — | Self-write |
| Communication | Screenshot and annotation | Doodle, arrows, mosaic after capture, direct send | 1:1 chat | Self-write |
| Communication | Stickers and packs | Built-in, custom import, frequently-used pinned | 1:1 chat | Self-write |
| Communication | Frequent contacts | Pin, star, recent, frequency sort | Contacts | Self-write |
| Communication | Group chat | Create, invite, announcement, @member, group files | Org structure | Self-write |
| Communication | Message status | Read receipts, send status, presence | — | Self-write |
| Communication | Direct mode | Login-free point-to-point | — | Self-write |
| Communication | Multi-device login | Desktop single-instance, mobile coexistence, device management | Accounts | Self-write |
| Communication | History | Roaming, search, cloud backup, machine-change migration | Archiving | Self-write |
| Communication | Remote assist | Authorization request, screen view, remote control, tracing | 1:1 chat, org structure | Self-write |
| Structure | Org structure | Multi-level departments, profiles, titles | — | Self-write |
| Structure | Reporting lines | Direct-manager field (one per person), chain view | Org structure | Self-write |
| Structure | Contacts | Department tree, search, member detail | Org structure | Self-write |
| Structure | Contact visibility | Hide departments/members, restrict cross-department view, hide sensitive fields | Permissions | Self-write |
| Structure | Office floor map | Floor plan, seat points, name search, visual maintenance | Org structure | Self-write |
| Structure | Permission system | Roles, visibility scopes, admin tiers | Org structure | Self-write |
| Archiving | Message archiving | Collaboration-mode messages fully persisted | — | Self-write |
| Archiving | Search and export | By person/time/keyword; export as evidence | Archiving | Self-write |
| Archiving | Session audit | Access permission, access log, compliance trace | Search | Self-write |
| Files | P2P transfer | Files and folders, resumable, large files | — | Self-write |
| Files | Transfer metadata archiving | Name, size, hash, parties, time | Archiving | Self-write |
| Files | Shared space | Department shares, permissions, versions | Org structure | Integrate (Samba / web file platform) |
| Office | Calendar | Personal and shared, meeting invites | Org structure | Self-write |
| Office | To-dos | Assignment, reminders, status tracking | Org structure | Self-write |
| Office | Announcements | Publish, read stats, pin | Org structure | Self-write |
| Office | Approvals | Form design, workflow engine, CC, archiving | Org structure | Self-write |
| Office | Daily/weekly reports | Templates, submission, stats | Org structure | Self-write |
| Group tools | Group bot | Webhook, push, custom replies | Group chat | Self-write |
| Group tools | Poll / sign-up chain | Quick in-group initiation and tally | Group chat | Self-write |
| Group tools | Group tasks | In-group assignment and tracking | Group chat | Self-write |
| Collaboration | Online documents | Co-editing, comments, versions | Org structure | Integrate (OnlyOffice / Collabora) |
| Collaboration | Online sheets | Collaborative spreadsheets | Org structure | Integrate (same) |
| Collaboration | Knowledge base | Document tree, search, permissions | Online documents | Integrate |
| Conferencing | Audio/video | Multi-party realtime A/V, screen share | — | Procure or integrate (LiveKit / Jitsi) |
| Conferencing | Minutes | Transcription, summary, action extraction | A/V, AI | Self-write (AI part) |
| AI | In-group assistant | @ in group, answers from group history | Archiving, model deployment | Self-write |
| AI | Minute generation | Structured minutes from chat or transcript | Archiving, model deployment | Self-write |
| AI | Requirement structuring | Chat to structured requirement items | Archiving, model deployment | Self-write |
| AI | Smart search | Natural-language query over history | Archiving, vector retrieval | Self-write |
| Management | Admin console | Members, departments, permissions, devices, policies, audit | All | Self-write |
| Management | Device ledger | Identification, owner registration, enable/disable/unbind | Device fingerprint | Self-write |
| Management | Policy issuance | Login-free switch, cross-mode switch, new-device approval | Admin console | Self-write |

**Exclusions.** Four feature classes are not listed and explicitly not built, all conflicting with the trace purpose: **secret chat/burn-after-reading; recall that erases the server archive; one-sided terminal deletion; a user-facing trace switch** (item-by-item in Chapter I Section 2). These are ubiquitous and popular in commercial products; not providing them is purpose, not incapacity. If the review board disagrees, the premise "full collaboration-mode retention" itself must be revisited — adding them back destroys the trace's verifiability. One accompanying convention: recall and deletion actions themselves enter the archive as independent events, so "the message later disappeared" is itself traceable.

### 3. Phase matrix

**Table 5: Phase matrix**

| Phase | Goal | Modules | Effort |
| --- | --- | --- | --- |
| **Phase 1**<br>Communication and file-sharing core | Replace the current LAN tool, with archiving and search | 1:1 chat, screenshot and annotation, stickers, frequent contacts, group chat, message status, direct mode, multi-device login, org structure, reporting lines, contacts, contact visibility, permission system, message archiving, search and export, P2P transfer, transfer metadata archiving, admin console, device ledger, policy issuance, shared space (integrated) | ~20–25 person-weeks |
| **Phase 2**<br>Office capabilities | Lightweight office collaboration | Approvals, calendar, to-dos, announcements, daily/weekly reports, group bot, poll/sign-up chain, group tasks, session audit, office floor map, remote assist | ~11–14.5 person-weeks |
| **Phase 3**<br>AI capabilities | Group-chat content auto-structured | In-group assistant, minute generation, requirement structuring, smart search | ~6.5–8.5 person-weeks (+ hardware) |
| **Phase 4**<br>Conferencing and collaboration | Complete the collaboration surface | Audio/video conferencing, online documents, sheets, knowledge base | Recommend separate project |

Phase rationale: Phase 1's boundary is "replace the current tool and solve archiving" — the project's foundation and the most verifiable part. Phase 2 is the "pleasant to use" part — independent, incrementally deliverable, lower risk; the newly added floor map and remote assist are relatively heavy (the former hard in data maintenance, the latter in permission control and multi-platform adaptation). Phase 3 depends on GPU hardware and on Phase 1 archiving, so it cannot be earlier. Phase 4's items are separate domains with weak architectural coupling — last, independently approvable, not affecting prior value.

### 4. Implementation points of the six experience features

**Screenshot and annotation.** Mature local capability: capture, overlay doodle/arrow/mosaic/text, confirm into the conversation. **The hard parts are three adaptation details**: region selection under multi-monitor, pixel alignment under high DPI, and screen-capture API differences on domestic Linux (X11 vs Wayland; Kylin and UOS versions differ in default session type — each must be tested). ~0.5–1 person-week.

**Stickers.** Three layers: built-in set, custom packs (imported images/gifs sent as file identifiers), frequently-used sorted by frequency. **No sticker store** — it needs content sourcing and review, meaningless intranet. ~0.5–1 person-week.

**Frequent contacts.** Looks like sorting, is really data ownership: frequency and recency must be recorded with manual pin/star; small effort but **the data must live server-side** — machine-change login is confirmed, and a local-only list would be judged a defect. In direct mode the list stays local and merges into the cloud after login. ~0.5 person-week.

**Reporting lines (direct manager).** Easily mistaken for a free field on the department tree; it is **data independent of departmental affiliation**. Modeled on WeChat Work: an independent profile field, at most one per person, settable per person in the admin console or bulk-imported from a roster. **Without it, "see your own manager" is impossible** — you could only show the department head, and the affiliated department and the actual reporting target are often different people. Needs: a direct-manager column with bulk import and chain viewing. **The precondition is the data source** — manual admin maintenance or HR/OA sync must be decided by the board (Chapter XI). ~1–1.5 person-weeks, excluding external-system integration.

**Contact visibility.** Derived from "who sees whom, which fields." Three controls: hide specified departments/members (hidden ones invisible and unsearchable, whitelist exceptions possible); restrict a department to its own contacts; hide specific fields for specific people (e.g. executives). Compliance-relevant — **contacts contain phone numbers and the like; default all-staff exposure is itself a data-protection gap**. ~0.5–1 person-week.

**Office floor map.** Clear approach: upload floor plans (image or vector), annotate seats/offices bound to members; search by name to highlight and zoom, or click a point to reverse-lookup; multi-campus, multi-floor switching; drag-and-drop point editor in the admin console. **Development is easy (~1.5–2 person-weeks); two things are hard.** First, **data maintenance is a long-term burden** — seats change, departments move, plans get redrawn; if every change needs a developer, the feature will be abandoned within months. The point editor is a **necessary component**, and a maintenance owner (usually admin or IT support) must be named. Second, **site plans are sensitive** — full floor layouts, security corridors, key equipment rooms are not fit for all-staff visibility. Recommend default own-floor only, cross-floor/cross-campus by authorization, fully closed for security/confidential posts. Listed as a decision item.

**Remote assist.** The heaviest and most boundary-sensitive addition. Three technical tiers: **read-only** (view only, ~1.5–2 person-weeks); **controllable** (adds mouse/keyboard injection and clipboard sync, per-platform, ~3–4 person-weeks); **unattended** (connect without the controlled party present) — **not recommended**, risk far exceeds benefit. Phase 2 should do "screen view + authorized control" only, **open by default to IT operations only**.

Note: mature open-source self-hosted options exist and are all rejected. RustDesk is AGPL-3.0 — linking it into our client would drag our code into open-source obligations, conflicting with the "no open-source rebuild, own-code control" route; this project **references its architectural layering only** (signaling/relay/codec separation, controlled-side confirmation), copying no code. Three permissively licensed alternatives were also evaluated — **MeshCentral** (Apache-2.0: agent-based, closest to RustDesk, covering remote desktop/terminal/file management), **Apache Guacamole** (Apache-2.0: browser gateway over RDP/VNC/SSH, no agent needed), **FreeRDP** (Apache-2.0: client-side RDP library, safe to link). All three are excluded **not for licensing but for conflict with hard requirements**: RDP/VNC authorization is OS-level login and cannot carry "controlled-side local dialog showing the initiator's name and department, consented by click"; Windows RDP login takes over and locks the local session, so the controlled party cannot see what the operator does — the opposite of "continuously visible." MeshCentral is a full remote-management platform with its own server/console/account system that cannot integrate with our IM accounts and assist-trace tables; integration would cost more than self-development. The conclusion stands: **remote assist is self-write-or-nothing**, with scope fixed as: the 3–4 person-weeks cover **controlled-side screen capture and input injection, the authorization interaction, and server-side operation tracing**; **the media transport layer is excluded**, reusing the WebRTC foundation (Chapter IV Section 10).

Four hard requirements to write into the dev task brief:

1. **Explicit confirmation by the controlled party.** On request, the controlled terminal pops a dialog showing the initiator's name and department; consent is clicked locally; no silent connection of any kind.
2. **Continuously visible to the controlled party.** A clear status indicator stays during the session, with one-click interrupt at any time; hiding it is unsupported.
3. **Full tracing.** The server records initiator, controlled party, start/end, duration, operation class; screen recording on by default as audit evidence.
4. **Scope control.** Department-level switches from the admin console; default may be "IT department only"; HR/finance/confidential posts may be set non-controllable.

**Unlike the other features**: those are data flow; remote assist is cross-terminal operation permission — abuse impacts far more than one message leak. Chapter IX covers its compliance in parallel.

**Subtotal.** Phase 1 four items ~3–5 person-weeks (screenshot 0.5–1, stickers 0.5–1, frequent contacts 0.5, reporting lines 1–1.5, contact visibility 0.5–1); Phase 2 two items ~4.5–6 person-weeks (floor map 1.5–2, remote assist 3–4).

### 5. Two features recommended not to be self-developed

**Audio/video conferencing.** Media servers (SFU), signaling, codecs, echo cancellation, jitter buffering, weak-network resistance — a separate, high-threshold domain; 8–15 person-weeks would yield only "barely usable." **Recommend**: procure a commercial product with an intranet deployment edition, or integrate an open media server (LiveKit, Jitsi, Mediasoup) with this project doing SSO and UI integration. Note integrating open components differs in nature from "rebuilding open-source software" — deployment and invocation do not involve distributing our code and are not bound by contagious clauses, though specific license terms must still be checked. **Clarification: what is not recommended to self-write is the "conferencing system" layer, not WebRTC itself** — those servers are themselves built on WebRTC, adding SFU, signaling, room management, recording on top. Position: **this project owns the media-transport foundation (Chapter IV Section 10) and does not redevelop the conferencing layer**; "not using WebRTC" and "not self-writing conferencing" are two different judgments on two levels.

**Collaborative documents (online docs/sheets/knowledge base).** The core is collaborative editing (OT or CRDT), rich-text editing, realtime sync — again a separate domain. **Recommend**: deploy OnlyOffice Document Server or Collabora Online, embed via page into the file module, with unified identity and permissions. Their licenses differ (AGPL-family vs MPL-family); confirm compliance for internal use before adopting.

**Keeping these two out of self-write scope is the key precondition for on-time delivery.** If the board requires self-development, schedule and investment need re-estimation and separate project status.

### 6. Phase conclusion

"Match WeChat Work" is a direction, not a requirements list. Split into four phases, the project becomes four bounded, independently acceptable build tasks:

- **Phase 1 answers "can it replace"** — the hard need and the best ROI;
- **Phase 2 answers "is it good enough"** — office collaboration, incremental, risk-spread;
- **Phase 3 answers "can it save effort"** — AI, dependent on hardware and archives;
- **Phase 4 answers "full coverage"** — recommended not self-developed; procure or integrate.

The floor map and remote assist land in Phase 2, not Phase 1, for two reasons: they do not affect replaceability, so Phase 1 ships without them; and remote assist involves cross-terminal operation permission, best opened after Phase 1 lands and management rules mature — capability should not precede policy.

The board is asked to review on these boundaries, accept Phase 1 as its own completion, and approve each later phase separately.

---

## III. Why Not to Rebuild on an Open-Source Solution

The route rationale. Open-source rebuild is confirmed rejected; this chapter gives the reasons for board re-checking.

### 1. Available open-source options and their positioning

Domestic open-source IM projects usable for intranet office, by deployment form:

**Table 6: Open-source options by deployment form**

| Class | Representative projects | Server needed | Records stored | Identity source |
| --- | --- | --- | --- | --- |
| Server-free LAN direct | 茶话间 (Teahouse), iptux, BeeBEEP, nwChat, LAN Messenger | No | Local terminal | The device itself |
| Self-hosted server | 喧喧 (Xuanxuan), Mattermost, Rocket.Chat, Zulip | Yes | Server | Account |
| SDK / infrastructure | OpenIM, 野火IM, 悟空IM, MobileIMSDK | Yes | Server | Account; client self-written |

**Server-free** (茶话间, nwChat): pure LAN P2P, UDP broadcast discovery, 1:1 and group chat, file/folder transfer with resume, shared file cabinets, adapted to UOS and Kylin — highly consistent with our "direct mode," and **usable as a reference implementation for the direct engine**. Limitation: no server, records local-only, lost on reinstall or manual deletion, no admin visibility — cannot satisfy R3.

**Self-hosted server** (喧喧 representative): server retention, AGPL-3.0, Go server, Electron/React client, pure-LAN capable, with org structure, permissions, monitoring, Xinchuang adaptation. **Closest to our needs, but with two hard flaws** (below).

**SDK type** (OpenIM, 野火IM): server and per-platform SDKs but no ready-made full client; admin console also self-built — effort close to writing from scratch.

### 2. Four concrete obstacles on the rebuild path

**Obstacle 1, license contagion.** 喧喧 is AGPL-3.0, strong copyleft: offering a modified version over a network requires disclosing all modified source. Pure internal use is not directly blocked, but two real annoyances remain — any future external extension is constrained, and internal code must be managed to open-source standards, possibly conflicting with corporate code-asset rules. Mattermost/Rocket.Chat (MIT) avoid this but are weaker in Chinese UX and Xinchuang fit, shifting the effort elsewhere.

**Obstacle 2, architectural conflict.** Our core form is "two engines in one client." 喧喧's client is built around "connected to a server": session model, state management, message flow all depend on that connection. Supporting a login-free direct mode would require an entire parallel subsystem inside the client — server-independent state, UDP discovery and P2P transfer, a local message store with source marking, cross-mode session detection — **almost no reuse of the existing architecture**: a new app inside an old app, plus integration cost. The effort is no less than self-development, with added external dependency.

**Obstacle 3, the particularity of our requirements.** Commercial-grade org structure, permissions, and Xinchuang adaptation are mature, but our core demands concentrate on "dual engine + archiving boundary + cross-mode marking" — exactly what no existing open-source project implements. Reusable capability lies in the non-core zone; the non-reusable lies in the core.

**Obstacle 4, scale and maintenance cost.** Some free editions cap headcount (喧喧 free: teams under 50); beyond that, paid licenses. And modified code must track upstream merges forever — a long-term burden.

### 3. Conclusion

**No open-source rebuild; self-written business layer.** Distinguish two levels: "not rebuilding open-source software" means **not refitting someone's complete product into ours**; "using open components" — invoking a model, deploying a media server, linking a protocol library — is entirely different in nature and, with permissive licenses, is not just allowed but the main cost-reduction lever. Sections 1–2 argued the former fails; Section 4 bounds the latter. Open-source IM is used **as architecture reference and implementation reference only**, copying no code — hence no license constraints. Appendix B lists the referenced projects.

### 4. Component-level licensing discipline

A finer constraint governing **components**, not whole software: **integrate no strongly contagious (AGPL/SSPL) component — no dynamic or static linking, no merged compilation, no modified redistribution.**

Called out separately because it constrains everyday technical choices yet is easily waved off as "open source isn't free, mind compliance" with no one executing. All open-component usage in this plan complies:

**Table 7: Open-component usage list and license nature**

| Use case | Content | License | Introduction |
| --- | --- | --- | --- |
| AI models | Qwen series, DeepSeek series | Apache-2.0 / MIT | Deployed invocation; no modification, no distribution |
| Speech recognition | FunASR (DAMO Academy) | Apache-2.0 | Deployed invocation |
| Media transport (standard) | WebRTC | BSD-3-Clause | Standards/protocol interop |
| Media transport (Qt route) | libdatachannel | MPL-2.0 | Linked; file-level weak contagion, allowed |
| A/V conferencing | LiveKit / Jitsi | Apache-2.0 | Deployed integration |
| Collaborative documents | OnlyOffice Document Server | AGPL-3.0 | Deployed integration; exception judged below |

Three notes. **First, the dividing line of strength is "whether a derivative work is formed."** Apache-2.0/MIT/BSD are permissive — free linking and distribution with notice retention; MPL-2.0 is file-level weak contagion — disclosure required only for modified source files, linking unrestricted; AGPL-3.0 is strong contagion — obligations cover the whole program on linking or merged compilation, triggered for network services too. **Second, collaborative documents are the sole exception in the table and need separate judgment**: OnlyOffice is AGPL, but this project only **deploys and invokes its service** — no source modification, no linking into our client — so no derivative work; coexisting as an independent program is "aggregation," not "merging," essentially different from linking RustDesk. **The judgment holds only while the process boundary is maintained** — modifying its source or embedding it into our process voids the premise and requires re-evaluation. Write this into the dev brief and verify at delivery via the component list. **Third, this discipline explains Appendix B's listing stance**: it contains several AGPL items with different statuses — **RustDesk and 喧喧 explicitly excluded, architecture reference only; OnlyOffice an independently deployed integration object** — marked item by item.

---

## IV. Overall Architecture Design

Overall architecture: the dual-engine model, shared kernel, cross-mode intercommunication rules, account and multi-device model, archive-state marking, layered feature permissions, the file-channel bypass design, and the media-channel foundation choice.

### 1. The dual-engine model

Two independent communication engines coexist in the client.

**Direct engine (no login).** No server dependency. On start it announces itself via UDP broadcast, receives peer announcements, and builds the online device list; messages and files go TCP point-to-point. With no central node, its messages **are written only to the local DB; the server holds nothing** — R2's direct requirement. Security: a custom protocol with end-to-end encryption recommended (ECDH key agreement plus symmetric encryption), not IPMsg's plaintext frames.

**Collaboration engine (logged in).** Enabled after the user enters the server address and logs in. A long connection; messages relay through and persist on the server; contacts, org structure, groups, cloud history come from the server. Its messages **persist permanently server-side**; local data degrades to cache.

**Both engines run simultaneously.** Logging in does not stop the direct engine, because cross-mode intercommunication (Section 4) requires logged-in users to keep discovering non-logged-in colleagues. Counter-intuitive versus "the two never interfere after login" — must be made explicit in development.

### 2. Shared kernel

Above the engines, one shared data and interaction layer. **Local message store**: a single SQLite DB; the message table distinguishes direct vs cloud records by a source field and marks cloud messages' archive state by a sync-status field — the technical basis of "switching modes loses no history." **Session list**: one merged view, direct and cloud sessions interleaved by time, each carrying an archive-state marker; no source filter toggle, to prevent users misjudging retention by filtering. **Contacts view**: merged sources — cloud org structure plus direct-engine online devices; the same person in both (logged in and broadcasting) merges into one entry per mapping rules. **File transfer module**: both forms share one implementation; collaboration-mode files likewise go P2P, not through the server, with only transfer metadata reported.

### 3. Runtime states and switching

**Direct state (default).** Entered right after install, no input; direct engine active; same-segment discovery and contact; history local. **Collaboration state (optional).** Entered after entering the address and logging in; org structure, groups, cloud history appear; messages begin archiving. Logging out returns to direct state; local history kept; no new archive records. **Degraded state.** When the server is unreachable, the client falls back to the direct engine — same-segment messaging and file transfer still work, but nothing archives. **A server outage thus affects only archiving, org structure, and cloud history — not basic communication**, fundamentally different from single-channel architectures where "server down = everyone offline."

### 4. Cross-mode intercommunication rules

Logged-in and non-logged-in users may intercommunicate; the link auto-degrades to P2P. This is the design the board must understand best, as it decides archive completeness.

**Reachability.** The logged-in user's direct engine keeps running and discovers direct-state terminals; non-logged-in terminals see the "logged-in colleague" via broadcast identifiers carrying account info — displayed as a real name, not a strange device. **Message path.** Cross-mode messages **bypass the server and the archive**, written only to both local DBs — identical to that session in direct mode. **UI marking.** Cross-mode sessions show an "not archived" marker beside the title and fixed explanatory text above the input box; the marker cannot be hidden, so the user sees at every input that this conversation is not retained. **Archive start point.** A session may start cross-mode (unarchived) and become collaborative after the other party logs in; the server then archives **messages after login**, and the admin console marks "archived since <time>" at the record start. **Archiving is decided per message, not per session** — a definition that must be explicit.

**Risk and countermeasures.** The channel could be abused to evade retention ("log out and we'll chat direct"). Three counters: a "allow communication with non-logged-in devices" policy switch, off for high-retention departments; the client logs cross-mode session establishment (time, parties, duration) to the server — **not message content, but the fact that an unarchived communication occurred**, so abnormally frequent cross-mode traffic becomes visible; state the retention boundary honestly in employee communications.

### 5. Account and multi-device login model

Corrected this version to **WeChat-Work-style multi-device control** (previously misread as "account locked to one terminal").

**Loggability.** Any registered device, unlimited machine changes; the client collects a device fingerprint at login for server-side identification and record. **Terminal classes.** Desktop (Windows, domestic Linux, macOS as one class), mobile, web. **Online policy.** Desktop: **one online instance per account** — logging in on a second computer force-logs-out the first with clear notice; desktop and mobile may coexist. Matches WeChat Work: machine-change usability without multi-desktop state confusion. **Login records.** Account, fingerprint, device type and name, time, source address, client version are retained; users get a "login device management" entry to view and remotely log out; admins can query all records.

**The fingerprint's role changes accordingly** — no longer "lock the login," but three uses: direct-mode identity; the device ledger (device–user mapping); login records and anomaly detection (same account on many devices in a short window). Since credentials are not machine-bound, **the "new computer can't log in, admin must unbind" problem does not exist** — operations cost drops visibly. **First-login control for new devices**: keep a "new device requires approval or email confirmation" switch to prevent password-only misuse on陌生 devices.

### 6. Archive-state marking

Since archive scope depends on session form, the UI must give certainty about "is this conversation retained."

**Table 8: Session archive-state marking**

| Session type | UI marker | Meaning |
| --- | --- | --- |
| Both logged in | None (default) | Via server; all messages archived |
| One side not logged in (cross-mode) | "Not archived" shown | P2P; both local; no server record |
| Was cross-mode, now collaborative | "Archived since X" | Post-switch messages archived; earlier ones not |

The marker serves both users (clear retention scope, consistent with employee communications) and admins (why some conversations are absent from the console — not a missed record).

**On recall, one wording discipline**: after recall the client no longer shows the message, but **archive state and content are unchanged** — the server keeps the original plus a recall event. The recall prompt must say "message recalled," **never "deleted" or "no longer saved"** — consistent with Chapter I Section 2: recall yes, recall-as-erasure no.

### 7. Layered feature permissions

**Table 9: Feature permissions by runtime state**

| Feature | Direct (not logged in) | Collaboration (logged in) |
| --- | --- | --- |
| 1:1 text | Yes | Yes |
| P2P file transfer | Yes | Yes |
| Talk to non-logged-in colleagues | Yes | Yes (per policy switch) |
| Contacts | Same-segment online devices only | Full org structure |
| Group chat | Yes (server-free temporary groups) | Yes (announcements, files, tools) |
| Shared space | Read-only | Role-based read/write |
| Private cloud drive | No | Yes |
| Cloud history sync | No | Yes, migratable across machines |
| Messages enter server archive | No | Yes |
| Multi-device login | N/A (no account) | Yes; desktop single-instance |

The layering also serves management: it makes login genuinely rewarding — cloud history, no loss on machine change, org structure, private drive, group tools exist only after login — nudging employees toward account binding and completing the device-ledger-to-person mapping over time.

### 8. Policy switches

Three switches issued per department from the admin console:

**Table 10: Policy switches**

| Switch | Default | Effect | Scenario |
| --- | --- | --- | --- |
| Allow login-free use | On | Off: non-logged-in clients cannot enter the main UI; degrades to a conventional enterprise IM | High-secrecy departments and posts |
| Allow communication with non-logged-in devices | On | Off: logged-in clients stop responding to cross-mode discovery and messages; cross-mode sessions impossible | Departments with strict archive-completeness needs |
| New-device login requires approval | On | First login on an unknown device requires admin approval or email confirmation | All departments |

Combined: all on = loosest, most FeiQ-like; the second off = no archive gap but no reaching non-logged-in colleagues; the first off = that department degrades to mandatory accounts.

### 9. The file-channel bypass design

Archiving files literally would push hundreds of MB to GB of bytes through the server — disk and bandwidth become the bottleneck and the experience drops below removable media. So file transfer uses a **bypass design** in both forms: byte streams go directly between parties on the LAN, never through the server; in collaboration mode the server receives only **transfer metadata** — file name, size, hash, sender, receiver, initiation time, accepted or not — enough to prove "who transferred which file to whom, when," sufficient for accountability. Archiving the file body itself is optional, enabled per department. Direct mode is segment-bound; cross-segment transfer goes collaboration mode, or an opt-in server-relayed fallback path, off by default and gated by a size threshold when explicitly needed.

### 10. Media-channel foundation choice

**Status boundary:** this section specifies the target architecture and is not a statement of delivered functionality. In the current implementation, remote assistance sends JPEG screen frames through server-side HTTP polling; the WebRTC library and media connection path have not been integrated. The WebRTC foundation below is a planned follow-up.

In the target architecture, all media capability — remote-assist streaming, conferencing media, screen sharing — shares one transport channel, **unified on WebRTC as the standard foundation**, not implemented twice.

Three reasons. **First, it solves exactly the part that should never be self-implemented**: media transport (SRTP/DTLS), NAT traversal (ICE/STUN/TURN), congestion control and adaptive bitrate — problems stabilized by long engineering accumulation; self-implementation is unrealistic and adds no differentiation. **Second, marginal cost is minimal in our network**: same-segment terminals connect with host candidates alone, not even STUN; cross-segment/cross-region, the centralized collaboration server of Chapter VIII can **double as the TURN relay** — no extra media server. **Third, clean licensing**: WebRTC is a W3C/IETF standard, reference implementation BSD-3-Clause, fully consistent with Chapter III Section 4 — the very precondition for bypassing RustDesk (AGPL-3.0) and self-implementing remote assist.

But **what WebRTC does not provide** must be equally explicit — it is a pipe, not the capabilities at either end:

**Table 11: WebRTC capability boundary and how this plan obtains each**

| Capability layer | WebRTC provides? | How this plan obtains it |
| --- | --- | --- |
| Media transport (A/V, screen streams) | Yes | Direct use |
| Data channels (control, clipboard, small files) | Yes | Direct use |
| NAT traversal (ICE/STUN/TURN) | Yes | Same-segment direct; cross-segment via the collaboration server as relay |
| Congestion control / adaptive bitrate | Yes | Direct use |
| Screen capture | No | Per-OS implementation: Windows, macOS, domestic Linux each |
| Keyboard/mouse injection | No | Per-OS implementation; remote assist only |
| Multi-party forwarding (SFU) | No | Not self-developed; integrate LiveKit or Jitsi (Chapter II Section 5) |
| Authorization confirmation and operation tracing | No | Self-implemented; records into the server remote-assist table |

This fixes remote assist's self-development scope: **transport and traversal reuse WebRTC; the self-written parts are controlled-side screen capture, input injection, the authorization interaction, and server-side operation tracing.** The 3–4 person-weeks of Chapter II Section 4 cover exactly this, transport excluded.

**Media-stream retention stance made explicit.** "Connecting to the server is for the trace" applies to media with an object distinction: **media content itself is not stored** — a 30-minute call or assist recording is unaffordable in storage and bandwidth, the same reasoning as not archiving file bytes. Instead, **the fact of the communication is archived as metadata**: participants, start/end, duration, connection type (direct/relayed), media type. Remote-assist screen recording is an optional department-issued switch; recordings stored separately under the same retention period as archived messages. The reasoning matches the file channel exactly: **the purpose of the trace is auditability and evidentiary value, not keeping every frame.** The metadata answers "who communicated with whom, when, in what manner" — precisely the information level audit needs.

---

## V. Solution Design

Concrete design: module breakdown, data model, client technology selection, effort estimates, resource needs.

### 1. Module breakdown

**Client**, five modules: direct engine, collaboration engine, shared kernel (local store, sessions, contacts, file transfer), UI layer, system-integration layer (tray, notifications, autostart, multi-device kick). Above the UI, a set of tool components for the added experience features: screenshot and annotation, sticker management, frequent contacts, floor-map view, remote-assist session window. **Remote assist is the only client part requiring per-OS implementation** — screen capture and input injection differ across Windows, macOS, and domestic Linux and cannot share code.

**Server**, six modules: access gateway (long connections and message routing), accounts and devices (authentication, device ledger, multi-device control), org structure (departments, members, roles, permissions), messaging and archiving (storage, search, export), policy service (switch issuance), file-metadata service.

**Admin console**, four parts: member and department management, permission and role configuration, device ledger and login records, message search and audit.

### 2. Data model

Core tables. **accounts**: account id, name, employee no., department, **direct manager** (independent field, at most one per person — the sole data source for "see your own manager," not derivable from department), role, status, password digest. **devices**: fingerprint hash, type (desktop/mobile/web), name, OS, bound user, first-seen, last-active, status (normal/disabled/pending approval). **sessions**: session id, account, device, device type, login time, expiry, source address, client version, online state, kick reason — the basis of R4. **departments**: id, parent, name, order, head. **conversations**: id, type (1:1/group), name, creator, created time, archive-start marker. **conversation_members**: conversation, member, member type (account/device), join time, role (owner/admin/member), muted. **messages**: id, conversation, sender, type, body, send time, delivery status, **recall flag and time**; for R3, collaboration-mode messages **persist permanently server-side**; **recall only sets a flag — the body is neither modified nor cleared**; original content and time unchanged; **direct-mode messages never enter this table** — an architecture-decided boundary, not an implementation trade-off. **transfers**: file name, size, hash, sender, receiver, initiation time, accepted, completion time — separate from messages because metadata fields differ and retention may be tuned independently. **policies**: department, policy item, value, effective time — per-department switches including login-free, cross-mode, new-device approval, remote-assist scope, floor-map visibility. **floor_maps**: map id, campus, building, floor, image path, version, visibility, upload time. **seat_points**: point id, map id, member, x/y, type (seat/office/meeting room), note — bound points support both "search by name" and "click point to find person." **assist_sessions**: session id, initiator, controlled party, start, end, duration, operation class (read-only/controllable), authorization method (local confirm/admin-approved), recording id, interruption reason — the sole basis of remote-assist audit; **append-only, never modified or deleted**.

### 3. Client technology selection

Three candidates. **This version adds a fourth dimension beyond efficiency, package size, and domestic-OS fit: media capability — WebRTC availability in the target environment.** It decides whether remote assist, conferencing, and screen sharing can land, and the candidates differ by an order of magnitude: one "out of the box," one "extra but controllable effort," one "unusable on domestic systems."

**Qt (C++ or QML).** Strengths: mature networking and local-store support for the always-on UDP service and P2P TCP; FeiQ and nwChat both took this route with many referenceable implementations; fewest deployment problems on domestic Linux; small footprint and low memory, fit for old low-spec terminals. Weakness: UI development slower than web stacks. **Media: bring in libdatachannel (MPL-2.0, safe to link) or self-compile the WebRTC reference implementation (BSD-3-Clause), ~1–2 person-weeks added** — buying full control of the media chain; screen capture and input injection are per-OS anyway, independent of the transport library. **If terminal hardware spans widely and domestic systems dominate, evaluate this first.**

**Tauri (Rust + web frontend).** Small footprint, low memory, fast web UI, networking via Rust. **But one blocking risk must be verified first:** on Linux, Tauri uses the system WebKitGTK, and mainstream distributions compile WebKitGTK without WebRTC by default — `RTCPeerConnection` is undefined; reported on Deepin, upstream still pending. **So the Tauri route cannot use WebRTC on domestic Linux terminals** — a mandatory coverage target. Media would have to be re-implemented in Rust (e.g. webrtc-rs), with cost and uncertainty re-estimated, against the original efficiency intent. **Not recommended given explicit domestic-system coverage requirements.**

**Electron (Node + web frontend).** Highest efficiency, most mature ecosystem, **media out of the box** — Chromium's built-in WebRTC and desktop-capture APIs, no extra effort. Weakness: footprint and memory clearly larger; needs real-device testing on old domestic terminals — a risk to rule out by measurement, not a structural obstacle.

**Overall conclusion: Qt first choice, Electron second; Tauri not recommended given explicit domestic-system coverage.** The two weaknesses differ in kind — **Electron's size/memory is measurable and excludable by hardware survey; Tauri's missing WebRTC on domestic systems is blocking and unfixable by investment.** If phasing puts media features in Phase 2+, Electron could deliver Phase 1 quickly and migration be weighed later — but migration is costly and this should not be the default path.

### 4. Effort estimates

**Table 12: Phase 1 effort estimate**

| Work item | Effort (person-weeks) | Notes |
| --- | --- | --- |
| Direct engine | 2–2.5 | UDP discovery, P2P messages and files, encryption, local store |
| Collaboration engine | 1.5–2 | Long connection, message sync, offline messages, reconnect |
| Client UI | 2.5–3 | Sessions, group chat, contacts, org structure, settings, device management |
| Server | 3–3.5 | Gateway, accounts and devices, org structure, messaging and archiving, policy |
| Admin console | 2.5–3 | Members, departments, permissions, device ledger, login records, search and export |
| File transfer and shared space | 1.5–2 | Transfer, resume, shared-directory integration |
| Communication-experience features | 1.5–2.5 | Screenshot and annotation, stickers, frequent contacts; multi-monitor and high-DPI adaptation, UI integration |
| Org info and contact permissions | 1.5–2.5 | Direct-manager field and bulk import, chain view, visibility and field hiding |
| System integration | 1 | Tray, notifications, autostart, kick notice |
| Network and multi-region access adaptation | 0.5–1 | Org-structure local cache and incremental sync, history pagination; if outage back-upload is adopted, also the local stash queue and per-message-id dedup. Direct mode stays in-segment; no cross-segment discovery work |
| Integration testing and deployment | 2–2.5 | Domestic-OS adaptation, dual-mode and multi-region scenario combinations |
| **Total** | **~20–25 person-weeks** | Excluding hardware procurement, network changes, licensing fees |

**Table 13: Phase 2 and 3 effort estimates**

| Phase | Work item | Effort (person-weeks) |
| --- | --- | --- |
| Phase 2 | Org-structure advanced (multi-level departments, roles, external contacts) | 1 |
| Phase 2 | Approvals (form engine + workflow engine) | 2.5–3.5 |
| Phase 2 | Calendar and to-dos | 1.5–2 |
| Phase 2 | Announcements, daily/weekly reports | 1 |
| Phase 2 | Group tools (bot, poll, sign-up chain, group tasks) | 1–1.5 |
| Phase 2 | Session audit (access permissions, access log) | 0.5–1 |
| Phase 2 | Office floor map (plan, point editor, search-locate) | 1.5–2 |
| Phase 2 | Remote assist (screen view, controlled operation, authorization and full tracing) | 3–4 |
| Phase 2 | **Subtotal** | **~11–14.5** |
| Phase 3 | AI service layer (context assembly, prompts, structured output, retrieval augmentation) | 2–2.5 |
| Phase 3 | In-group assistant | 1 |
| Phase 3 | Minute generation | 1–1.5 |
| Phase 3 | Requirement structuring | 0.5–1 |
| Phase 3 | Speech-transcription integration | 1–1.5 |
| Phase 3 | Model deployment and tuning | 1 |
| Phase 3 | **Subtotal** | **~6.5–8.5 + hardware** |

### 5. Resource needs

**Servers.** One centralized collaboration server, sized by **whole-domain headcount**, tiers in Chapter VIII Section 6. Phase 1: 8 cores / 16 GB to start (gateway and messaging), plus PostgreSQL and object storage. If file-body archiving or heavy shared-space traffic is enabled, deploy file storage separately or mount a dedicated volume. Message archive storage: 500 GB to start, planned by retention period. **Staffing.** Backend 1, client 1, plus 0.5 each for test and ops; at that input Phase 1 takes ~8–10 weeks. **Hardware (Phase 3).** See Chapter VI.

### 6. System identity and naming conventions

**Naming is not decorative here.** Chapter III excluded a batch of AGPL open-source projects; the system name must keep distance from them, or delivery-time component lists and compliance notes will confuse "this project" with "the evaluated-and-excluded."

**The English name and code name are fixed as Memex; the formal Chinese name remains 「内网办公即时通讯系统」.** "Memex" comes from Vannevar Bush's 1945 "memory extender" concept — a machine letting one retrieve all prior records along one's own associative paths. This plan's core principle is the trace, whose substance is not "stored" but "findable again" — consistent with that idea. The name must satisfy four constraints:

- **No overlap with excluded open-source projects** (RustDesk, 喧喧, MeshCentral, Apache Guacamole, FreeRDP, etc.).
- **No resemblance to commercial office-communication products** (WeChat Work, Feishu, DingTalk, Slack, Teams, Mattermost) — avoiding trademark and procurement ambiguity.
- **No implication of secrecy or surveillance**: no roots suggesting "secret chat"/"burn-after-reading" (conflicting with explicit exclusions); avoid Trace/Track/Guard-style "tracking" feel — the client installs on every office terminal, and a surveillance-flavored name amplifies rollout resistance.
- **Directly usable as an identifier**: pure ASCII, no hyphens, not starting with a digit; usable as repo name, package name, Windows service name, intranet domain.

**Table 14: System identity and naming list**

| Use | Identity | Notes |
| --- | --- | --- |
| English name / code name | Memex | External identity and project code name |
| Formal Chinese name | 内网办公即时通讯系统 | For reports and official documents, used alongside the code name |
| Code repositories | memex, memex-client, memex-server | Client and server in separate repos |
| Package name | com.(org domain reversed).memex | Per actual org domain |
| Windows service name | MemexServer | Display name states it is the collaboration server |
| Installers | Memex-Setup-x.y.z.exe, MemexClient-x.y.z-win-x64.exe | Version per actual release |
| DB and log prefixes | memex, [MEMEX] | For log aggregation and search |
| Intranet domain / hostname | memex.corp.local (example), memex-srv | Replace per actual domain |
| Report cover identity | 内网办公即时通讯系统（Memex） | Used in this report's cover title |

**Name-collision check.** Candidates were screened before fixing: IntraLink heavily overlaps a commercial secure-collaboration product in an adjacent domain — rejected; Retrace matches a shipping APM product — rejected; Memex's historical namesakes are mostly defunct or renamed. **Caveat: this check is a first pass for internal use with limited coverage; formal trademark search is required if the system is later delivered externally or sold.**

**Employee-facing wording.** Of a piece with the constraints: client UI and training materials uniformly say「归档与检索」(archiving and retrieval) for archive capability, **never 「监控」「追踪」「审查」(surveillance/tracking/inspection)**. The necessity of admin-side functions is covered in Chapter IX, but wording directly affects rollout resistance — the same capability called "messages are searchable" versus "your operations are monitored" meets entirely different acceptance.

---

## VI. AI Capability Planning (Phase Three)

AI is Phase 3, after communication and office capabilities — but its preconditions and investment scale are stated here for the board's full picture.

### 1. Hard constraints of intranet deployment

**No internet means models must live on the intranet.** Public LLM APIs are all unreachable; only local inference of open models. Three constraints: the capability ceiling is set by model size (local models trail mainstream commercial ones, especially in complex reasoning and long-text understanding; information-extraction tasks like minutes and requirement structuring suffer less, complex logical reasoning more); throughput and concurrency are bounded by VRAM — high-frequency all-staff use needs matching hardware; hardware procurement is a precondition — work starts only after hardware lands.

### 2. Hardware threshold

The binding constraint is **VRAM**, not code effort.

**Table 15: Model size and hardware thresholds**

| Model size | VRAM quantized | Hardware threshold | Suitable scenarios |
| --- | --- | --- | --- |
| 7B–9B | ~6–8 GB | One consumer GPU, or CPU (slow) | Short-text summary, simple Q&A; long meeting records struggle |
| 14B | ~10–14 GB | One 24 GB GPU | Minutes and requirement structuring usable; price/performance knee |
| 32B | ~20 GB+ (more with long context) | One 24 GB tight; dual GPU or one large-VRAM card | Complex reasoning and long documents; minutes clearly better |
| 70B | ~42 GB | Dual GPU+ | Approaches commercial-model quality |

**Most underestimated point: VRAM use is not just weights — context length and concurrency count too.** Minutes are exactly long-context: a two-hour transcript can exceed ten thousand characters; a 14B model at 32K context adds several GB of KV cache. Do not size to the minimum; leave margin for context and concurrency. **Recommended target: a 32B-class model (dual GPU or one large-VRAM card)** — the tier where minute quality clearly improves; on a tight budget, 14B meets basic needs.

### 3. Model and component selection

**Table 16: AI component selection**

| Component | Recommendation | License | Notes |
| --- | --- | --- | --- |
| LLM | Qwen series (7B/14B/32B) | Apache-2.0 | Strong Chinese, full size ladder, permissive |
| LLM (alternate) | DeepSeek series | MIT-family | Strong reasoning, fit for structured extraction |
| Inference service | vLLM or Ollama | Apache-2.0 / MIT | vLLM high throughput, concurrent; Ollama simple, fit for validation |
| Model gateway | Self-written lightweight gateway (or a LiteLLM-class proxy) | Self-write / MIT | Unified OpenAI-compatible protocol; endpoint registration, routing, degradation, invocation audit; Chapter VI Section 5 |
| Vector retrieval | pgvector (reuse PostgreSQL) or Qdrant | PostgreSQL / Apache-2.0 | Retrieval augmentation for "answer from group history" |
| Chinese embedding model | BGE series | MIT | Stable Chinese semantic retrieval |
| Speech transcription | FunASR (DAMO Academy) | Apache-2.0 | Better Chinese accuracy than generic; speaker diarization |

All are "deploy and invoke," no modification or distribution — **no contagious license trigger**, consistent with the no-rebuild judgment.

### 4. How the three application scenarios land

**Scenario 1, in-group assistant.** Triggered by @ in group. The server takes the group's recent messages in a time window, recalls relevant history via vector retrieval, assembles the prompt, returns the answer to the group. Three technical points: context needs sliding window plus summary compression (long groups won't fit whole); answers must be marked "AI-generated" to avoid being mistaken for a colleague; reading scope is strictly bound by group membership — the bot must not read groups the asking user is not in.

**Scenario 2, minute generation.** Input is either a time range of group chat or a meeting-recording transcript. The server does chunked summarization (chunk then consolidate) into a fixed structure: topic, time, attendees, discussion items, conclusions, action items (with owners and deadlines), next meeting. **Quality hinges on fidelity and traceability** — every conclusion and action item traceable to the original message with UI jumps; always marked "AI-generated, for reference only; the original record prevails."

**Scenario 3, requirement structuring.** For requirement-intake groups: scattered discussion structured into items — description, raiser, time, priority, modules, acceptance criteria, status. The key point: **the model must output structured data (JSON), not prose** — via constrained decoding or prompt templates; output written to a requirements table becomes ticket-manageable in the console, closing the loop.

### 5. Model access layer design

One concrete question: **can the AI layer support both local models and external APIs?** Answer: the interface layer should support and must be pluggable; the runtime layer is constrained by network and compliance. Two levels, stated separately:

**Table 17: The two-level judgment on model access**

| Dimension | Possible? | Reality in the current environment |
| --- | --- | --- |
| Software supports multiple model endpoints | Yes, and should be designed pluggable | Just use the OpenAI-compatible protocol; very cheap |
| Reach external APIs on the internet | Interface can call | **Network unreachable** — intranet physically isolated; external endpoints inaccessible |

Conclusion: **pluggable in architecture; local-models-only in runtime by default.** Not contradictory.

**Gateway responsibilities** — five: **protocol unification** (OpenAI-compatible; local vLLM, Ollama and most clouds natively support it; switching endpoints is config-only); **endpoint registry** (name, address, credentials, context window, capability flags for tool calling and structured output, permitted scope); **routing** (by task type, data classification, availability — Table 18); **health check and degradation** (auto-failover; local queue overflow may shed per config); **invocation audit** (caller, time, endpoint, input size, message range read).

**Table 18: Model routing policy**

| Task type | Data scope | Allowed endpoints | Notes |
| --- | --- | --- |---|
| Summaries, minutes, requirement structuring, smart search | Archived messages | **Local models only** | Hard constraint, not config-decided |
| General Q&A, public-material organization | No archived data | Local; external allowed if a compliant internet egress exists | Requires explicit admin registration and enablement |
| Development-time baseline comparison | Desensitized samples | External (dev environment only) | To gauge local vs commercial gap |

**Three constraints hard-coded.** More important than "supports external APIs," because they prevent data exfiltration. First, **archived messages go to local models only** — decided by **the data's source marking, never by prompt-content sniffing**; made configurable, one misconfiguration is one leak. Second, **external endpoints disabled by default** — explicit admin registration and enablement, with who/when/why recorded for audit. Third, **outbound attempts alarm independently** — any outbound connection from the gateway, successful or not, is logged and alarmed, never silent.

**Two practical gains.** First, **Phase 3's software engineering can precede hardware**: prompt engineering, context assembly, structured-output parsing, retrieval augmentation can be built and validated against external endpoints in dev, then only the endpoint config changes when GPUs arrive — decoupling procurement lead time from development (GPU lead times being long, this matters). Second, **zero rework when an internet egress appears**: a compliant public model service or a vendor's private deployment (also a standard API endpoint) needs only one gateway registration; client and IM server unchanged.

**Compliance precondition.** If the organization handles classified levels, external APIs are forbidden outright; the gateway design still holds, its value shifting to unified management and audit of multiple local endpoints, with room reserved for future compliant change.

### 6. Architecture design

AI deploys as an independent service, decoupled from the IM service. **IM server → AI service → model gateway → model layer.** The IM server provides message archives as the sole data source; the AI service does context assembly, prompt-template management, structured-output parsing, retrieval augmentation; the gateway does registration, routing, degradation, audit (Section 5); the model layer holds LLM inference, vector retrieval, and transcription. The AI service upgrades and scales independently without affecting IM stability. **Permission is the first principle**: AI reads must carry the caller's identity and fetch within that identity's IM permissions — **no "AI reads what the user cannot"**; the module's key security point. **AI output archiving**: store AI-generated minutes and requirement items as a **separate content type**, marked with generation time, model, input scope, and generator — not mixed with original messages; originals archive per existing rules; AI output is separately cleanable.

### 7. Preconditions and effort

**Three start preconditions:** GPU hardware in place and deployment-verified; Phase 1 archiving and permissions live and stable; the AI-readable message scope and compliance boundary fixed and employees notified. Without all three, do not start. **Effort:** ~6.5–8.5 person-weeks (excluding hardware procurement and environment prep). **One cost to state:** the GPU server's one-time procurement cost may well **exceed the entire Phase 1 development investment** — list it separately at approval to avoid mixing with software costs.

---

## VII. Companion LAN File-Sharing Plan

File sharing is loosely coupled with IM; deploy a standalone file platform rather than embedding it in the client — upgrades don't interfere and the permission model is cleaner. The in-client file cabinet is positioned as "quick temporary transfer"; centralized shared directories belong to the standalone platform.

### 1. Selection points

One easily missed but mandatory check: **maintenance activity.** The survey found a formerly widely used lightweight file-management project archived with no further releases or security fixes — such projects must not enter new production. Treat "maintenance activity" as an evaluation item equal to features and license.

### 2. Option comparison

**Table 19: File-sharing options**

| Option | Form | Strengths | Limits | Fit |
| --- | --- | --- | --- | --- |
| Samba | Network share | OS-builtin; mapped drive on Windows feels local | Permissions tied to OS accounts, not unified with IM identity | Directory-level sharing, Windows-dominant fleets |
| Cloudreve | Web file platform | Friendly Chinese UI; groups and quotas; share-link passwords and expiry | Needs separate account management | Web access and share links |
| Kodbox | Web file platform | Windows-desktop-like operation, low learning curve | Heavier than lightweight options | Users accustomed to desktop-style operation |
| Seafile Community | Sync drive | Chunked dedup, fast incremental sync; desktop and mobile clients | More deployment components | Multi-terminal auto-sync |

### 3. Recommendation

**Samba plus a web file platform.** The most frequent need is "shared directories that feel like a local disk" — Samba is best there with zero extra maintenance; for web access, outgoing share links, or per-person authorization, add a web platform (actively maintained Cloudreve or Kodbox). To unify identity, either bridge device credentials at login or converge access to department-level shares and de-emphasize personal accounts — to be detailed in the implementation plan. Whatever the choice, three principles: **least-necessary permissions**; **shared content under retention management**, same retention period as chat records; **distinguish quick transfer from centralized sharing**.

---

## VIII. Network Topology and Deployment Planning

Deployment form and supporting requirements across segments and regions. Two confirmed points: the collaboration server is **single and centralized**, no regional edge nodes; and **login-free direct mode is confined to the local segment, positioned as temporary, nearby use**. Together: **all cross-segment and cross-region communication is carried by collaboration mode.**

### 1. Two independent cross-segment questions

Direct-mode discovery relies on UDP broadcast (port 2425); routers and L3 switches do not forward broadcast by default — basic device behavior, not misconfiguration. **Conclusion: do not solve cross-segment discovery for direct mode.** The remainder of this chapter is explanation and deployment form only — **not a development task and places no configuration demand on network admins.**

Even so, "**discovery**" and "**reachability**" are worth separating — they decide a common misjudgment (whether to build a relay service). **Discovery**: broadcast cannot leave the segment, so cross-segment terminals cannot see each other; fixing this needs network-device cooperation, not bypassable at the application layer. **Reachability**: after discovery, messages are point-to-point unicast — if the two segments are L3-routable (mutually pingable), direct unicast works, **no gateway needed, no relay traversed**. Hence the rule: **routable — solve only discovery; not routable — only then is true relay needed.** The common mistake conflates the two, turning "one device command" into "build a relay service." This plan does not go there (Section 8).

### 2. Four network scenarios and the two-mode division of labor

**Table 20: Division of labor by network scenario**

| Scenario | Typical case | Direct-mode viability | Collaboration | Needed |
| --- | --- | --- | --- | --- |
| Same segment | Terminals in one VLAN | **Works**: UDP discovery, P2P | Works | Zero configuration |
| Cross-VLAN, L3 reachable | Different VLANs, one campus, routing core | Needs device config to discover — **not scheduled here** | **Works** | Just log in; no network changes |
| Cross-VLAN, L3 unreachable | Segments isolated by firewall or NAT | Unusable | **Works** | Just log in; no network changes |
| Multi-region | Remote branch via dedicated line | Unusable; enabling not advised | **Works** | Dedicated line or encrypted tunnel to the server |

The table directly answers "why no extra cross-segment investment": **from row 2 on, collaboration mode already works natively with no network-side changes**; direct mode's absence in those scenarios costs nothing — it was never designed for them.

**Three notes on UDP broadcast (for understanding, not tasks).** First, devices forward only fixed ports (TFTP, DNS, DHCP, NetBIOS) by default — **2425 is not on the list**; if cross-segment direct were ever truly needed, admins must explicitly allow and configure UDP relay (Cisco: add the UDP port under the interface; Huawei/H3C: UDP Helper with a destination address). Second, even with a relay, forwarding is **directed to specified addresses only**, never broadcast-flooding all segments. Third, **software relay instances forwarding broadcasts between segments must be avoided** — every segment receives every other's broadcasts; per-point broadcast volume grows N-fold and the network total N², manufacturing instability. All three are reference for future requirement changes; nothing scheduled this version. **Port fixing stays**: direct mode uses a fixed port range (recommended UDP 2425–2436, TCP 2426–2437) even segment-locally — unfixed ports make security policy unwritable and later troubleshooting and admission control harder.

### 3. Deployment topology

Confirmed: **single centralized server** at HQ, carrying access gateway, message relay, archiving, org structure, admin console, and AI inference; all regions reach the same server via dedicated line or encrypted tunnel; no regional edge nodes. The server uses internal domain/hostname identity consistently (example memex.corp.local, replace per reality), consistent with Chapter V Section 6.

Three reasons. First, **simplest architecture and operations** — no cross-node data sync, version consistency, failover, or distributed transactions; the operations object drops from "many places, many sets" to "one place, one set." Second, **server failure does not mean domain-wide loss of contact — the fundamental reason single-box risk is acceptable here**: dual-engine means clients fall back to the direct engine when the server or line is unreachable; same-segment messaging and file transfer continue. The server is the **single point of archiving and management, not of communication** — fundamentally different from conventional centralized IM, and the precondition for accepting single-box. Third, **lowest investment** — no per-region node servers and their operations.

**Direct mode's boundary: the local segment.** Login-free direct works only in-segment; cross-segment and cross-region go through login. Three reasons: the login-free form's core value is "zero deployment, use on install" — cross-segment/region would require nodes, tunnels, and latency handling, voiding that value; cross-region P2P is usually unguaranteeable in bandwidth and policy, with traffic falling back to relays anyway; and an uncontrolled channel crossing arbitrary segments is compliance-indefensible. See Section 8.

### 4. Three engineering constraints

**Address planning must not collide.** If regions reuse the same segment (e.g. all 192.168.1.0/24), routing conflicts follow — the most common multi-region pitfall; plan uniformly before deployment and fold it into network changes. **Clock unification.** Archiving, login records, and audit logs all depend on timestamps; terminals and the server must share an NTP source, or cross-region message ordering and forensic timing drift. **Link encryption.** Cross-region links over uncontrolled paths should be encrypted per classified-protection requirements (IPsec or SM-crypto tunnels); link-layer encryption and the collaboration engine's own application-layer encryption are independent — have both.

### 5. Three impacts of single-box centralization and countermeasures

**Latency.** Remote logins, org-structure pulls, and history searches cross cities and feel slower. Counters: local caching with incremental sync of org structure and frequent conversations; history paged on demand, no full preloading; console global search may return asynchronously. **Link outage and archive gap — the first rule single-box must fix.** When the server or line is unreachable, clients fall back to direct mode; messages bypass the server and the archive, creating a gap. Two treatments, board to confirm. **Option 1 (recommended)**: in collaboration mode, when **both parties are logged in**, outage-period messages stash locally and auto-back-upload on reconnect; only conversations with non-logged-in users truly go direct and unarchived. The server dedupes by client-generated message ids at back-upload. This shrinks the gap to "conversations with non-logged-in colleagues," costing the client a stash queue and back-upload mechanism (~0.5–1 person-week). **Option 2**: keep the current design — outage-period collaboration messages degrade to direct, unarchived; simple, but the gap grows with outage length. **Single-point risk.** Archiving, org structure, cloud history, and AI concentrate in one place. Counters: scheduled DB backups with restore drills; a cold standby machine and switchover plan; direct mode as the basic-communication fallback; published maintenance windows with the client clearly indicating fallback to direct mode and non-archiving.

### 6. Server resource tiers

One server carries whole-domain concurrency; size by **whole-domain registered headcount**.

**Table 21: Server sizing tiers**

| Domain size | Recommended config | Deployment | Notes |
| --- | --- | --- | --- |
| ≤200 | 8 cores 16 GB | Gateway, DB, app co-located | Phase 1 starter tier |
| 200–500 | 16 cores 32 GB | DB separate; message storage on a dedicated volume | Storage 500 GB to start, planned by retention |
| 500+ | Tier by load test | Gateway, DB, file metadata, AI inference separated | "Single centralized" then means **logically one set**, not one physical box; the centralized architecture itself is unchanged |

**Bandwidth.** Cross-region links carry messages, org sync, logins, and history search; file bytes bypass and do not occupy them, so dedicated-line bandwidth needs are modest — size to message peaks and concurrent history search. After AI lands, cross-region inference adds load; re-evaluate in Phase 3.

### 7. Network-side precondition checklist

External dependencies outside the dev cycle; raise them with network admins at project start. **Cross-segment broadcast forwarding is not among them** — direct mode is segment-bound and raises no such requirement. Item 3 (inter-region links) usually takes longer than software; start earliest.

**Table 22: Network-side preconditions**

| No. | Item | Content | Owner | Notes |
| --- | --- | --- | --- | --- |
| 1 | Direct-mode port allowance | UDP 2425–2436, TCP 2426–2437 | Network admin | Fixed ranges or security policy is unwritable; in-segment only, no cross-segment allowance |
| 2 | Server reachability | Collaboration server ports reachable domain-wide | Network admin | All regions reach the same server via line or tunnel |
| 3 | Inter-region links | Dedicated line or IPsec / SM-crypto tunnel | Network admin | Encrypted per classified protection; start earliest |
| 4 | IP address planning | No segment collisions across regions | Network admin | The most common multi-region pitfall |
| 5 | Clock sync | Domain-wide NTP | Network admin | Archiving and audit depend on timestamps |
| 6 | Network admission control | IP–MAC binding or port-level authentication | Network admin | Necessary complement to the login-free scheme |
| 7 | Server room resources | Rack, power, uplink bandwidth | IT Department | Single centralized box, reserved by domain size |

### 8. Direct mode's scope and why it does not cross segments

The previous version proposed "local server doubles as discovery node." **Confirmed unnecessary; this section gives the reasons and the resulting boundary.**

**Positioning.** Login-free direct serves "temporary, nearby, zero-deployment" needs — a visitor, a borrowed machine, instant talk within one office area. All such scenarios are in-segment and short-lived; the user's own words: "it's just temporary use." **"Temporary" and "nearby" are direct mode's complete boundary.**

**Three reasons not to do cross-segment.** First, **doing it breaks the positioning**: any cross-segment investment — admin-configured UDP relays or discovery nodes — ends "zero deployment, use on install"; the core value vanishes and it becomes a system needing planning, coordination, and maintenance — worse than just logging in. Second, **collaboration mode already covers it, more easily**: cross-segment/region is natively reachable after login with no network changes (Table 20); collaboration is better and stronger there (history, org structure, search), so there is no reason for direct mode to chase parity. Third, **cleaner compliance**: direct-mode messages bypass the server and leave no trace; letting it cross segments and regions creates an uncontrolled channel spanning arbitrary segments in the enterprise intranet — indefensible under classified protection and audit. Keeping it in-segment is the clearer boundary.

**The resulting boundary.** **Direct mode is temporary use within the local segment.** It depends on UDP broadcast, which does not leave the segment; no cross-segment configuration or development is needed or should be done for it. **For all cross-segment and cross-region communication, log in to collaboration mode.**

**Capabilities not retained.** The previous version considered manual peer-address entry or segment scanning for cross-segment direct. With the positioning converged, this is **dropped** — keeping it would dress an emergency tool up as a "cross-segment solution," contradicting the positioning and inviting "why can he and not me" explanations. **With a real cross-segment need, use collaboration mode.**

### 9. File sharing across regions

Shared space (Samba and the web platform) should deploy **one local instance per region**, not cross-region unified: Samba directory access is latency-sensitive and cross-city access stutters badly; unified cross-region storage adds backup and DR complexity. Two ways to fetch files cross-region: if the inter-region line is L3-routable, clients may access the remote share directly — fit for small files and low frequency; **the recommended way is a file transit area** — put files into the local share, send the access link by message, and let the receiver pull locally, keeping large files off the cross-region link.

---

## IX. Risk and Compliance Notes

By category: identity and access, archive completeness, compliance, availability, and technical — the latter two with multi-region additions.

### 1. Identity and access risk

**Risk.** Direct mode has no login credentials; any terminal on the intranet can join. The console shows a device list, not a people list — who uses a device is unverifiable system-side. With login-free use open, employees may stay in direct mode indefinitely, leaving the device-to-person mapping chronically incomplete. **Counters.** First, **enable new-device approval**: first registration enters pending state; before approval, collaboration login is impossible — smallest input, clearest gain. Second, **network-layer admission control**: switch-side IP–MAC binding or port-level authentication; needs network-admin cooperation; list as a precondition. Third, **a device ledger with owner registration**: each device registers user, department, and responsible person at approval; unbind and disable on transfer or retirement. Fourth, **login guidance and ownership reminders**: the client keeps prompting the login entry when not logged in; the console marks devices idle-unlogged for days as "pending assignment" for admins to push binding.

### 2. Archive completeness risk

**Risk.** Cross-mode sessions bypass the server; direct messages never archive — an evasion path exists in theory ("you log out, we chat direct"). Not theoretical: such dual-form designs will be attempted in practice. **Counters.** The "allow communication with non-logged-in devices" switch, off for high-retention departments; the fixed "not archived" marker on cross-mode sessions plus establishment logs (time, parties, duration — no content) making abnormal frequency visible; honest disclosure of the retention boundary in policy. **The exposure is bounded and explicit.** The other commonly imagined evasion paths are already blocked by design: recall does not erase; terminal deletion does not reach the server; no user-facing trace switch (Chapter I Section 2). The one item management must own: **cross-mode sessions are this plan's only trace gap, and its cause is the form boundary, not a functional trade-off.**

### 3. Compliance risk

**Risk.** The archive scope is "collaboration-mode conversations" — messages produced after login and relayed by the server. Narrower and less intrusive than full archiving, but with several compliance points. **Counters.** First, **prior notice and consent**: revise the employee handbook or issue a specific notice stating scope, purpose, period, and access rights, and retain the record — **stating the retention boundary (what is and is not retained) explicitly**. Second, **limit retention scope**: only text messages and file-transfer metadata; no terminal-file content capture, no screen recording, no collection beyond purpose; file bodies unarchived by default. Third, **set retention period and expiry cleanup**: 6 months recommended, auto-archived or cleaned at expiry. Fourth, **strictly limit access**: console query rights to necessary posts only; every query logged and traceable; querying others' conversations preferably requires approval. Fifth, **supplementary notice when AI lands** (Phase 3 reads archives): state the AI read scope and output handling; **if the organization handles classified levels, external API endpoints must be unregistrable in the gateway**; even unclassified, archived data is local-models-only (Chapter VI Section 5); all gateway outbound connections logged and auditable. Sixth, **proactively address the expectation gap**: the current LAN tool advertises "records only on your machine, no server" and has a "secret chat" (burn-after-reading) mode; employees hold a stable expectation that "chat is not seen by the company," while this plan centrally retains collaboration messages — **the likeliest point of rollout resistance**, independent of the technical plan but directly affecting adoption.

**One clarification first, so the notice is not written backwards: secret chat and burn-after-reading are current-tool features, not capabilities this plan builds** (exclusions in Chapters I and II). Notices and training must not present them as "new-tool features," or a second misunderstanding follows — that the new tool also has a no-trace option, discovered missing only in use. Recommend stating two things separately before launch: **login-free direct mode keeps the "no server, no archive" experience** (an inherent property and part of the dual-engine value — not an optional privacy switch); **logged-in collaboration mode clearly states retention**, shown by the archive-state markers. Employees knowing which state they are in and whether a given message is retained is more acceptable than a blanket "the company starts keeping records." Also write one discipline into the materials: **retention does not mean superiors may read subordinates' conversations.** Query rights go to designated posts only; a direct manager gains nothing by virtue of being a manager. Left unsaid, this is universally misread as "my boss can read my chat" — a primary resistance source.

### 4. Two boundaries: remote assist and site information

Two of the added features differ in risk kind from the rest and need separate boundaries.

**First, remote assist.** The other features are data flow — messages, files, org info moving in-system, risk of leakage; remote assist is **cross-terminal operation permission**, risk of operating someone else's computer. Different order of magnitude. Beyond the four hard requirements of Chapter II Section 4 (explicit confirmation, continuous visibility, full tracing, scope control), three institutional points: use limited to **troubleshooting and technical support**, never for viewing others' work; **retention and access rules** parallel archived messages (6 months suggested), recordings kept with access records; **anomaly alerting** — unusual initiators, off-hours initiation, frequent connections to the same controlled party — not just passive logging. Two further notes: **remote-assist tracing and admin archive queries are two different things and not interchangeable** — archive queries read chat content; assist records read operation behavior, which in compliance review is often the more evidentiary. And align with the trace principle: **media content of assist and calls is not stored, but the fact of communication must be archived** — same structure as the file channel: the metadata of "who communicated with whom, when, how" (participants, start/end, duration, connection type), not the imagery. Assist screen recording is a separate optional department-issued switch. Not storing media is a storage/bandwidth constraint, **not an exception to the trace principle** — state both together in the materials to avoid the reading that "a no-trace loophole exists."

**Second, the office floor map.** Floor plans contain office layout, security corridors, key equipment rooms and machine-room locations; domain-wide visibility equals an internal site atlas. Three boundaries: **own floor only by default**, cross-floor/cross-campus by authorization; floors housing **security, confidential, finance, and HR posts may be wholly unpublished**; **no original-image download** — the client renders only. Map visibility should track door-access permissions roughly: areas one cannot enter need not be drawn.

### 5. Availability risk

**Risk.** Dual-engine design: on server failure, collaboration archiving, org structure, and cloud history are unavailable, but **the direct engine still works** — same-segment messaging and file transfer continue. The server is the single point of archiving and management, not of communication. This lowers high-availability urgency, but archive-loss risk still needs backup as a hedge. **In multi-region deployment, a cross-region line outage takes that region's collaboration mode down entirely** — employees there fall back to same-segment direct; cross-region messages temporarily undeliverable, back-uploaded per Chapter VIII Section 5 on recovery. **Counters.** Scheduled DB backups with restore drills; cold standby and switchover plan; published maintenance windows; the client clearly indicating fallback to direct mode and non-archiving when the server is unreachable.

### 6. Technical risks

**Table 23: Technical risks and responses**

| Risk | Description | Response |
| --- | --- | --- |
| Two engines contend for resources | UDP listener and long connection both running may interact on low-spec terminals | Thread/process isolation; caps on broadcast rate and retransmit; measure on low-spec terminals |
| UDP broadcast blocked by policy | AP isolation, VLAN division, or firewall may block broadcast even in-segment | Confirm policy before deployment; client gives a clear "no devices found" hint steering to collaboration mode |
| Direct mode unusable cross-segment | Different-VLAN terminals cannot discover each other — design positioning, not a defect | **Treat as expected**: cross-segment/region uses collaboration; explain in training so multi-VLAN office areas don't misread it as failure (Chapter VIII Section 8) |
| Multi-region latency and outages | Single-box means cross-city latency; a line outage drops that region's collaboration | Client caches org structure, incremental sync, paged history; fall back to direct during outages; back-upload per Chapter VIII Section 5 |
| Cross-region IP conflicts | Same segments in different regions collide after interconnect | Unified IP planning before deployment; network-side precondition |
| Large-file transfer pressure | Big transfers occupy bandwidth | Bytes go direct, not through the server; per-file caps; size thresholds on server-relayed paths |
| Multi-device state conflicts | Incorrect desktop kicks leave two online, garbled states | Server treats "one online per terminal class" as authoritative, atomically kicking the previous session at login; client device-management page |
| Domestic-OS adaptation | Qt or web-stack display and dependency issues on specific desktops | Include domestic terminals in adaptation from Phase 1, not deferred to acceptance |
| Local data mistaken for records | Employees think clearing local deletes records, or that collaboration messages are local-only | Client distinguishes "local cache" vs "cloud archive"; archive-state markers on sessions |
| Broadcast identity spoofing | In direct mode, forged packets can impersonate a device or account | Broadcast account ids are display-only, never authorization; collaboration sender identity is the server record |
| Remote assist abused | Operating others' terminals — abuse impacts exceed message leaks | IT-only by default; explicit confirmation with one-click interrupt; full tracing and recording; anomaly alerts |
| Stale map data | Seats and departments change; the map loses trust and is abandoned | Drag-and-drop point editor; named maintenance owner; folded into department/seat change processes |
| Screen-capture platform differences | X11 vs Wayland capture on domestic Linux, differing default sessions per distro; WebRTC itself provides no capture — per-OS implementation required | Cover target UOS and Kylin versions in Phase 1; Wayland via PipeWire and portals, tested at selection; explicit degradation notices on unsupported environments |
| Missing client media capability | Tauri cannot establish media on domestic Linux (WebKitGTK without WebRTC) — blocking | Verify WebRTC on target terminals at selection (check RTCPeerConnection); conclusion in Chapter V Section 3 — prefer Qt or Electron |
| Media retention boundary misread | No media-content storage may be misread as a trace loophole | Answer with metadata archiving (participants, start/end, duration, connection type); state in materials and the archive query UI, consistent with the file channel |

---

## X. Implementation Plan and Recommendations

Phase plan for Phase 1, pilot recommendations, and acceptance criteria, with inter-phase dependencies and external preconditions.

### 1. Overall plan

**Table 24: Phase 1 stage plan**

| Stage | Time | Main work | Deliverable |
| --- | --- | --- | --- |
| 1: Direct-mode development | Weeks 1–2 | UDP discovery, P2P messages and files, local store, basic UI | A client replacing the current tool |
| 2: Direct-mode pilot | Weeks 3–4 | One department pilots: in-segment discovery success rate, large-file speed, dual-mode switching | Pilot log and issue list |
| 3: Server and collaboration | Weeks 5–8 | Gateway, accounts and devices, org structure, archiving, multi-device control, client login and switching | A complete system with archiving |
| 4: Admin console and policy | Weeks 9–10 | Members and departments, permissions, device ledger, login records, search and export, policy switches | A manageable system |
| 5: Experience features and cross-mode | Weeks 11–12 | Screenshot and annotation, stickers, frequent contacts, reporting lines and contact visibility; cross-mode detection and routing, archive-state markers; deploy shared space and wire permissions | Complete dual-mode form |
| 6: Full rollout and acceptance | Weeks 13–14 | HQ rollout in batches, ledger registration and training; item-by-item acceptance | Acceptance report |
| 7: Cross-region rollout | After Phase 1 acceptance | Verify network preconditions, regions join via dedicated lines, per-region rollout and training | Remote regions live |

Four notes. First, **stage 1 contains no server work** and is immune to procurement and network schedules — it can start immediately; this is the phasing's main value. Second, **network admission control, server procurement, and cross-region line rebuild decide whether stage 3 starts on time** — evaluate them during stage 1; the line rebuild belongs to network admins and usually outlasts software — start earliest. Third, **reporting-line data prep can parallel development** — if maintained manually in the console, HR must compile during Phase 1 development; if synced from HR, extra coordination and schedule — decide early (Chapter XI). Fourth, Phases 2 and 3 are separate projects after Phase 1 acceptance, with independent boundaries and deliverables.

### 2. Pilot recommendations

Pick a mid-sized department covering all terminal types (Windows and domestic systems) with a part-time IT liaison; ≤20 terminals, to expose device discovery, file transfer, large-file performance, and dual-mode switching cheaply. Verify: real-network **in-segment** UDP discovery success rate, gigabit large-file throughput, post-login org-structure and history-search responsiveness. **Also confirm in the pilot: whether the department's terminals share one VLAN** — an office area split into per-floor or per-department VLANs may leave direct mode unable to discover within the department: not a defect, but must be known before rollout and explained in training. In multi-region settings, pilot a remote branch in a second round for line latency, cross-region message reachability, and search responsiveness.

### 3. Suggested acceptance criteria

**Table 25: Acceptance criteria (Phase 1)**

| No. | Item | Standard |
| --- | --- | --- |
| A1 | Login-free, config-free usable | A fresh terminal discovers same-segment peers and starts conversations with no address or account |
| A2 | Direct mode bypasses the server | Packet capture shows no server traffic for direct messages; server holds no related record |
| A3 | Mode switching | Enter address and log in; switch to collaboration without restart; local history preserved and merged |
| A4 | Multi-device control | Second desktop login kicks the first with notice; desktop and mobile coexist |
| A5 | Login-record completeness | Console shows full login history: device type, name, time, source |
| A6 | Archive completeness | After local deletion or sender recall, admins still retrieve the original; the recall action (who, when, which) is queryable |
| A7 | Cross-mode marking | Sessions with non-logged-in terminals show "not archived," unhideable |
| A8 | Archive start accuracy | The console's archive-start for a session converted from cross-mode matches the actual login time |
| A9 | Policy switches take effect | Disabling cross-mode communication blocks such sessions; disabling login-free blocks the main UI |
| A10 | Transfer and sharing | Files and folders transfer; resume works; shares respect permissions; violations denied |
| A11 | Server degradation | On server loss, the client clearly falls back to direct; same-segment messaging and transfer still work |
| A12 | Domestic-OS coverage | Messaging, transfer, and UI verified on UnionTech UOS and Kylin |
| A13 | Cross-segment communication | Collaboration messages flow between different-VLAN terminals; direct mode is segment-bound, no cross-segment requirement |
| A14 | Server recovery | One DB restore drill completed per plan; process executable, data intact |
| A15 | Multi-region access | A remote region joins via dedicated line: login, messaging, org structure, history search all normal |
| A16 | Outage archiving rules | Outage-period collaboration messages stash locally and back-upload deduped by message id — no duplicates, no gaps |
| A17 | Screenshot and annotation | Correct region selection under multi-monitor and high DPI; doodle/arrow/mosaic usable; direct send on confirm |
| A18 | Stickers and frequent contacts | Custom packs importable and sendable; frequent contacts survive machine-change login |
| A19 | Reporting lines | Profiles show the direct manager; the chain walks upward; bulk import works and flags errors |
| A20 | Contact visibility | Hidden departments/members absent and unsearchable; restricted departments see only their own contacts |
| A21 | Media foundation usability | WebRTC connections verified on target terminals (Windows, UOS, Kylin); screenshot capture works per platform; unsupported environments degrade with explicit notice |
| A22 | Component license list | Full third-party list at delivery; verified free of strongly contagious (AGPL/SSPL) components; the exception (collaborative documents) stays independently deployed with verification records |
| A23 | Identity consistency | Installer names, Windows service name, DB/log prefixes, intranet domain all use the Memex identity per Chapter V Section 6 |

**Phase 2 criteria listed separately.** The floor map and remote assist are Phase 2; their criteria are compiled at Phase 2 approval. Remote assist must include four verifications: the controlled terminal actually receives and confirms the authorization request; the status indicator stays visible with one-click interrupt; the server holds complete operation records; department-issued switches actually take effect.

---

## XI. Items for the Review Board to Decide

**Fifteen items**, unchanged in count from the previous version; items 5 and 6 bear directly on trace scope. **"No secret chat / burn-after-reading" is not listed** — it is the necessary corollary of "connecting to the server is for the trace" (Chapter I Section 2), not a votable option; dissent means revisiting that premise and returning to the requirements stage, not treating it as a missing feature.

**Table 26: Decision items**

| No. | Item | Recommendation | Impact |
| --- | --- | --- | --- |
| 1 | Adopt the self-written-business-layer route? | Yes | Sets the technical route for all subsequent work; Chapter III gives the no-rebuild rationale |
| 2 | Endorse the four-phase plan? | Yes; each later phase separately approved after Phase 1 acceptance | Sets project scale and risk structure; avoids one-shot full investment |
| 3 | Self-developer conferencing and collaborative documents? | No — procure or integrate open components | Self-development forces re-estimation; recommend a separate project |
| 4 | "Desktop single-instance, mobile coexistent" login policy? | Yes | Matches WeChat Work; machine-change usability with controllable state |
| 5 | Allow logged-in ↔ non-logged-in communication? | Allow, with a per-department off switch for high-retention units | Allowed: continuous UX but an archive gap; disallowed: no gap but non-logged-in colleagues unreachable |
| 6 | Chat retention period | 6 months | Shapes compliance and storage planning |
| 7 | Single centralized server, no regional edge? | Yes | Simplest architecture and operations, lowest cost; dual-engine fallback means server failure ≠ domain-wide outage; beyond one box, split gateway/DB/AI deployment |
| 8 | Archiving during server/line outages | Local stash with post-recovery back-upload | Shrinks the gap to non-logged-in conversations; client stash queue and id-dedup ~0.5–1 person-week |
| 9 | May the gateway register external API endpoints? | Default no; if allowed, non-archive data only | Archive data to external endpoints is an exfiltration risk; keep the door technically but locked, audited, and logged when opened |
| 10 | Start GPU procurement evaluation? | Evaluate during Phase 1, procure at Phase 3 | GPU lead time and cost exceed expectations; early evaluation avoids Phase 3 stalling on hardware |
| 11 | Implement network admission control? | Yes | Necessary complement to the login-free scheme; needs network admins |
| 12 | Remote-assist exposure | IT operations only; off for ordinary staff by default | Cross-terminal operation permission; abuse exceeds message leaks; requires explicit confirmation, visibility, full tracing (Chapters II, IX) |
| 13 | Floor-map visibility | Own floor by default; cross-floor/campus by authorization | Plans contain security corridors and equipment rooms — sensitive; track door-access permissions |
| 14 | Contact-visibility policy | All-visible by default; executives' details hidden as needed | Contacts hold personal data; default full exposure is a data-protection gap |
| 15 | Source of direct-manager data | Manual admin maintenance with bulk import first; no HR integration yet | Decides whether "see your own manager" lands; external integration needs extra schedule and cross-department coordination |

**One optional confirmation:** whether to **stay compatible with existing FeiQ clients** (implementing direct mode on the IPMsg protocol so remaining FeiQ users can interoperate). If required, direct-mode protocol implementation changes, effort +~0.5–1 person-week, and the channel's messages travel in plaintext — weaker than a custom encrypted protocol. If the FeiQ fleet is small or has an exit plan, recommend against compatibility.

With the fifteen items plus the optional one confirmed, the project enters Phase 1 implementation. Items 2 and 3 jointly set project scale; items 12–15 involve permissions and personal-data handling — review with the compliance post.

---

## Appendix A: Device Fingerprint Implementation Points

**Table A-1: Hardware information collected**

| Item | Windows source | Linux source | In fingerprint |
| --- | --- | --- | --- |
| Motherboard UUID | Win32_ComputerSystemProduct UUID | /sys/class/dmi/id/product_uuid | Yes (primary) |
| CPU id | Win32_Processor ProcessorId | serial field in /proc/cpuinfo | Yes (primary) |
| System-disk serial | Win32_DiskDrive SerialNumber | serial under /sys/block for the disk | Yes (secondary) |
| NIC MAC | Win32_NetworkAdapter, physical only | interface hardware address | No, reference only |

Implementation points. **1. Exclude virtual NICs** — filter virtualization/VPN adapters, keep physical only. **2. Primary/secondary tiers** — motherboard UUID and CPU id primary, disk serial secondary; two of three matching counts as the same device, tolerating one hardware repair. **3. Volatile items excluded** — RAM size, OS version, device name never participate. **4. Store hashes only** — the server keeps the SHA-256 of the fingerprint, never raw hardware info. **5. Purpose** — direct-mode identity, the device ledger, and login records; **not login restriction** — accounts log in on any device; control comes from "desktop single-instance" and "new-device approval," not hardware binding. **6. Invalidation and re-registration** — OS reinstall or motherboard/disk replacement changes the fingerprint; the device registers as new and needs admin approval; since machine-change login is unaffected, this never blocks normal work.

## Appendix B: Reference Projects and Sources

**Table B-1: Reference projects**

| Project / source | Address | Notes |
| --- | --- | --- |
| 茶话间 Teahouse | https://github.com/skyjt/teahouse | GPL-3.0; server-free LAN messaging; direct-engine reference |
| nwChat | https://gitee.com/winkine/nw-chat | Qt/QML cross-platform intranet messaging, P2P server-free; client-architecture reference |
| LANShare | https://github.com/ByteMini/LANShare | Go; decentralized LAN communication |
| IP Messenger (IPMsg protocol source) | protocol ports UDP 2425 / TCP 2426 | Released 1996; open source; many interoperating clients |
| FeiQ official site | http://feiq18.com | Closed-source free; IPMsg-compatible; unmaintained for years |
| FeiQ Android (protocol reference) | https://www.52pojie.cn/thread-2125688-1-6.html | Open source; IPMsg implementation compatible with Windows FeiQ |
| RustDesk | https://github.com/rustdesk/rustdesk | AGPL-3.0; self-hosted remote desktop; **excluded, not an integration target**; architecture-layering reference for remote assist only (signaling/relay/codec separation) |
| 喧喧 Xuanxuan | https://github.com/xuanim/xuanim | AGPL-3.0. **Excluded, not an integration target**; self-hosted-IM architecture reference only; free edition capped at 50 users |
| MeshCentral | https://github.com/Ylianst/MeshCentral | Apache-2.0; remote-management platform with its own agent. **Evaluated and excluded** (authorization interaction and account system mismatch; integration cost above self-development); reference only |
| Apache Guacamole | https://guacamole.apache.org | Apache-2.0; browser remote-desktop gateway (RDP/VNC/SSH). **Evaluated and excluded** (depends on OS-level remote services; cannot carry local-dialog authorization); reference only |
| FreeRDP | https://github.com/FreeRDP/FreeRDP | Apache-2.0; RDP protocol library. **Evaluated and excluded** (same as Guacamole); reference only |
| Qwen models | https://github.com/QwenLM | Apache-2.0; strong Chinese |
| DeepSeek models | https://github.com/deepseek-ai | MIT-family |
| vLLM | https://github.com/vllm-project/vllm | Apache-2.0; high-throughput inference |
| Ollama | https://github.com/ollama/ollama | MIT; simple deployment, fit for validation |
| FunASR | https://github.com/modelscope/FunASR | Apache-2.0; Chinese speech recognition |
| BGE embedding models | https://github.com/FlagOpen/FlagEmbedding | MIT; Chinese semantic retrieval |
| Qdrant | https://github.com/qdrant/qdrant | Apache-2.0; vector database |
| OnlyOffice Document Server | https://github.com/ONLYOFFICE/DocumentServer | AGPL-family; collaborative-document integration option |
| Collabora Online | https://github.com/CollaboraOnline/Online | MPL-family; collaborative-document integration option |
| LiveKit | https://github.com/livekit/livekit | Apache-2.0; conferencing integration option |
| WebRTC | https://webrtc.org | W3C/IETF standard, reference implementation BSD-3-Clause; this plan's media-transport foundation — standards/protocol interop, not an integration target |
| libdatachannel | https://github.com/paullouisageneau/libdatachannel | MPL-2.0 (file-level weak contagion, safe to link); lightweight WebRTC for the Qt route |
| Cloudreve | https://github.com/cloudreve/Cloudreve | Actively maintained web file platform |
| Kodbox | https://github.com/kalcaddle/kodbox | Actively maintained web file platform |

> **Note** Licenses, platform support, and maintenance status cited above come from the projects' official repositories or sites, verified as of October 2026, and change over time. The table distinguishes project status: "excluded" entries are not selections, only architecture references; unmarked entries are reference implementations or components pending integration. Re-verify before formal approval: license terms of components to be integrated (especially whether OnlyOffice and Collabora remain independently deployed processes), current GPU server models and supply, actual WebRTC availability on target domestic terminals, and the actual scale of FeiQ clients on the target network.
