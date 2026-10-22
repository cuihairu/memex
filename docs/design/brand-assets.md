> **English** | [中文](../zh/design/品牌物料.md)

# Brand Assets: Server-Side Configurable Company Branding; the Client Rebrands Itself On Connect

> Status: shipped (design finalized 2026-10-08; the server batch + client access batch + settings page
> batch have landed; implementation differences backfilled in §2/§4/§6 — cache path, signal-driven form,
> the clear routes, and the fetch timing differ from the original spec in four places; the backfilled
> version prevails). Goal: the admin side configures company name / logo / theme accent color /
> login-page copy / splash screen (optional) → the client automatically fetches and applies after login
> (window title / login window / sidebar / window icon all switch accordingly, local cache + version
> number, changes take effect live, cache is used offline); when nothing is configured = Memex's own mark
> is the fallback, and behavior is unchanged.

## 0. Scope Ruling

- **Storage**: the server DB uses a single-row aggregate table `branding` (id=1), not the generic kv —
  branding is an aggregate (read the whole thing at once, a monotonically increasing version to detect
  changes).
- **The read surface is auth-free**: brand assets are not sensitive (company name/logo/copy); the login
  window must show them before auth — all three GET routes are auth-free; the client fetch does not
  depend on file-surface login (pulls directly via QNetworkAccessManager; offline / file surface not
  enabled = silently use the cache or the default mark).
- **The write surface is authorized**: az new rule `branding-manage` (ExplicitAllow = effective_roles
  contains org-admin; unmatched default-deny). Admin writes go through a FilesClient authed session (the
  client brand settings page); the ops fallback goes through the CLI (writes the store directly).
- **Theme accent selection**: the brand accent is a **local brand color** (sidebar brand row /
  login-window logo backdrop / login button), **it does not enter the R19 theme token system** —
  ThemeTokens.brand is part of the user's theme choice; server branding should not override the user's
  light/dark theme preference; the two color sets each govern their own surface.
- **Current avatar-placeholder status, honestly noted**: the client currently has no standalone avatar
  component (bubbles/lists do not display avatars) — the brand avatar placeholder color lands on the
  sidebar brand row's logo backdrop and the login window's logo backdrop; when an avatar component is
  built later, take the color from BrandKit.accent().

## 1. Storage (single-row aggregate table in the store)

```sql
CREATE TABLE IF NOT EXISTS branding (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  company_name TEXT NOT NULL DEFAULT '',
  accent       TEXT NOT NULL DEFAULT '',   -- '#rrggbb'，空=未配
  slogan       TEXT NOT NULL DEFAULT '',   -- 登录页文案，空=未配
  logo         BLOB,                       -- PNG 字节，NULL=未配
  splash       BLOB,                       -- PNG 字节，NULL=未配（可选件）
  version      INTEGER NOT NULL DEFAULT 0, -- 每次写 +1（变更判据）
  updated_ms   INTEGER NOT NULL DEFAULT 0
);
```

Methods: `branding_get()` (no row returns the all-empty default version 0), `branding_set(name,
accent, slogan, ts_ms)` (three text items, version+1), `branding_set_logo(bytes, ts_ms)` /
`branding_set_splash(bytes, ts_ms)`, `branding_clear_logo()` / `branding_clear_splash()` (every write
version+1; PNG validation is in the route layer, the store only keeps the bytes).

## 2. Routes (FileServer six: three auth-free reads + three authorized writes; implementation backfill)

- `GET /files/branding` (**auth-free**): `{company_name, accent, slogan, version, has_logo,
  has_splash}` — no bytes; the client compares version to decide whether to pull PNGs again.
- `GET /files/branding/logo` / `GET /files/branding/splash` (**auth-free**): `image/png` bytes; 404
  when unconfigured (the client falls back to the default mark).
