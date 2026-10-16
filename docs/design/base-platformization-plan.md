> **English** | [中文](基础平台化改造计划（身份-组织-授权-归档）.md)

# Base Platformization Plan (Identity / Organization / Authorization / Archive)

> Source: the "Memex Complete Improvement Plan" submitted by the user on 2026-10-04 (original text archived; wording unchanged).
> Positioning: **keep the "dual-engine + trail + intranet + self-developed" core, and raise Identity / Organization / Authorization / Archive into a true foundational platform**. This document is the sole blueprint for the subsequent transformation (P0 first); execution is broken into todo items and coded one by one.
> Related: [建设方案评审报告-V10.0](../../src/guide/decisions.md 同为本文依据)，[decisions.md](../../src/guide/decisions.md)。

---
## 一、Final Product Positioning

Today's README says "intranet office instant-messaging system", which is not enough to describe the project. The real product structure:

```text
Memex
│
├── Communication
│   ├── Direct P2P
│   └── Server Collaboration
│
├── Identity
│   ├── Account
│   └── Device
│
├── Organization
│   ├── Department
│   ├── Member
│   └── Reporting Line
│
├── Authorization
│   ├── Role
│   ├── Permission
│   ├── Scope
│   └── Policy
│
├── Archive
│   ├── Message
│   ├── Event
│   ├── Audit
│   └── Search
│
├── File
│   ├── P2P Transfer
│   └── Shared Space
│
├── Office
│   ├── Approval
│   ├── Todo
│   ├── Location
│   └── Remote Assistance
│
└── AI
    ├── Assistant
    ├── Summary
    └── Retrieval
```

One-line product positioning:

> **Memex is an intranet communication and collaboration platform for physically isolated networks; through the dual engines of zero-config LAN direct connection and server-side collaboration, it provides organization management, permission control, message archiving, file sharing, office collaboration, and intranet AI capabilities.**

"Instant messaging" is still the product category, but not the definition of the entire architecture.

---

## 二、First Priority: A True Identity Model

The existing data model `accounts / devices / sessions` is in the right direction (V10 explicitly covers account login across device switching, single-point-online of the same kind on desktop, device fingerprint, login records). Further clarifying:

```text
Identity
├── Account
├── Device
├── Session
├── Credential
└── IdentityBinding
```

### Account
```text
account_id
employee_id
name
status
department_id
manager_id
```

### Device
```text
device_id
device_fingerprint
device_type
os
hostname
owner_account_id
status
```

### Session
```text
session_id
account_id
device_id
login_at
expire_at
ip
client_version
status
logout_reason
```

### Credential

Do not cram the password directly into the Account concept; reserve room for multiple credential forms:

```text
Password
Token
Certificate
SSO
Device Credential
```

The identity system is not locked to "account + password".

---

## 三、Second Priority: Authorization Becomes a Formal Independent Layer

Today's state (departments, members, roles, permissions, address-book visibility, policy, session audit, location map, remote assistance) looks more like "each feature's own permission requirements". Upgrade to:

```text
Authorization
│
├── Permission
├── Role
├── Scope
├── Policy
└── Decision
```

Not just traditional RBAC.

### 1. Permission: What One Can Do

```text
user.read / user.update
department.read / department.manage
message.search / message.export / message.audit
device.read / device.manage / device.kick
file.read / file.write / file.share
remote_assist.request / remote_assist.control
location.read / location.manage
policy.read / policy.update
```

### 2. Role: Who Can Do What

```text
Employee
IT
HR
DepartmentAdmin
SecurityAuditor
SystemAdmin
```

A role is just a set of permissions.

---

## 四、Most Important: Scope

A permission is not simply `can message.search = true`, but `can message.search WHERE department = X`:

```text
SELF
DEPARTMENT
DEPARTMENT_AND_CHILDREN
ORGANIZATION
SPECIFIC_USERS
SPECIFIC_DEPARTMENTS
```

