> **English** | [中文](../zh/design/群组知识共享.md)

# Group Knowledge-Sharing Design: Announcements / Memos / Password Vault (R24 Design Spec)

> Status: design spec (user requirement 2026-10-04). Scope: three kinds of group-scoped shared information — announcements (notifications), memos (shared knowledge), password vault (shared credentials). Cross-references: todo R24, File Storage and One-Way External-Network Transfer (R23), wingman vault encryption scheme (reused).

## 1. The Three-Part Set and Their Boundaries

| Feature | Semantics | Visible to | Writable by | Sensitivity |
|---|---|---|---|---|
| **Group announcement** | Notification (pinned, visible to all members, history kept) | Group members | Group owner/admin | Low (plaintext) |
| **Group memo** | Shared knowledge entries (title + content): environment info, WiFi instructions, on-call roster | Group members | Maintained by admin (optionally open member editing + trail) | Medium (plaintext, **no passwords allowed**) |
| **Group password vault** | Shared credential entries (name/account/password/URL/notes) | Authorized members (explicit unlock) | Maintained by admin | **High (ciphertext end to end)** |

**Key separation**: memos and the password vault **never mix** — memos are plaintext, the vault is ciphertext; passwords go into the vault only, and the memo UI shows a hint for content that looks like a password. User scenarios mapped: backend-team environment info → memo; WiFi password for a large group → password vault (WiFi instructions as text → memo).

## 2. Group Password Vault (Core Security Surface)

- **Encryption scheme implementation references persona (user-designated reference)**: persona = an account/vault system aligned with 1Password (core/src/crypto, ssh-agent, 1pux import) — the encryption surface (KDF/AEAD/key derivation), SSH key generation and import (crypto/ssh_generate, ssh_import), and agent-style usage (ssh-agent mode) directly reference persona's implementation; no new crypto wheel is built. The wingman vault (PBKDF2 600k→KEK→AES-256-GCM wrapping a DEK) serves as a corroborating precedent of the same kind.
- **View semantics**: explicit unlock (enter the vault password / session unlock), **every view leaves a trail** (who / when / which entry — audited into the group log); the list display is masked by default (only name/account shown).
- **Export is off by default**; copying a password = an explicit action + trail.
- Permissions: admin maintains entries + an authorization list (whether all members can view by default or only admin-authorized members — **all members can unlock by default** (the point of sharing); the group owner can change the authorization list).

## 3. Group Announcements and Group Memos

- **Announcements**: fixed entry in the pinned area, edit history kept (who changed it / when), @all-members linked notification semantics (reuse the three-tier push: announcement = important strong reminder).
- **Memos**: entry list (title + body, supporting simple sections / code blocks — environment info often carries configuration), search embedded; when open editing is enabled, each entry keeps a revision history with rollback.

## 4. Deliverables (todo R24)

- R24-1 Group announcements (publish / pin / history / notification linkage)
- R24-2 Group memos (entry CRUD / search / revision history / permissions)
- R24-3 Group password vault (encrypted storage / explicit unlock / view trail / authorization list / re-wrapping)
- R24-4 UI boundaries: password-lookalike hint in memos, masked display in the vault, export off by default
