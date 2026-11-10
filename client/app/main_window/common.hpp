// main_window 拆分模块的内部共享小工具；对外 API 仍只在 main_window.hpp
//（LAY-002：界面行控件/气泡 HTML 构造器、盯键、日期锚点、品牌图标等
//  原 main_window.cpp 文件头匿名命名空间 helper 区的共享声明）。
#pragma once

#include <QColor>
#include <QDate>
#include <QIcon>
#include <QString>
#include <QWidget>
// 多数模块共用的头统一上收到此（LAY-002）
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QPixmap>
#include <QSettings>
#include <QSize>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <engine/collab/files_client.hpp>
#include <nlohmann/json.hpp>

#include "theme.hpp"
#include "avatar_store.hpp"
#include "emoji_pack_dialog.hpp"
#include "group_vault_dialog.hpp"
#include "mailbox_view.hpp"
#include "net_guard.hpp"
#include "rail_nav.hpp"

namespace memex::client {

// 当前令牌（界面颜色唯一来源，R19 · T4.9）——不再散落十六进制字面量
inline const ThemeTokens& tk() { return ThemeManager::instance().tokens(); }

// 新消息闪烁的合并窗：窗内后续消息不叠加（同会话/跨会话短时间多条
// 合并成一次闪烁）；窗口激活时 Qt 自行取消当前闪烁，窗状态另由触发点
// 的 isActiveWindow 判定管
constexpr int kFlashAlertMs = 2000;

// 最近联系时刻的行内时间位（原型会话行右端 10:24／昨天）：今日 HH:mm，
// 昨日「昨天」，更早 MM-dd；无记录（0）返回空。
QString fmt_last_contact(qint64 ms);

// 品牌图标：logo 本体（docs/src/public/logo.svg → 多尺寸 PNG 进 qrc，
// 生成链见 client/app/icons/README.md）优先；资源缺失（裁剪构建）回落
// 程序绘制示意（品牌橙圆角底＋白色 M）。
// 窗口/任务栏/托盘/关于页共用同源。
QIcon brand_icon();

QString esc(const QString& s);

QString hhmm(qint64 ts_ms);

// 需求批①文字颜色：受控标记〔#RRGGBB〕…〔/〕→ 颜色 span。线上与存储仍是
// 纯文本（协议/归档无感）；渲染侧先 esc 后替换＝零注入面，色值严格
// [0-9A-Fa-f]{6} 才生效，不成对/坏色保持字面显示（无害降级）。
QString colorize(const QString& content);

// 气泡行：外出＝品牌橙靠右，来访＝浅灰靠左（QTextBrowser 富文本子集，不用圆角）。
// at_mode=true（群聊）：@账号 标记整体包品牌橙加粗。
// receipt 非空＝我发出消息的回执勾（需求批⑦；已读灰勾单 2026-10-14）：
// delivered＝单勾 ✓、read＝双勾 ✓✓，两态同为灰色（继承 meta 的 text_muted
// 令牌，浅深双主题各自可读）——「送达 vs 已读」靠勾数区分，不另造图标语言、
// 不挂散文标签（沿用微信／QQ／Telegram 同一套勾）。挂在 meta 行尾，历史
// 重载与实时更新同构。
// watch_key 非空＝可盯行（我发出的文本消息，「盯」单 2026-10-14）：meta
// 行与正文包进行锚（右键据此定位行键；openLinks 已关＝左键无导航副作用），
// watched=true 再在 meta 尾追加灰 ◉（与 ✓/✓✓ 同行同色的小眼睛语义）。
// 「盯」行键 ↔ 气泡锚链接：键百分号编码进自定义 scheme（openLinks 已关，
// 左键无导航）；右键经 anchorAt 反解回行键。两域键：
//   直连 d:<收件人>:<发送序号>（无 msg_id，与回执同口径）
//   协作 c:<msg_id>
constexpr const char* kWatchScheme = "memexwatch:";
// 「盯」节奏（实现单修订 2026-10-14：未读超阈值才提醒，已读只静默解除
// ——已读通知不是盯）：阈值默认 10 分钟、超时解默认 24h，均 QSettings
// 可配（watch/remind_minutes、watch/expire_hours），每次 sweep 现读＝
// 改配置即时生效不需重启。巡检 30s 一轮（分钟级语义，秒级精度即可）。
qint64 watch_remind_ms();
qint64 watch_expire_ms();
QString watch_href(const QString& key);
QString watch_key_from_href(const QString& href);
QString direct_watch_key(const QString& peer, quint64 seq);
QString collab_watch_key(const QString& msg_id);

// —— 原型桌面会话面（desktop-chat.html）行控件化 ——
// 头像八色（prototypes.css .avatar.v2–v9）：按显示名稳定取色（同人同色，
// 跨主题恒同——人色不随浅深主题漂移）。首字头像 28px（.avatar.sm 近似，
// 富文本以嵌套表格＋cellpadding 实现）。
QColor avatar_hue(const QString& name);
QString avatar_initial(const QString& name);

// —— 原型协作列表行（desktop-collab.html .row）控件化 ——
// 头像（40px 圆角 12、八色同源）＋在离线点（.presence 11px 描边侧栏底）
// ＋主列（行首＝名＋右端时间；末条预览）＋右徽标（.n-count 未读数品牌
// 胶囊）。presence：1 在线／0 离线／-1 无（群行）。WA_TransparentForMouse
// Events 让点击/悬停穿透回列表——选中、悬浮与条目悬浮提示仍走
// QListWidget 原生路径（UserRole 数据、itemClicked 不受行控件影响）。
QWidget* make_session_row(const ThemeTokens& t, const QString& avatar_name,
                          int presence, const QString& name,
                          const QString& time, const QString& preview,
                          int unread, const QString& badge = {});
QString avatar_html(const QString& name);
// 文件卡图标字（.bubble.file-card .f-icon）：按扩展名映射一字
QString file_icon_char(const QString& name);
QString human_size(qint64 bytes);

QString bubble_html(const QString& name, const QString& text, qint64 ts_ms,
                    bool outgoing, bool at_mode = false,
                    const QString& image = {},
                    const QString& receipt = {},
                    const QString& watch_key = {},
                    bool watched = false,
                    bool owner = false);

// 文件卡气泡（原型 .bubble.file-card：38px 图标格＋名称 700＋11px 元数据；
// 收发两向同款，仅左右与气泡底色随 outgoing 翻转）
QString file_bubble_html(const QString& name, const QString& file_name,
                         const QString& file_meta, qint64 ts_ms,
                         bool outgoing);

// 居中系统行（建立会话提示／跨态说明／文件进度）：次要文本色。
// 正文不做转义——调用方按既有口径自行 esc（与改版前逐字一致）。
QString system_line_html(const QString& text);

// 胶囊系统行（原型 .sys-hint .pill／.recalled .pill：panel-2 底＋细边，
// 富文本以单元格底色近似圆角胶囊）
QString chip_line_html(const QString& text);

// —— 需求批⑧ 聊天记录按日期 ——
// 日期锚点：消息行按本机日期带命名锚，跳转用 scrollToAnchor 定位。
// 系统行无时刻不锚——目标日以系统行开头时定位到其后最近的消息行。
QString day_anchor(qint64 ts_ms);
// 当日窗（毫秒含端点）：按本机时区取 [00:00:00.000, 23:59:59.999]。
qint64 day_start_ms(const QDate& d);
qint64 day_end_ms(const QDate& d);
// 筛选头的时间端展示（0=不限侧以「…」示）
QString day_label(qint64 ms);
// 分日分隔线（需求批⑧余量收口）：居中日头；近两日用「今天／昨天」口吻
//（原型 .day-divider「今天 13:38」），更早沿用 yyyy-MM-dd 全量口径。
QString day_divider_html(const QDate& d);
// 需求批⑩ 在线时长秒数展示：服务端按秒计（滚动窗并集），四舍五入到
// 分钟级可读量——秒级展示太抖（ORG_QUERY 每次现算都在涨）。
QString format_online_s(qint64 seconds);

} // namespace memex::client