Examples: HR → `user.read` + scope=HR_DEPARTMENT; IT → `device.manage` + scope=ALL; auditor → `message.search` + scope=AUTHORIZED_DEPARTMENTS.

---

## 五、Policy

Separate Permission from Policy:

```text
Permission: remote_assist.control      → has the remote-assist capability
Policy: remote_assist                 → under what conditions it may be used
    allowed_department = IT
    require_consent = true
    require_audit = true
    record_screen = optional
```

(The document already makes explicit that remote assistance requires "explicit authorization, continuously visible, full trail"; the location map, cross-state communication, new-device login and the like are also under policy control.)

**Authorization Decision flow:**

```text
Subject → Role → Permission → Scope → Policy → Resource → Allow / Deny
```

---

## 六、Permission Decisions Must Be a Unified Service

`if (user.department == ...)` must not be scattered across org/archive/file/location/remote/admin:

```text
AuthorizationService.authorize(subject, action, resource, context)
→ ALLOW / DENY
→ with reason, matched_policy, scope
```

Only this way can the audit answer "why this person can see this message".

---

## 七、Permissions Must Support "Data Filtering", Not Just Operation Permissions

Address-book "invisible + unsearchable" must not live only in the UI layer as `if (!visible) hide()`; it must be in the data access layer:

```text
Server Query → Authorization Filter → Allowed Records → Client
```

Otherwise the search API can still return hidden users. **Authorization must take effect at the data access layer, not just the UI layer.**

---

## 八、Permission Inheritance and Conflict Rules

Under a multi-layer structure (company → technology center → game dept / infrastructure dept), the adjudication priority of "company allows / technology center allows / game dept denies" must be fixed in writing:

```text
Explicit Deny > Explicit Allow > Inherited Policy > Default
```

Or simpler: "default deny + nearest scope wins". **Must be written into the design doc**, otherwise permissions get out of control the moment they grow complex.

---

## 九、Policy Is Not a Mixed Bag of "Department Switches"

Unified Policy structure:

```text
Policy
{
    resource
    action
    subject_scope
    condition
    effect
    priority
    effective_at
    expire_at
}
```

The existing `policies(department_id, policy_item, value)` is gradually upgraded to avoid becoming a key/value mixed bag.

---

## 十、The Dual-Engine Design Is Preserved and Strengthened

```text
                 Memex Client
                      │
       ┌──────────────┴──────────────┐
       │                             │
 Direct Engine                Collaboration Engine
       │                             │
 UDP Discovery                   Gateway
 TCP P2P                         Account
 P2P File                        Org
       │                         Archive
       │                         Policy
       └──────────────┬──────────────┘
                      │
                 Shared Domain
                      │
          SQLite / Conversation /
          Contact / File / UI
```

**Do not merge the two Engines into one Transport.** (Keeping the current trade-off.)

---

## 十一、Modify One Important Behavior: No "Silent Degradation"

Falling back to direct connection automatically when the server is unreachable is good for availability, but dangerous from the trail perspective:

> The user thinks they are still logged in and archiving, while actually the server is down and it auto-Directs, and messages are not archived — a cognitive risk.

It must be displayed explicitly that "the current state is unarchived communication", and the coverage must be unified:

```text
top status bar / conversation marker / input-box hint / pre-send state / resend rules after recovery
```

---

## 十二、Cross-State Communication Defined as an "Explicit Risk Domain"

```text
Communication Domain
ARCHIVED        (archived)
UNARCHIVED      (direct connection without login, not archived — the current design is preserved)
DEGRADED        (should be archived but the server is unreachable, degraded and unarchived — must be marked explicitly)
```

Not simply online/offline.

---

## 十三、Archive Model: Event-Sourcing-Style Immutable Records

The existing direction of "recall does not delete the server-side original, only adds a recall event" is correct; unify it as:

