> **English** | [中文](办公室位置图.md)

# Office Location Map: Floor Plan + Point Editing + Search & Locate

> Status: first-phase minimal closed loop implemented (2026-10-08). Cross-references: platform-3 Organization (seat occupant = account, display uses organization-structure name), platform-1 authorization service (edit right goes through az), permission model (edit trace-leaving), R23-3 file assistant (independent file-surface session precedent).

## 0. Scope decision

- **First-phase minimal closed loop**: floor plan (grid-coordinate abstract plane, no real CAD base map) + seat points (drag-style editing) + search & locate (by account/seat number) + floor visibility (default: own floor only).
- **Not done** (honestly noted for later): real base map upload, multiple buildings, meeting-room/equipment points, seat reservation flow.

## 1. Data model

- `office_seats` table: `id / floor TEXT (floor number like "3F") / label TEXT (seat number, UNIQUE(floor,label)) / x REAL / y REAL (plane normalized coordinates 0~1) / account TEXT (occupant account, empty = empty seat)`.
- **One person one seat**: `account` non-empty has a partial unique index (seat change = unbind first, then bind).
- A seat is the floor attribution: **own floor = the floor of the seat one occupies** (not derived from organization-structure department — department ≠ floor, no mapping is fabricated).

## 2. Visibility and adjudication

- **View** (GET /files/office-map): if the user has a seat → returns **all points on this floor** (including colleagues' occupancy; same-floor mutual visibility is the semantic essence of the location map); no seat → empty map (floor empty). org-admin viewing can use `?floor=` to specify a floor (cross-floor management surface).
- **Edit** (az verb `office:manage`, ExplicitAllow = effective_roles contains org-admin): seat add/delete/reposition (drag-and-drop landing = upsert), bind/unbind occupant. Miss = default-deny.
- **Trace-leaving**: bind/unbind/create/delete go through std::cout server-side trace-leaving (same watermark as approval decisions); drag-reposition is high-frequency and low-sensitivity, only persisted to DB without log spam.

## 3. Surface and interaction

- Server three routes: GET /files/office-map (own floor or specified floor points), POST /files/office-map/seat (upsert{floor,label,x,y}; remove=true goes delete), POST /files/office-map/bind ({id,account}, empty account = unbind; ghost rejected).
- Client OfficeMapDialog (independent file-surface session): abstract plane canvas (QPainter grid + dots + seat number/account labels); org-admin can drag-and-drop to land (save on release) and add/delete/bind; search box filters and highlights by account/seat number (hits bright, rest dimmed); can_manage is adjudicated by the server az and returned (client does not self-decide).
