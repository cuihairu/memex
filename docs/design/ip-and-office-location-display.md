> **English** | [中文](IP与办公位置显示.md)

# IP and Office Location Display

> Status: design draft (2026-10-04 user decision). Positioning: the display rules for member network identity and office location on the session/profile surface.

## Rules

- The peer info displays two fields: **IP** and **office location** (office location comes from organization structure/profile, empty if none).
- **Two setting switches**:
  - **IP display**: **on** by default;
  - **Office location display**: **off** by default.
- **When office location is enabled and the peer has no office location info, that item is not displayed** — no placeholder, no empty label (no info = no such row).
- The switches affect only the local display (one's own view), they do not change server-side records; IP/location are still only used for trace-leaving audit.
- Desktop and mobile share the same switch semantics (Settings · Privacy/Display Settings).