- `POST /files/branding` (`branding-manage`): JSON `{company_name?, accent?, slogan?, clear_logo?,
  clear_splash?}` — text fields absent = untouched, explicit empty string = clear; the accent shape gate
  is `#rrggbb` (bad shape → 400); clear_* booleans (non-boolean → 400) = asset removal, each removal is
  an independent action with version+1 (the original spec's "no clear route on the HTTP surface,
  clearing goes through the CLI" is superseded by this field — needed for the brand settings page's
  「清除」 button). Empty object (no writable branded field) → 400. After writing, the new version is
  returned in the response.
- Route awareness of body_cap: brand-asset routes allow 512KiB+64KiB of headroom (the whole body must
  be read before a semantic 413 can be returned; absurd out-of-range large bodies still hit the 64KiB
  general cap layer).
- `POST /files/branding/logo` / `POST /files/branding/splash` (same authorization): body = raw PNG
  bytes. Validation: **PNG magic** (the 8 bytes `89 50 4E 47 0D 0A 1A 0A`) fails → 415; **size
  ≤512KiB**, over → 413; **IHDR dimensions ≤2048×2048** (offsets 16..24, width and height each 4 bytes
  big-endian) over → 413 (the copy distinguishes 「尺寸超限」). Valid = persisted with version+1,
  returns `{ok, version}`.

## 3. CLI (ops fallback; writes the store directly, not via HTTP)

```
memex_server branding show   --db PATH
memex_server branding set    --db PATH [--company NAME] [--accent '#rrggbb']
                             [--slogan TEXT] [--logo FILE.png] [--splash FILE.png]
                             [--clear-logo] [--clear-splash]
```

set only touches the given items (absent items stay untouched); logo/splash take file paths (same PNG
validation as the route layer; a failed validation on the CLI side exits non-zero and states the
reason).

## 4. Client (BrandKit + brand settings page)

**BrandKit** (client/app/brand_kit.{hpp,cpp}, application-level singleton):

- State: `company_name / accent(QColor, invalid = unconfigured) / slogan / logo(QPixmap) /
  splash(QPixmap) / version`, initially all empty = the Memex default mark.
- Fetch: `fetch(host, files_port)` — NAM GETs `/files/branding` (auth-free); if the version differs from
  local (or locally there is no logo but has_logo) then GETs the PNGs; on success immediately
  `apply_all()` + persists the cache. Failure is silent (file surface not enabled / offline = stay
  as-is).
- Cache (implementation backfill): this Qt version has no `AppCacheLocation` enum — lands under
  `QStandardPaths::AppDataLocation/brand/` as `brand.json` + `logo.png` + `splash.png`
  (`MEMEX_TEST_BRAND_DIR` env var as the test override seam, following the emoji cache precedent); on
  startup `load_cache()` runs first (branded even offline), and a successful fetch overwrites it
  (changes take effect live).
- Application surface (implementation backfill): not the `apply_window/apply_login_dialog`
  explicit-call form; instead it is **brand_applied signal-driven** — the main window connects this
  signal at the end of its construction to `apply_brand()` (windowTitle = company name, otherwise
  "Memex"; windowIcon and tray = logo non-empty QIcon, otherwise the qrc multi-size default mark +
  programmatically drawn fallback; sidebar brand row shown/hidden — no brand = hidden = unconfigured
  means no change); the login window is a rewritten implementation (top brand area logo 40×40 + company
  name bold + slogan, hidden when there is no brand; the title follows the company name).
- Wiring (implementation backfill): the fetch timing is earlier than the original spec — **immediately**
  `fetch(host, files_port)` after the login window accepts (GET is auth-free = usable before auth, not
  waiting for logged_in; the difference: the brand can still be fetched when the credentials are wrong,
  which is a natural consequence of the auth-free convention). The file port takes QSettings
  `collab/files_port` (the login window gains a 「文件口」 row, default 24362, listed separately from
  the collaboration port); a BrandKit fetch failure does not affect the login flow (silently stays
  as-is).

