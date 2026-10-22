# Client Settings

> Client settings batch (2026-10-08 user order, six items: notification toggle group /
> three-level notification sound / screenshot shortcut / away lock / network & remote /
> settings page grouping) landed item by item; this page continues to be written in
> step with the batch. See "Settings Page Grouping" at the end for the grouping roll-up.

## Shortcuts

- **Screenshot shortcut**: default `Ctrl+Alt+A`; change the key under Settings →
  「快捷键…」 (entered via QKeySequenceEdit, effective on save — the main-window shortcut
  rebinds immediately, no restart needed).
- **Conflict detection**: on save, the key is compared against the other shortcuts inside
  the window (QShortcut and menu items that carry keys); a conflict is flagged in red text
  and the original key is kept; an empty key sequence is refused.
- **Persistence**: QSettings `shortcuts/screenshot`, read as soon as the main window is
  constructed; the button tooltip shows the current key in sync.
- Scope note: this is a **window-scoped** shortcut; 「全局热键」 is a separate batch gated on
  platform capability — the Wayland security model does not allow global hooks, and global
  key capture on X11 is a separate desktop-integration batch.

## Notifications and Sounds

See [Notifications and Webhook Integration](/guide/notify), sections 「个人通知偏好」 and
「事件通知开关组与提示音」; after the grouping roll-up the entry point remains Settings →
「通知偏好」.

## Away and Lock Screen

Settings → 「离开与锁屏…」; three things: away password, idle timeout, and an enable switch.

- **Away password**: the local QSettings store keeps only the salt + SHA-256
  (`away/pwd_salt` / `away/pwd_hash`); plaintext never lands on disk; the salt is regenerated
  every time a password is set (the same password yields different hashes on two machines).
  Once a password is set, leaving the field blank means "no change".
- **Idle timeout**: any input — mouse move/click, keyboard, scroll wheel — restarts the
  countdown; on timeout (`away/timeout_sec`, configured in minutes 1–120 on the settings
  page, default 5 minutes) the screen locks automatically. If no password is set, saving
  with the switch on is refused (without a password a lock holds nobody, so no lock is
  installed).
- **Lock screen face** (acceptance screenshot): a frameless,
  always-on-top, full-screen dark mask — **it shows only the unread message count, never the
  content**; while locked, newly arriving messages/files only increment the counter, and
  popups and sounds are fully silenced (the notification face records no content); an
  online event is not counted as unread. The counter resets to zero on unlock (conversation
  content stays where it is in the chat area and is visible once unlocked).
- **The only way out is the password**: Esc / the close button / Alt+F4 are all refused; a
  wrong password leaves the red text as it is and the right password unlocks. Timing restarts
  after unlock.
- Scope note: the lock screen is a **local mask** — the presence online table has no away /
  leave state field, so a contact on the other side sees no 「离开」 marker (extending the
  protocol with per-user state is a separate batch).

## Network and Remote

Settings → 「网络与远程…」; two things: subnet blacklist, and the remote-control pairing
password.

- **Subnet blacklist**: segment format `a.b.c.d/prefix-length` (prefix 0–32, e.g.
  `192.168.10.0/24`). **Off by default = allow everything**; once enabled, a segment-table hit
  means a direct-connect discovery packet gets **no reply** (dropped and never entered into
  the neighbor table) plus a new inbound connection is **rejected on the spot** (the TCP
  connection is dropped as soon as it is established). Segment input passes the local
  `validate_cidr` gate — a bad segment (missing slash / not dotted-quad / octet out of range /
  prefix out of range) is flagged in red and enters neither the table nor the store. Saving
  writes QSettings (`net/blacklist_enabled`, `net/blacklist`) and reinstalls the filter
  immediately — flipping the switch or editing the segment table at runtime applies to the next
  batch of packets and connections under the new table; switch off = no filter installed
  (allow everything).
- **Remote control**: **off by default, and enabling it requires an explicit action** — while
  off, the controlled side approves a remote-assistance request straight through the approval
  flow with no password; **once enabled, the pairing password must be entered before approval**
  (wrong password = refused, nothing goes on the wire; cancel = the request stays pending
  approval); 「先设配对密码，才能开启远程控制」 is the save gate (turning on remote control
  without a password = a lock that holds nobody).
- **Pairing password**: the local QSettings store keeps only the salt + SHA-256
  (`remote/pwd_salt` / `remote/pwd_hash`); plaintext never lands on disk; the salt is
  regenerated on each set; if already set, leaving it blank means "no change"; two mismatched
  entries are refused.
- Scope note: the blacklist blocks **new connections at both the discovery and the inbound
  entry points** — already established connections are not torn down retroactively (a
  controlled session has its own end / revoke paths); IPv4 segment matching only, IPv6
  neighbors do not take part in segment matching.

## Settings Page Grouping

(Batch in progress: the five groups — notifications / sounds / shortcuts / away & lock screen /
network & remote — will be rolled up into unified grouping, and this page will be continued
after that lands.)