```text
Message             (original message, immutable)
MessageEvent        (append-only: message.created / delivered / read / recalled / edited)
Current Message State (reconstructable from events)
```

Audit, recall, edit, delivery, read, and export all benefit.

---

## 十四、The "Permanent Retention" Decision Must Be Re-decided

decisions.md contains a contradiction: the report suggests 6 months, while the engineering wording became "permanent retention, cleanup configurable".

Change the wording: **cannot be deleted by ordinary users; the final lifecycle is configured by an administrator.**

```text
Retention Policy
├── 30 days
├── 6 months
├── 1 year
├── 3 years
└── Indefinite
```

"Non-tamperable" and "legitimate lifecycle management" do not conflict.

---

## 十五、Even Administrators Cannot Delete Without a Trace

Admin Delete must become Retention Purge, and record who/when/what/why/policy/approval:

```text
High Risk Operation → Two Person Approval → Purge → Audit Event
```

message export / message purge / audit access / remote control / policy change all belong to high-risk operations.

---

## 十六、Session Audit Permissions Are Independent of Ordinary Administrator Permissions

```text
SystemAdmin     system.manage, device.manage    (≠ can read messages automatically)
SecurityAuditor message.audit, message.search, message.export
DepartmentAdmin user.manage, department.manage
```

It is forbidden to equate `Admin = can read all chats`.

---

## 十七、The Audit Itself Also Needs Auditing

```text
Auditor A → queries employee B → views 2026-10-01 messages
→ generates audit.message.search / audit.message.view / audit.message.export
```

```text
Business data → Audit → Audit log (itself not modifiable by ordinary administrators)
```

Otherwise "an administrator reading an employee's chats" is itself a black box.

---

## 十八、The File System Distinguishes Two Concepts

```text
File Transfer    = message attachment / temporary transfer (client file cabinet = quick handoff)
File Space       = persistent collaborative storage (central shared file platform, separate in phase two)
```

Do not merge the two into one File.

---

## 十九、The File Bypass Must Add "Transfer Authorization"

```text
Server ── metadata      A ──────── B (bytes do not pass through the server — preserved)
```

But this must be solved: does A have permission to send? Does B have permission to receive/download? Is the file allowed to cross departments? Is re-forwarding allowed?

**FileAuthorization must call the unified Authorization Service** — the file does not pass through the server, but permissions must not bypass the server.

---

## 二十、P2P Must Not Be "Trusted" Merely Because It Is on the Intranet

UDP broadcast discovery + TCP P2P means "can reach the same layer-2 network ≠ a trusted user". At minimum add:

```text
Device Identity + Authenticated Handshake + Session Key + Message Integrity
```

UDP broadcast itself does not carry identity authentication:

```text
UDP Discovery only → TCP → Cryptographic Handshake → Authenticated P2P Session
```

---

## 二十一、The Direct Engine Defines an Explicit Encryption Boundary

```text
Discovery: device_id / public_key / capabilities
→ X25519 Session Key → AEAD → Message / File
```

Packets captured on the LAN cannot be read directly.

---

## 二十二、The Message Protocol Decides ID / ACK / Idempotency in Advance

```text
message_id
conversation_id
sender_id
client_sequence
server_sequence
created_at
```

Delivery semantics: at-most-once / at-least-once; **the actual recommendation is "client sends at least once + server-side message_id idempotency dedup"**, and exactly once at the network layer is not pursued.

---

## 二十三、The Client Local DB Formally Designs Sync State

`source + sync_state` carries too much semantics; upgrade it to an explicit state machine:

```text
LOCAL / PENDING / SENDING / SERVER_ACKED / ARCHIVED / FAILED
+ archive_state
```

---

## 二十四、Organization Structure and Permissions Are Fully Separated

Do not keep piling department/role/manager into Account:

```text
Account
    ├── Membership       → Department (one person, multiple departments)
    ├── RoleAssignment   → Role (one position, multiple roles / temporary delegation)
    └── ReportingLine    → Manager
```

