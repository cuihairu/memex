> **English** | [中文](权限模型.md)

# Permission Model: User-Group-Role-Grant (Cross-cutting Design)

> Status: design draft (2026-10-04 user directive: "similar to Linux account management"). Positioning: the unified permission model shared by R23-R26 — group = group, joining a group grants inheritance, creating a group requires privilege, group capabilities are configured by admins. Cross-references: group tools framework (R25), group server tools (R26), group knowledge sharing (R24), organization structure (groups are visible in the organization structure).
>
> **Implementation status (2026-10-08 reconciliation, second batch closed out)**: all six model rules are landed — in-group roles (group owner/admin/member) and "join to inherit, leave to lose" landed with R23-2 (`group_members.role` + AuthorizationService policy registration); "group creation requires grant" landed with platform-12 (202d626: CollabServer group_az_ adjudication, adding role `group_creator` (org role grant, time-window mechanism) or basic admin pass-through, default deny when ungranted); "group capabilities fully configured by admin" landed with platform-12 establishing the table and vocabulary (`group_capabilities` seven items: notice/memo/vault/tools/server_tools/files/uplink; unconfigured = currently allowed, configured deny = reject — first-phase step-in stance, full deny-by-default left for later) + change ledger + CLI `group capability set|list`; "groups visible in organization structure" and "R24-R26 surfaces wired per the model" landed with the second batch close-out: ORG_DATA carries `OrgGroup` full group distribution (/etc/group analogy, transparent and queryable, not trimmed by viewer), client organization tree lists group members, FileServer `capability_gate` unified gate covers memo/vault/tools (including ci/pack/export)/server_tools four surfaces 37 routes + files surface four verbs (upload/list/download/pin/delete), disabled = 403 `deny:capability-<name>`; "full trace-leaving" = group creation deny/grant/capability changes all land in the ledger. The vocabulary's uplink item has no execution point yet (the current external-network inbox surface is per-account with no group dimension; it will be wired when the group inbox surface lands).
>
> **Conflict and inheritance (2026-10-10, plan book §8 close-out)**: the tier order Explicit Deny > Explicit Allow > Inherited > Default Deny is written into the engine and this document's §5; the scope dimension is landed — `add_scoped_rule` attaches department paths, queries carry `scope`, only on-chain can hit, within the same tier the nearest scope wins (same depth same tier: first registered first matched), existing global rules' behavior unchanged.

## 1. Model (Linux group model + in-group roles + trace-leaving)

```
User(User) ──joins──► Group(Group, permission carrier) ──configures──► Grant surface(tool/resource/action whitelist)
                        │
                        └─ In-group roles: group owner / admin / member
```

| Rule | Content | Linux analogy |
|---|---|---|
| Join to inherit | Tools/resource capabilities configured for the group are automatically obtained by members | `usermod -G` inherits group permissions |
| Leave to lose | Exit/removal revokes immediately | Removed from group |
| Group creation requires grant | Creating a group is a privileged action (granted or approved by organization-structure admins) | `groupadd` requires privilege |
| Group capabilities fully configured by admin | Which tools the group uses (CI/CD/server/files/vault…) is checked and configured by admins | Group ACL/sudoers |
| Groups visible in organization structure | All groups enter the organization structure tree (transparent and queryable) | `/etc/group` |
| Full trace-leaving | Who exercised what capability is fully recorded | (Linux has no such layer) |

## 2. In-group roles (one more layer than Linux)

- **Group owner**: the owner of the group (configures tools, appoints/removes admins, transfers/dissolves);
- **Admin**: tool configuration, member management, announcement/memo maintenance;
- **Member**: uses the capabilities the group has configured.

Roles are an in-group layering, not cross-group (a user is an admin in group A and a member in group B, without mutual influence).

## 3. Grant surface (the admin's "configure all" list)

A group's capability switches (checked by admins): group announcement / group memo / group vault / group tools (CI/CD, packaging, config export…) / group server tools / group files / external-network upload inbox. Each capability can further carry a fine-grained whitelist (e.g. CI/CD only allows triggering which pipelines).

## 4. Boundaries

- Organization-structure management (creating organizations/departments, granting who can create groups) belongs to the organization-management surface (memex already has an organization structure; this model only defines groups and permission relationships);
- Credentials (vault/server credentials/tool tokens) are never made plaintext-visible by "joining a group" — **permission = exercisable actions, credentials still go through the encryption surface + explicit unlock**.

## 5. Conflict and inheritance (plan book §8, landed 2026-10-10)

**Tier order written in (global priority, independent of depth)**:

1. **Explicit Deny** (explicit rejection, overrides any Allow);
2. **Explicit Allow** (explicit grant: owner, group owner/admin management rights);
3. **Inherited** (inherited grant: group members' established capabilities);
4. **Default Deny** (whitelist stance: reject when no rule hits).

**Scope dimension**: rules can carry a department-path scope (e.g. `公司/技术中心/游戏部`), and queries carry the same scope. A rule's scope **must be on the query chain** (equal to that path, or a subordinate whose path starts with it + `/`) to possibly hit; off-chain does not hit. A rule without scope = global, always covers.

**Nearest within the same tier**: when multiple rules in the same tier hit, take the **nearest scope** (the one with the most path segments); same depth same tier takes **first registered** first matched (deterministic). Inter-tier order is not affected by scope — a Deny attached to the deepest scope still overrides an Allow attached to a shallower one. When a query has no scope, only global rules can hit.

**Existing execution points**:

- The department-chain level-by-level policy resolution (`resolve_policy` / `assist_policy_resolve`: own department → level-by-level superiors → global row → built-in default) is in the same direction as this engine — nearest scope first;
- Data filtering (blueprint §7) and single-point adjudication share the same source: `filter_allowed` runs each row of the query result set through the same rule bucket, rows that should not be seen are filtered out at the server rather than hidden in the UI;
- The reason trace-leaving format is unchanged: `deny:<name>` / `allow:<name>` / `inherited:<name>` / `default-deny`, and the audit uses this to answer "who was allowed/denied, based on which rule".
