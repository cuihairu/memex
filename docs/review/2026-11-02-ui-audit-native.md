# UI Audit (impeccable 流程) — 原生桌面客户端 · 2026-11-02

- 范围：**原生 C++/Qt6 桌面客户端**（此前 web 界面与静态原型已审，原生实机界面未审）。
  在 Windows Server 2022 实机（openclaw，10.240.0.3）跑真实二进制按 impeccable 维度审核：
  层级 / 排版 / 对比度 a11y（WCAG 1.4.3 / 1.4.11）/ 布局 / 边界态。
- 方法：schtasks 交互会话启动真实 `memex-client`；PrintWindow / `grab()` 抓真机窗口；
  亮/暗双主题（注册表 `HKCU:\Software\memex\Memex\appearance\theme_mode` 切换，抓完还原）；
  边界态覆盖 **未发现设备 · 未登录 · 320px 窄窗 · 亮/暗**。
  对比度按 ThemeTokens 设计值精确计算，并以真机截图像素采样交叉验证（title 实测 12.14:1
  与 token 计算完全一致，方法互证）。
- 截图证据：`docs/review/ui-audit-native-2026-11-02/`（取证时点为修复前）。

## 截图证据

| 视图 | 文件 | 说明 |
|---|---|---|
| 全桌面首启（亮） | `win-initial-light.png` 2560×1440 | 客户端窗＋启动宿主控制台＋悬浮提示 |
| 主窗（带 OS 边框，未登录） | `win-memex-login.png` 336×839 | 标题「Memex」，未发现设备/未登录态 |
| 主窗抓帧 · 亮 · 未登录 | `state-light.png` 320×749 | 空设备列表＋登录协作态卡 |
| 主窗抓帧 · 暗 · 未登录 | `state-dark.png` 320×749 | 同上暗主题 |

## 整体 critique

原生客户端与静态原型同一设计语言（品牌橙＋暖米白、60px rail＋侧栏骨架），实机渲染
质量与原型一致：**层级、排版、布局、边界态四个维度没有发现坏损**——

- **层级**：菜单栏 → 设备区标题（局域网设备＋「未发现设备」标签右对齐）→ 搜索框 →
  自动发现说明行 → 登录协作态卡（主行动面）→ 底部状态栏，信息密度与留白节奏正确。
- **排版**：320px 窄窗下标题/说明/按钮/状态栏全部无裁切、无溢出、无换行破相；
  状态栏「直连态 · 运行中（未发现设备）｜协作态 · 未登录」完整放下。
- **布局**：rail 图标上下分组（功能组＋底部主题/头像徽标）位置稳定，亮暗两态几何一致。
- **边界态**：空设备列表（列表区留白＋「未发现设备」标签）、未登录（登录协作态卡＋
  状态栏明示）、双主题渲染完整，均正确呈现。

坏损集中在**对比度 a11y**，且病灶与原型审计同构——都在令牌层，修复是令牌级的：

## 问题清单（按严重度）

| # | 严重度 | 问题 | 定位 | 处置 |
|---|---|---|---|---|
| 1 | **高** | 亮色 `text_muted` #7d7169：surface 4.43 / surface_alt 4.14 / brand_tint **3.81**——自动发现说明、卡片副文案等正文级次要文字不达 AA 4.5:1 | `client/app/theme.cpp` 亮色令牌 | **已修**（0e4c782：#6e6256，全部承载面 ≥4.76；test_theme 断言 3.0→4.5） |
| 2 | 中 | 亮色 `brand_text` #a05a26 压 `brand_tint`（选中行/徽标底）= **4.24:1**——rail 激活图标、选中行品牌文字、本机保存徽标不达 AA（其余承载面 4.61–5.27 达标，唯选中行失守） | `client/app/theme.cpp` 亮色令牌（默认＋自定义生成器同值） | **已修**（b286843：#9c5220，最差面 brand_tint 4.64；断言同步收紧 4.5） |
| 3 | 低 | 白字压品牌橙（`on_brand`/`brand`）= **3.44:1**：登录按钮、主行动面、己方气泡。达 WCAG 1.4.11 非文本（≥3:1）与 AA 大字（≥3:1），**不达 AA 正文 4.5:1** | `theme.cpp` `kBrandOrange` #e16531 | **记录不改**（品牌识别色；改填充即换品牌。见「建议」） |
| 4 | 信息 | 首启桌面截图中的深蓝控制台窗为 `cmd.exe` 宿主（标题栏可辨，标签「管理员: …memex(admin)」），是**部署引导脚本**，非客户端本体 UI | 部署侧 | 非坏损，记录归属 |
| 5 | 信息 | 暗色主题全套令牌实测全过（muted 5.87–6.30、brand_text 6.54–8.01），无需改动 | `theme.cpp` 暗色令牌 | 通过 |

## 对比度实测（WCAG 1.4.3）

| 令牌 | 承载面 | 修复前 | 修复后 | WCAG |
|---|---|---|---|---|
| `text_muted` 亮 | surface | 4.43 ✗ | **5.55** ✓ | AA |
| `text_muted` 亮 | surface_alt | 4.14 ✗ | **5.18** ✓ | AA |
| `text_muted` 亮 | surface_raised | 4.73 ✗ | **5.92** ✓ | AA |
| `text_muted` 亮 | brand_tint（选中行） | 3.81 ✗ | **4.76** ✓ | AA |
| `text_muted` 暗 | surface_alt / surface | 5.87 / 6.30 | （未改）✓ | AA |
| `brand_text` 亮 | brand_tint（选中行） | 4.24 ✗ | **4.64** ✓ | AA |
| `brand_text` 亮 | surface / wash / raised | 4.93–5.27 ✓ | **5.08–5.77** ✓ | AA |
| `brand_text` 暗 | brand_tint / wash | 6.54 / 7.23 | （未改）✓ | AA |
| `on_brand` 白 / `brand` | 登录按钮/主行动面 | 3.44 | （未改） | AA-large ✓ / 正文 ✗ |
| 正文 `text` 亮 | surface | 12.14（像素实测=token 计算） | — | AAA ✓ |

## 测试门禁

- `client/tests/test_theme.cpp` 断言收紧并随修复提交：`text_muted` 对 `surface_alt`
  **与 `brand_tint`** 亮暗各断 ≥4.5（新增 brand_tint 腿）；`brand_text` 对 `brand_tint`
  3.0→**4.5**（`check_new_token_contrast` 亮暗双套共用）。
- 自定义主题生成器复核：9 套亮色预设 muted（`hsl(h,0.09,0.38)`）最差 5.36:1、
  brand_text（已同步 #9c5220）≥4.64:1，全部达标，生成器公式未改。
- `ctest` 串行复验：theme / checkbox_glyph / settings_window / mode_switch 全绿；
  config-effect 截图产物与已提交版本像素一致（AE=0），GIF 无需重生成。

## 建议（不阻塞）

- #3 若日后要全 AA：主行动面按钮文字改粗体/大字号（14pt bold 即达 AA-large 口径），
  或填充改深一档橙（需品牌侧决策，`brand_hover` #c9541f 4.40:1 仍不足 4.5，需更深）。
  当前白字压橙为全行业品牌按钮惯例，维持现状记录在案。

## 就地修复 commits

- `fix: raise light theme muted text to wcag aa contrast`（0e4c782，#1）
- `fix: darken brand text for wcag aa on selected rows`（b286843，#2）