---

## 二十五、The Direct Manager Field Is Kept Independent

**A direct manager cannot be derived from the department structure** — the Department Tree and the Reporting Line are two different graphs and must not be merged into one tree.

---

## 二十六、Location Map Permissions Are Not Just "Department-Visible"

The location map is sensitive spatial information:

```text
Location: Visibility / Accuracy / Role / Purpose
```

Ordinary employees see their own department; IT sees the whole building; HR sees the organization scope; it can even be "name only, no precise seat". Do not turn it into an enterprise real-time tracking system.

---

## 二十七、Remote Assistance Designed as an Independent Security Domain

Keep the existing principles (explicit confirmation, continuously visible, full trail, controllable scope); the model:

```text
RemoteAssistSession
{
    id, requester, target
    requested_at, approved_at, started_at, ended_at
    permissions: view / keyboard / mouse / clipboard / file_transfer
    consent, audit
}
```

Instead of a single `remote_control = true`.

---

## 二十八、AI Permissions: Formally Reuse Authorization

V10 is already correct (AI reads messages carrying the caller's identity and does not bypass user permissions because the AI is a service). Elevate it to a system principle:

```text
AI is a user of Memex
User → Authorization → AI Service → Retrieval
```

Instead of `AI Service → Database → Everything`.

---

## 二十九、Content Generated by AI Is Separated from Original Messages

```text
Content
├── Message
├── File
├── Event
├── AIArtifact     (artifact_id / type / creator / model / input_scope / created_at / content)
└── AuditEvent
```

Keep today's approach of "AI minutes/requirement entries saved as independent types, including model, input scope, and generator".

---

## 三十、A Single Centralized Deployment Is a Phase-1 Trade-off, Not an Architecture Ceiling

```text
Phase 1: Gateway / Account / Org / Archive / Policy / FileMeta → one Deployment
Future:    Gateway × N / Archive × N / DB HA / Object Storage
```

Do not write "a single server" as "the system can only have a single server".

---

## 三十一、Do Not Microservice-ify the Database Too Early

gateway/account/org/archive/policy/filemeta under server/ should be understood as **module boundaries**, not six processes. Phase 1 is single process, single DB, with logical splitting reserved (V10's trade-off is reasonable):

```text
MemexServer (one process, clear internal interfaces)
 ├── Gateway
 ├── Account
 ├── Org
 ├── Authorization
 ├── Archive
 ├── Policy
 └── FileMeta
```

---

## 三十二、Authorization Becomes the Seventh Core Module

```text
gateway / account / org / authorization(new top-level) / archive / policy / filemeta
```

- **Org**: what organization people belong to.
- **Authorization**: what people can do to what.

The two must be kept separate.

---

## 三十三、The Admin Console Is Re-divided

```text
Admin
├── Identity
├── Organization
├── Authorization
├── Devices
├── Archive & Audit
├── Policy
├── Files
└── System
```

The console structure aligns with the Server Domain.

---

## 三十四、Phase-1 Scope Is Phased (No Bloat)

20–25 person-weeks is on the optimistic side. Phase 1 is split again:

- **R1-A Communication fundamentals**: Direct / Collaboration / Message / Conversation / Local DB
- **R1-B Identity & Organization**: Account / Device / Session / Department / Membership / Basic Role
- **R1-C Archive**: Archive / Search / Recall / Audit / Retention
- **R1-D File**: P2P Transfer / Metadata / Resume / Hash
- **R1-E Production**: Permission / Security / Upgrade / Backup / Recovery / UOS / Kylin / Windows

---

## 三十五、Advanced Permissions Can Go to Phase Two, but the Authorization Framework Must Be Phase One

Phase 1 must have:

```text
Identity / Role / Permission / Scope / Policy / Audit
```

Phase two only adds Approval / Location / Remote / Advanced policy permissions. Otherwise you write `if admin` first and rework AuthorizationService in phase two — the cost is enormous.

---

## 三十六、Core Phase-1 Acceptance Metrics

Not "can chat", but:

> **In the dual-engine environment, identity, permissions, and archive state are always explainable.**

Any message can answer:

```text
Who sent it? Which device? When? Through which Engine? Is it archived? Why archived / not archived?
Who can view it? Who has viewed it? Was it recalled? Does the recall affect the original record?
```

If these can be answered, the foundation is formed.

---

## 三十七、Final Recommended Overall Architecture

```text
                         Memex
                           │
        ┌──────────────────┼──────────────────┐
        │                  │                  │
     Client              Server             Admin
        │                  │                  │
 ┌──────┴──────┐     ┌─────┴────────┐         │
 │             │     │              │         │
Direct      Collab  Identity     Organization │
Engine      Engine     │              │        │
 │             │       └──────┬───────┘        │
 └──────┬──────┘              │                │
        │                 Authorization        │
        │                      │                │
        └──────────┬───────────┤                │
                   │           │                │
                Domain       Policy              │
                   │           │                │
          ┌────────┼───────────┼────────┐       │
          │        │           │        │       │
       Message   File       Archive   Audit    │
          │        │           │        │       │
          └────────┴───────────┴────────┘       │
                                               │
                                     ┌─────────┴─────────┐
                                     │                   │
                                  Search              Manage
```

The AI position (never bypassing Authorization):

```text
AI Service → User Identity → Authorization → Retrieval → Allowed Data → Model
```

---

## 三十八、Final Priority

### P0: Must Be Fixed Now
1. **Identity Model**: Account / Device / Session / Credential
2. **Authorization Model**: Permission / Role / Scope / Policy / Decision
3. **Archive Model**: Message / MessageEvent / AuditEvent / Retention
4. **Direct Security**: Device Identity / Handshake / Session Key / AEAD
5. **Collaboration Protocol**: message_id / sequence / ack / idempotency / offline queue / resume

### P1: Phase-1 Implementation
```text
Direct Engine / Collaboration Engine / Identity / Organization / Authorization
Message / Archive / Search / File Transfer / Basic Admin / Audit
Windows / UOS / Kylin
```

### P2: Phase Two
```text
Advanced RBAC / Scope / Policy / Approval / Todo / Calendar
Office Map / Remote Assist / Advanced Audit / Shared Space
```

### P3: Phase Three
```text
AI Service / AI Retrieval / Group Assistant / Meeting Summary
Requirement Extraction / Local Model / AI Audit
```

### P4: Phase Four (Not Self-Developed)
```text
Video Conference / Collaborative Documents → WebRTC / LiveKit / Jitsi / OnlyOffice
(preserve process/service boundaries)
```

---

## 核心原则（取代「连接服务端的唯一目的即留痕」）

> **通信、身份、权限与留痕必须始终可解释。**

1. **Direct First**: basic communication works on the LAN even without a server.
2. **Server Collaboration**: the server provides identity, organization, collaboration, cross-subnet communication, and archive capabilities.
3. **Authorization Everywhere**: all protected data and operations must go through unified authorization, not be decided by business modules themselves.
4. **Audit by Design**: high-risk behaviors such as archiving, recall, search, export, permission changes, and remote assistance must all produce traceable records.

**The single change with the highest priority for execution:**

> Pull **Authorization** out of `org` / `policy` and make it a core Domain on the same level as Identity, Organization, and Archive, with the basic framework completed in phase one.

Reason: the dual-engine design is mature and the archive principles are clear; what is truly missing is a backbone that runs through all features — the unified authorization model. Once the backbone is in place, the address book, files, audit, location map, remote assistance, and AI can all hang onto it naturally.

---

*The blueprint above is entirely organized from the user's original words of 2026-10-04 (the heading numbering 一~三十八 is preserved); the execution breakdown is in the corresponding entries of todo.md.*