**Brand settings page** (client/app/brand_settings_page.{hpp,cpp}, standalone QDialog, isomorphic to
the existing dialog pattern): a connection area (host/file port/account/password) → FilesClient login;
a display area (current company name/accent/slogan/has-logo/has-splash/version, echoed after
connecting); an editing area: company name row (empty = clear) / accent (hex input + QColorDialog color
picker; bad shape rejected locally and not sent to the network) / login-page copy row / logo file
selection (QFileDialog + QLabel live preview; if QPixmap cannot open it, rejected locally) / same for
splash (optional) + 「清除 logo」「清除 splash」; 「保存文本」 = POST /files/branding, 「上传素材」 =
(once a file is chosen) POST logo/splash. The 403 status line states 「品牌设置归 org-admin」 (the
unified failure channel passes through the server's semantic 413/415 copy). The main window's 「设置」
menu gains 「品牌物料…」 (QPointer lazy construction with WA_DeleteOnClose; the original spec said the
"Options" menu, but the actual menu name is 「设置」 — the shipped behavior prevails). After a
save/clear/upload succeeds, a local BrandKit fetch is triggered (settings take effect immediately =
this machine sees them first).

## 5. Asset Specifications (upload validation and design suggestions)

| Item | Format | Suggested size | Limit | Notes |
|---|---|---|---|---|
| logo | PNG (transparent background) | 512×512 square | 512KiB, ≤2048² | Shared by window icon / tray / sidebar / login window; the client scales it per size automatically (QIcon multi-size interpolation); non-square is usable but the corner-radius crop uses the short side |
| splash | PNG | within 1024×768 | 512KiB, ≤2048² | Optional; splash page / login window banner, the client scales proportionally |
| accent | `#rrggbb` | — | — | Recommended to match the logo's primary color; light/dark themes compute their own brand_text brightening/darkening (local application does not auto-transform today; verify contrast when choosing) |
| company_name | Text | ≤32 字 | — | Window title / sidebar / login window |
| slogan | Text | ≤64 字 | — | Login-page copy row |

## 6. Test Conventions

- server `test_files_api` extends with a "品牌物料" section: auth-free GET default all-empty version 0
  / POST unauthenticated 401, non-org-admin 403, org-admin 200 and version+1 / bad accent shape 400 /
  non-PNG logo 415, over 512KiB 413, valid 200 then an auth-free GET logo returns the same bytes +
  has_logo=true / same for splash + 404 after clear / version monotonic.
- CLI leg: after `branding set --company/--logo`, show echoes it and the direct store query matches;
  setting a non-PNG file exits non-zero.
- client `test_brand_kit` (real server × offscreen, ctest leg 58, shipped): default mark (no brand =
  Memex title/icon always present, logo empty) → CLI configures the brand → fetch rebrands + cache
  persisted → offline (new BrandKit instance load_cache) still rebranded → bad port fetch stays silent
  → CLI clears the brand → fetch returns to the default mark.
- client `test_brand_settings` settings-page smoke test (ctest leg 59, shipped): rejected locally when
  not connected → wrong-password status line states it → member's save gets the 403 ownership copy (and
  does not rebrand) → org-admin's save gets 200 + the local BrandKit rebrands immediately → bad accent
  shape rejected locally and not sent to the network → logo selection preview + upload (rejected
  locally if non-PNG) → clearing the logo falls back.
- Screenshot acceptance (shipped, programmatic QWidget::grab written to docs/src/public/branding/, plus
  manual walkthrough): before/after frames — after the settings page configures the brand, another
  client (an independent MainWindow instance) loads from cache = before; after the CLI configures a new
  brand, a connected client fetches and it takes effect live = after; the difference between the two =
  the sidebar brand row (company name + logo backdrop) follows the brand change.

## 7. Not Done (honestly noted)

- No multi-brand tenants (the single-row table = one brand set for the whole server).
- Brand changes are not actively pushed to online clients (fetch + version comparison; online clients
  take effect live at their next login / when the settings page opens — "changes take effect live" is
  defined by the fetch timing).
- No full-screen splash display on startup (the splash asset is currently used as the login window
  banner; a full-screen startup page belongs to desktop integration, to be filed separately).
- The avatar-placeholder surface (§0) and the global theme-token replacement (§0 selection) are not
  part of this item.
