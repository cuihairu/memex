#include "main_window.hpp"

#include "rail_nav.hpp"
#include "collab_defaults.hpp"

#include <QAction>
#include <QCloseEvent>
#include <QApplication>
#include <QColorDialog>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDateEdit>
#include <QDateTimeEdit>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGridLayout>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QTextEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenuBar>
#include <QMenu>
#include <QNetworkInterface>
#include <QProgressBar>
#include <QScrollArea>

#include "file_assistant.hpp"
#include "emoji_pack_dialog.hpp"
#include "avatar_store.hpp"
#include "crop_dialog.hpp"
#include <engine/collab/files_client.hpp>
#include "group_memo_dialog.hpp"
#include "group_vault_dialog.hpp"
#include "group_ci_dialog.hpp"
#include "group_pack_dialog.hpp"
#include "group_server_dialog.hpp"
#include "task_dialog.hpp"
#include "approval_dialog.hpp"
#include "report_dialog.hpp"
#include "group_tools_dialog.hpp"
#include "brand_settings_page.hpp"
#include "shortcut_settings.hpp"
#include "away_settings.hpp"
#include "net_remote_settings.hpp"
#include "net_guard.hpp"
#include "assist_dialog.hpp"
#include "audit_dialog.hpp"
#include "archive_console.hpp"
#include "office_map_dialog.hpp"
#include "message_search_dialog.hpp"
#include <QMessageBox>
#include <QBuffer>
#include <QPainter>
#include <QPixmap>
#include <QRegularExpression>
#include <QTimer>
#include <QWindow>
#include <QScrollBar>
#include <QSettings>
#include <QSize>
#include <QShortcut>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTextStream>
#include <QTextCursor>
#include <QTextImageFormat>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <map>
#include <utility>

#include <nlohmann/json.hpp>

#include <core/local_store.hpp>

#include "notify_center.hpp"
#include "notify_prefs.hpp"
#include "brand_kit.hpp"

#ifndef MEMEX_VERSION // 测试目标未传版本定义时兜底（与 main.cpp 同款）
#define MEMEX_VERSION "dev"
#endif

#include "theme.hpp"
#include "theme_settings_page.hpp"

namespace memex::client {

namespace {

// 当前令牌（界面颜色唯一来源，R19 · T4.9）——不再散落十六进制字面量
const ThemeTokens& tk() { return ThemeManager::instance().tokens(); }

// 新消息闪烁的合并窗：窗内后续消息不叠加（同会话/跨会话短时间多条
// 合并成一次闪烁）；窗口激活时 Qt 自行取消当前闪烁，窗状态另由触发点
// 的 isActiveWindow 判定管
constexpr int kFlashAlertMs = 2000;

// 最近联系时刻的行内时间位（原型会话行右端 10:24／昨天）：今日 HH:mm，
// 昨日「昨天」，更早 MM-dd；无记录（0）返回空。
QString fmt_last_contact(qint64 ms) {
  if (ms <= 0) return QString();
  const QDateTime dt = QDateTime::fromMSecsSinceEpoch(ms);
  const QDate day = dt.date();
  const QDate today = QDate::currentDate();
  if (day == today) return dt.toString(QStringLiteral("HH:mm"));
  if (day == today.addDays(-1)) return QStringLiteral("昨天");
  return dt.toString(QStringLiteral("MM-dd"));
}

} // namespace

namespace {
// 品牌图标：logo 本体（docs/src/public/logo.svg → 多尺寸 PNG 进 qrc，
// 生成链见 client/app/icons/README.md）优先；资源缺失（裁剪构建）回落
// 程序绘制示意（品牌橙圆角底＋白色 M）。
// 窗口/任务栏/托盘/关于页共用同源。
QIcon brand_icon() {
  QIcon icon;
  for (int s : {16, 32, 48, 64, 128, 256}) {
    QPixmap pm(QStringLiteral(":/icons/memex-%1.png").arg(s));
    if (!pm.isNull()) icon.addPixmap(pm);
  }
  if (!icon.availableSizes().isEmpty()) return icon;
  QPixmap pm(64, 64);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setBrush(QColor(QStringLiteral("#e16531")));
  p.setPen(Qt::NoPen);
  p.drawRoundedRect(pm.rect().adjusted(2, 2, -2, -2), 14, 14);
  p.setPen(Qt::white);
  QFont f = p.font();
  f.setPixelSize(38);
  f.setBold(true);
  p.setFont(f);
  p.drawText(pm.rect(), Qt::AlignCenter, QStringLiteral("M"));
  return QIcon(pm);
}
} // namespace

namespace {

QString esc(const QString& s) {
  QString out = s;
  out.replace(QChar('&'), QStringLiteral("&amp;"));
  out.replace(QChar('<'), QStringLiteral("&lt;"));
  out.replace(QChar('>'), QStringLiteral("&gt;"));
  out.replace(QChar('"'), QStringLiteral("&quot;"));
  return out;
}

QString hhmm(qint64 ts_ms) {
  return QDateTime::fromMSecsSinceEpoch(ts_ms).toString(QStringLiteral("HH:mm"));
}

// 需求批①文字颜色：受控标记〔#RRGGBB〕…〔/〕→ 颜色 span。线上与存储仍是
// 纯文本（协议/归档无感）；渲染侧先 esc 后替换＝零注入面，色值严格
// [0-9A-Fa-f]{6} 才生效，不成对/坏色保持字面显示（无害降级）。
QString colorize(const QString& content) {
  static const QRegularExpression re(
      QStringLiteral("〔#([0-9A-Fa-f]{6})〕(.*?)〔/〕"));
  QString out;
  qsizetype pos = 0;
  auto it = re.globalMatch(content);
  while (it.hasNext()) {
    const auto m = it.next();
    out += content.mid(pos, m.capturedStart() - pos);
    out += QStringLiteral("<span style=\"color:#%1;\">%2</span>")
               .arg(m.captured(1), m.captured(2));
    pos = m.capturedEnd();
  }
  out += content.mid(pos);
  return out;
}

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
qint64 watch_remind_ms() {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  const int minutes =
      settings.value(QStringLiteral("watch/remind_minutes"), 10).toInt();
  return qint64(minutes > 0 ? minutes : 10) * 60 * 1000;
}
qint64 watch_expire_ms() {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  const int hours =
      settings.value(QStringLiteral("watch/expire_hours"), 24).toInt();
  return qint64(hours > 0 ? hours : 24) * 60 * 60 * 1000;
}
QString watch_href(const QString& key) {
  return QLatin1String(kWatchScheme) +
         QString::fromUtf8(QUrl::toPercentEncoding(key));
}
QString watch_key_from_href(const QString& href) {
  const int prefix = int(QLatin1String(kWatchScheme).size());
  if (!href.startsWith(QLatin1String(kWatchScheme))) return QString();
  return QUrl::fromPercentEncoding(href.mid(prefix).toUtf8());
}
QString direct_watch_key(const QString& peer, quint64 seq) {
  return QStringLiteral("d:%1:%2").arg(peer, QString::number(seq));
}
QString collab_watch_key(const QString& msg_id) {
  return QStringLiteral("c:%1").arg(msg_id);
}

// —— 原型桌面会话面（desktop-chat.html）行控件化 ——
// 头像八色（prototypes.css .avatar.v2–v9）：按显示名稳定取色（同人同色，
// 跨主题恒同——人色不随浅深主题漂移）。首字头像 28px（.avatar.sm 近似，
// 富文本以嵌套表格＋cellpadding 实现）。
QColor avatar_hue(const QString& name) {
  static const QColor kHues[] = {
      QColor(0x7c, 0x5c, 0xbf), QColor(0x2e, 0x8b, 0x8b),
      QColor(0xb7, 0x79, 0x1f), QColor(0x5b, 0x7f, 0xbf),
      QColor(0xbf, 0x5b, 0x82), QColor(0x4f, 0x8a, 0x4a),
      QColor(0x8a, 0x6d, 0x4f), QColor(0x5f, 0x6b, 0x7a)};
  quint32 h = 2166136261u; // FNV-1a：对 QString 逐 UTF-16 码位
  for (const QChar c : name) h = (h ^ c.unicode()) * 16777619u;
  return kHues[h % 8];
}
QString avatar_initial(const QString& name) {
  const QString n = name.trimmed();
  return n.isEmpty() ? QStringLiteral("?")
                     : QString(n.at(0)).toUpper();
}

// 组织架构成员卡点击选中（事件过滤：QWidget 无 clicked 信号，鼠标释放
// 即触发；随卡片父子链销毁）
class OrgCardClick : public QObject {
 public:
  explicit OrgCardClick(std::function<void()> fn, QObject* parent)
      : QObject(parent), fn_(std::move(fn)) {}
  bool eventFilter(QObject*, QEvent* e) override {
    if (e->type() == QEvent::MouseButtonRelease) {
      fn_();
      return true;
    }
    return false;
  }

 private:
  std::function<void()> fn_;
};

// —— 原型协作列表行（desktop-collab.html .row）控件化 ——
// 头像（40px 圆角 12、八色同源）＋在离线点（.presence 11px 描边侧栏底）
// ＋主列（行首＝名＋右端时间；末条预览）＋右徽标（.n-count 未读数品牌
// 胶囊）。presence：1 在线／0 离线／-1 无（群行）。WA_TransparentForMouse
// Events 让点击/悬停穿透回列表——选中、悬浮与条目悬浮提示仍走
// QListWidget 原生路径（UserRole 数据、itemClicked 不受行控件影响）。
QWidget* make_session_row(const ThemeTokens& t, const QString& avatar_name,
                          int presence, const QString& name,
                          const QString& time, const QString& preview,
                          int unread, const QString& badge = {}) {
  auto* row = new QWidget;
  row->setAttribute(Qt::WA_TransparentForMouseEvents);
  auto* lay = new QHBoxLayout(row);
  lay->setContentsMargins(10, 8, 10, 8);
  lay->setSpacing(10);

  auto* avatar = new QLabel(avatar_initial(avatar_name), row);
  avatar->setFixedSize(40, 40);
  avatar->setAlignment(Qt::AlignCenter);
  avatar->setStyleSheet(QStringLiteral(
                           "background:%1; color:%2; font-weight:700; "
                           "font-size:15px; border-radius:12px;")
                           .arg(avatar_hue(avatar_name).name(),
                                t.on_brand.name()));
  if (presence >= 0) {
    auto* dot = new QLabel(avatar);
    dot->setFixedSize(11, 11);
    dot->move(29, 29);
    dot->setStyleSheet(
        QStringLiteral("border-radius:5px; background:%1; border:2px solid %2;")
            .arg(presence ? t.success.name() : t.text_muted.name(),
                 t.surface_alt.name()));
  }
  lay->addWidget(avatar, 0, Qt::AlignTop);

  // 名与预览按侧栏余宽省略号截断（QLabel 不自省略，构建时裁）
  const QFontMetrics fm(avatar->font());
  auto* main = new QWidget(row);
  auto* mlay = new QVBoxLayout(main);
  mlay->setContentsMargins(0, 0, 0, 0);
  mlay->setSpacing(2);
  auto* top = new QHBoxLayout();
  top->setSpacing(8);
  auto* name_l =
      new QLabel(fm.elidedText(name, Qt::ElideRight, 150), main);
  name_l->setStyleSheet(QStringLiteral("font-weight:600; font-size:13px; "
                                       "color:%1;")
                            .arg(t.text.name()));
  top->addWidget(name_l, 1);
  if (!time.isEmpty()) {
    auto* time_l = new QLabel(time, main);
    time_l->setStyleSheet(
        QStringLiteral("font-size:11px; color:%1;").arg(t.text_muted.name()));
    top->addWidget(time_l, 0, Qt::AlignRight);
  }
  mlay->addLayout(top);
  auto* sub =
      new QLabel(fm.elidedText(preview, Qt::ElideRight, 170), main);
  sub->setStyleSheet(
      QStringLiteral("font-size:12px; color:%1;").arg(t.text_muted.name()));
  mlay->addWidget(sub);
  lay->addWidget(main, 1);

  if (unread > 0) {
    auto* pill = new QLabel(QString::number(unread), row);
    pill->setAlignment(Qt::AlignCenter);
    pill->setMinimumWidth(17);
    pill->setFixedHeight(17);
    pill->setStyleSheet(
        QStringLiteral("background:%1; color:%2; font-size:11px; "
                       "font-weight:700; border-radius:8px; padding:0 5px;")
            .arg(t.brand.name(), t.on_brand.name()));
    lay->addWidget(pill, 0, Qt::AlignRight | Qt::AlignVCenter);
  }
  // 尾部描边胶囊（原型 .r-badges .noarch「未归档」）：警示色描边，如临群行
  if (!badge.isEmpty()) {
    auto* b = new QLabel(badge, row);
    b->setStyleSheet(
        QStringLiteral("color:%1; border:1px solid %1; border-radius:999px; "
                       "padding:1px 8px; font-size:10.5px;")
            .arg(t.warning.name()));
    lay->addWidget(b, 0, Qt::AlignRight | Qt::AlignVCenter);
  }
  return row;
}
QString avatar_html(const QString& name) {
  return QStringLiteral(
             "<table cellspacing=\"0\" cellpadding=\"7\"><tr><td "
             "bgcolor=\"%1\" align=\"center\"><span style=\"color:#ffffff; "
             "font-weight:700;\">%2</span></td></tr></table>")
      .arg(avatar_hue(name).name(), esc(avatar_initial(name)));
}
// 文件卡图标字（.bubble.file-card .f-icon）：按扩展名映射一字
QString file_icon_char(const QString& name) {
  const QString sfx = QFileInfo(name).suffix().toLower();
  if (sfx == QStringLiteral("xlsx") || sfx == QStringLiteral("xls") ||
      sfx == QStringLiteral("csv"))
    return QStringLiteral("表");
  if (sfx == QStringLiteral("zip") || sfx == QStringLiteral("rar") ||
      sfx == QStringLiteral("7z") || sfx == QStringLiteral("tar") ||
      sfx == QStringLiteral("gz"))
    return QStringLiteral("压");
  if (sfx == QStringLiteral("png") || sfx == QStringLiteral("jpg") ||
      sfx == QStringLiteral("jpeg") || sfx == QStringLiteral("gif"))
    return QStringLiteral("图");
  return QStringLiteral("文");
}
QString human_size(qint64 bytes) {
  if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
  if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg(bytes / 1024);
  if (bytes < 1024LL * 1024 * 1024)
    return QStringLiteral("%1 MB")
        .arg(QString::number(static_cast<double>(bytes) / 1048576.0, 'f', 1));
  return QStringLiteral("%1 GB")
      .arg(QString::number(static_cast<double>(bytes) / 1073741824.0, 'f', 2));
}

QString bubble_html(const QString& name, const QString& text, qint64 ts_ms,
                    bool outgoing, bool at_mode = false,
                    const QString& image = {},
                    const QString& receipt = {},
                    const QString& watch_key = {},
                    bool watched = false,
                    bool owner = false) {
  const ThemeTokens& t = tk();
  QString receipt_tag;
  if (outgoing && receipt == QStringLiteral("delivered")) {
    receipt_tag = QStringLiteral(" ✓");   // 已送达＝单勾
  } else if (outgoing && receipt == QStringLiteral("read")) {
    receipt_tag = QStringLiteral(" ✓✓");  // 已读＝双勾（灰勾）
  }
  if (watched) {
    receipt_tag += QStringLiteral(" ◉"); // 盯住＝小眼睛（text_muted 灰）
  }
  // 群主标签（原型 desktop-group.html .msg-meta .role：名后 10px 品牌色）
  const QString owner_html =
      owner ? QStringLiteral(" <span style=\"font-size:10px; color:%1;"
                             "font-weight:600;\">群主</span>")
                    .arg(t.brand_text.name())
              : QString();
  const QString meta_text = QStringLiteral("%1%2 %3%4")
                                .arg(esc(name), owner_html, hhmm(ts_ms),
                                     esc(receipt_tag));
  const QString meta =
      watch_key.isEmpty()
          ? QStringLiteral(
                "<span style=\"color:%1; font-size:small;\">%2</span>")
                .arg(t.text_muted.name(), meta_text)
          : QStringLiteral(
                "<a href=\"%1\"><span style=\"color:%2; text-decoration:none; "
                "font-size:small;\">%3</span></a>")
                .arg(esc(watch_href(watch_key)), t.text_muted.name(),
                     meta_text);
  // 图片消息（需求批④）：气泡内容＝内嵌图片（本机保留路径，file:// 直载）；
  // 普通消息照旧转义文本。
  QString content =
      image.isEmpty()
          ? colorize(esc(text))
          : QStringLiteral(
                "<img src=\"file://%1\" width=\"280\"><br>"
                "<span style=\"color:%2; font-size:small;\">图片消息 %3</span>")
                .arg(image, t.text_muted.name(), esc(QFileInfo(image).fileName()));
  if (at_mode && image.isEmpty()) {
    // @成员高亮（T4.1）：中文昵称同为账号字符——ASCII-only 正则曾漏配
    // 「@张伟」（2026-10-29 原型比对发现），补 CJK 基本区
    static const QRegularExpression at_re(
        QStringLiteral("@[A-Za-z0-9_.\\-\\x{4e00}-\\x{9fa5}]+"));
    QString highlighted;
    qsizetype pos = 0;
    auto it = at_re.globalMatch(content);
    while (it.hasNext()) {
      const auto m = it.next();
      highlighted += content.mid(pos, m.capturedStart() - pos);
      highlighted += QStringLiteral(
                         "<span style=\"color:%1; font-weight:600;\">%2</span>")
                         .arg(t.brand.name(), m.captured());
      pos = m.capturedEnd();
    }
    highlighted += content.mid(pos);
    content = std::move(highlighted);
  }
  // 可盯行正文同锚（右键气泡主体同样命中行键）。span 必须在锚内：链接字色
  // 取 QPalette::Link＝品牌橙，盖掉锚外的气泡字色 span——橙底橙字不可见
  // （Xvfb 实录）；text-decoration:none 保住元信息语言（不整段下划线）。
  if (!watch_key.isEmpty()) {
    content =
        QStringLiteral("<a href=\"%1\"><span style=\"color:%2; "
                       "text-decoration:none;\">%3</span></a>")
            .arg(esc(watch_href(watch_key)),
                 outgoing ? t.bubble_out_text.name() : t.bubble_in_text.name(),
                 content);
  }
  const QString body = QStringLiteral(
                           "<table cellspacing=\"0\" cellpadding=\"6\"><tr><td "
                           "bgcolor=\"%1\"><span style=\"color:%2;\">%3</span></td>"
                           "</tr></table>")
                           .arg(outgoing ? t.bubble_out.name() : t.bubble_in.name(),
                                outgoing ? t.bubble_out_text.name()
                                         : t.bubble_in_text.name(),
                                content);
  // 头像列（原型 .msg .avatar 28px 首字）：己方居右、对方居左；meta 行随
  // 气泡侧对齐（原型 .msg-col：己方元信息/回执勾在气泡上方右缘）
  const QString av = avatar_html(name);
  if (outgoing) {
    return QStringLiteral(
               "<div align=\"right\">%1</div>"
               "<table width=\"100%\" cellspacing=\"6\"><tr>"
               "<td width=\"28%\"></td><td align=\"right\" "
               "valign=\"top\">%2</td><td valign=\"top\">%3</td></tr></table>")
        .arg(meta, body, av);
  }
  return QStringLiteral(
             "<div>%1</div>"
             "<table width=\"100%\" cellspacing=\"6\"><tr>"
             "<td valign=\"top\">%2</td><td align=\"left\" "
             "valign=\"top\">%3</td><td width=\"28%\"></td></tr></table>")
      .arg(meta, av, body);
}

// 文件卡气泡（原型 .bubble.file-card：38px 图标格＋名称 700＋11px 元数据；
// 收发两向同款，仅左右与气泡底色随 outgoing 翻转）
QString file_bubble_html(const QString& name, const QString& file_name,
                         const QString& file_meta, qint64 ts_ms,
                         bool outgoing) {
  const ThemeTokens& t = tk();
  const QString meta =
      QStringLiteral("<div%1><span style=\"color:%2; font-size:small;\">%3 "
                     "%4</span></div>")
          .arg(outgoing ? QStringLiteral(" align=\"right\"") : QString(),
               t.text_muted.name(), esc(name), hhmm(ts_ms));
  const QString card =
      QStringLiteral(
          "<table cellspacing=\"0\" cellpadding=\"6\"><tr><td bgcolor=\"%1\">"
          "<table cellspacing=\"0\" cellpadding=\"0\"><tr>"
          "<td bgcolor=\"%2\" width=\"38\" height=\"38\" align=\"center\" "
          "valign=\"middle\"><span style=\"color:%3; font-weight:800; "
          "font-size:16px;\">%4</span></td>"
          "<td width=\"8\"></td>"
          "<td><span style=\"color:%5; font-weight:700;\">%6</span><br>"
          "<span style=\"color:%7; font-size:small;\">%8</span></td>"
          "</tr></table></td></tr></table>")
          .arg(outgoing ? t.bubble_out.name() : t.bubble_in.name(),
               t.brand_tint.name(), t.brand_text.name(),
               esc(file_icon_char(file_name)),
               outgoing ? t.bubble_out_text.name() : t.bubble_in_text.name(),
               esc(file_name), t.text_muted.name(), esc(file_meta));
  const QString av = avatar_html(name);
  if (outgoing) {
    return QStringLiteral(
               "<div align=\"right\">%1</div>"
               "<table width=\"100%\" cellspacing=\"6\"><tr>"
               "<td width=\"28%\"></td><td align=\"right\" "
               "valign=\"top\">%2</td><td valign=\"top\">%3</td></tr></table>")
        .arg(meta, card, av);
  }
  return QStringLiteral(
             "<div>%1</div>"
             "<table width=\"100%\" cellspacing=\"6\"><tr>"
             "<td valign=\"top\">%2</td><td align=\"left\" "
             "valign=\"top\">%3</td><td width=\"28%\"></td></tr></table>")
      .arg(meta, av, card);
}

// 居中系统行（建立会话提示／跨态说明／文件进度）：次要文本色。
// 正文不做转义——调用方按既有口径自行 esc（与改版前逐字一致）。
QString system_line_html(const QString& text) {
  return QStringLiteral("<div align=\"center\"><span style=\"color:%1; "
                        "font-size:small;\">%2</span></div>")
      .arg(ThemeManager::instance().tokens().text_muted.name(), text);
}

// 胶囊系统行（原型 .sys-hint .pill／.recalled .pill：panel-2 底＋细边，
// 富文本以单元格底色近似圆角胶囊）
QString chip_line_html(const QString& text) {
  const ThemeTokens& t = tk();
  return QStringLiteral(
             "<div align=\"center\"><table cellspacing=\"0\" "
             "cellpadding=\"3\"><tr><td bgcolor=\"%1\" align=\"center\">"
             "<span style=\"color:%2; font-size:small;\">%3</span></td></tr>"
             "</table></div>")
      .arg(t.surface_alt.name(), t.text_muted.name(), text);
}

// —— 需求批⑧ 聊天记录按日期 ——
// 日期锚点：消息行按本机日期带命名锚，跳转用 scrollToAnchor 定位。
// 系统行无时刻不锚——目标日以系统行开头时定位到其后最近的消息行。
QString day_anchor(qint64 ts_ms) {
  if (ts_ms <= 0) return QString();
  return QStringLiteral("<a name=\"d%1\"></a>")
      .arg(QDateTime::fromMSecsSinceEpoch(ts_ms)
               .date()
               .toString(QStringLiteral("yyyyMMdd")));
}
// 当日窗（毫秒含端点）：按本机时区取 [00:00:00.000, 23:59:59.999]。
qint64 day_start_ms(const QDate& d) {
  return QDateTime(d, QTime(0, 0)).toMSecsSinceEpoch();
}
qint64 day_end_ms(const QDate& d) {
  return day_start_ms(d) + 86400000LL - 1;
}
// 筛选头的时间端展示（0=不限侧以「…」示）
QString day_label(qint64 ms) {
  return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms).date().toString(
                      QStringLiteral("yyyy-MM-dd"))
                : QStringLiteral("…");
}
// 分日分隔线（需求批⑧余量收口）：居中日头；近两日用「今天／昨天」口吻
//（原型 .day-divider「今天 13:38」），更早沿用 yyyy-MM-dd 全量口径。
QString day_divider_html(const QDate& d) {
  const QDate today = QDate::currentDate();
  QString label = d.toString(QStringLiteral("yyyy-MM-dd"));
  if (d == today) {
    label = QStringLiteral("今天");
  } else if (d == today.addDays(-1)) {
    label = QStringLiteral("昨天");
  }
  return system_line_html(QStringLiteral("── %1 ──").arg(label));
}
// 需求批⑩ 在线时长秒数展示：服务端按秒计（滚动窗并集），四舍五入到
// 分钟级可读量——秒级展示太抖（ORG_QUERY 每次现算都在涨）。
QString format_online_s(qint64 seconds) {
  if (seconds < 60) return QStringLiteral("不足 1 分钟");
  const qint64 h = seconds / 3600;
  const qint64 m = (seconds % 3600) / 60;
  if (h <= 0) return QStringLiteral("%1 分钟").arg(m);
  if (m <= 0) return QStringLiteral("%1 小时").arg(h);
  return QStringLiteral("%1 小时 %2 分").arg(h).arg(m);
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("Memex"));
  setMinimumSize(320, 420); // 下限＝列表态最小可读（rail 60＋列表列 260）
  resize(1280, 800); // 先占位；build_ui 后再落一次（布局 sizeHint 会改写）

  build_ui();
  // build_ui 之后须再落一次（布局激活会按 sizeHint 改写窗尺寸，无 show 时）
  resize(1280, 800);
  // 布局令＋Telegram 机制（尺寸随布局态绑定）：启动无会话必然坍缩到列表态
  // 尺寸（宽 320）；点好友经 open_chat 展开到会话态宽。列表态存档由
  // restore_window_geometry 按态恢复——会话态宽窗不盖回无会话态
  close_chat();

  wire_engines();
  direct_engine_.start();
  // 双态共用同一份本地库：协作消息与直连消息同库，界面按 source 合并展示
  collab_engine_.attach_store(direct_engine_.store());
  // 「盯」提醒：启动装载（重启后盯仍有效）＋巡检定时（未读超阈值提醒
  // 一次、超时静默过期、离线期已读补触发）
  load_watches();
  watch_timer_ = new QTimer(this);
  watch_timer_->setInterval(30 * 1000);
  connect(watch_timer_, &QTimer::timeout, this, &MainWindow::sweep_watches);
  watch_timer_->start();
  sweep_watches();
  wire_collab();
  // T4.4 截图与标注：确认后发送（取消／失败仅提示）
  connect(&screenshot_tool_, &ScreenshotTool::confirmed, this,
          &MainWindow::on_screenshot_confirmed);
  connect(&screenshot_tool_, &ScreenshotTool::cancelled, this, [] {
    // 取消无需提示（用户主动放弃）
  });
  connect(&screenshot_tool_, &ScreenshotTool::failed, this,
          [this](const QString& reason) { show_status(reason); });

  // 离开锁屏（用户令 2026-10-08 ④）：全局输入事件喂计时器，超时即锁
  idle_timer_ = new QTimer(this);
  idle_timer_->setSingleShot(true);
  connect(idle_timer_, &QTimer::timeout, this,
          &MainWindow::maybe_lock_screen);
  qApp->installEventFilter(this);
  apply_away_lock_settings();
  apply_net_settings(); // 网段黑名单（QSettings 缺省=关=不拦）

  refresh_devices();
  show_status(QStringLiteral("就绪"));
  update_banner();

  // R19 · T4.9：主题切换即时生效——令牌变了就重刷控件样式与聊天区富文本
  connect(&ThemeManager::instance(), &ThemeManager::theme_changed, this,
          [this](const QString&) {
            apply_theme_styles();
            setWindowIcon(brand_icon());
            refresh_devices(); // 列表行控件按新令牌重建（行内样式非全局 QSS）
          });

  // 品牌物料：缓存先行（离线也有牌）＋拉取应用后整窗换牌（设计稿
  // 品牌物料.md §4——brand_applied 驱动，默认标兜底没配就不变）
  BrandKit::instance().load_cache();
  connect(&BrandKit::instance(), &BrandKit::brand_applied, this,
          &MainWindow::apply_brand);
  apply_brand();

  // 恢复上次窗口几何：按当前布局态恢复（启动＝列表态，只取列表态存档；
  // 会话态存档留给 open_chat，不盖回无会话态——2026-10-15 缺陷修复）。
  // 列表态存档宽不超 320（列表态右缘上限＝320），restore 不被 maximumWidth
  // 钳掉；上限复归在 restore 出口统一做，不在此先放开（放开＝新装环境
  // 上限恒失守）
  restore_window_geometry();
}

void MainWindow::apply_brand() {
  const BrandKit& kit = BrandKit::instance();
  setWindowTitle(kit.window_title());
  setWindowIcon(kit.window_icon());
  if (tray_) tray_->setIcon(kit.window_icon());
  if (brand_row_ != nullptr) {
    const bool has = kit.branded();
    brand_row_->setVisible(has);
    if (has) {
      if (!kit.logo().isNull()) {
        brand_logo_->setPixmap(
            kit.logo().scaled(20, 20, Qt::KeepAspectRatio,
                              Qt::SmoothTransformation));
      }
      brand_name_->setText(kit.company_name());
    }
  }
}

void MainWindow::build_ui() {
  // —— 菜单：协作态登录／登出（不重启切换形态，T2.4）；群聊（T4.1）——
  auto* collab_menu = menuBar()->addMenu(QStringLiteral("协作"));
  auto* act_login = collab_menu->addAction(QStringLiteral("登录协作态…"));
  auto* act_org = collab_menu->addAction(QStringLiteral("组织架构…"));
  act_collab_logout_ = collab_menu->addAction(QStringLiteral("登出（回到直连态）"));
  act_collab_logout_->setEnabled(false);
  collab_menu->addSeparator();
  auto* act_group_new =
      collab_menu->addAction(QStringLiteral("新建群聊…"));
  auto* act_dgroup_new =
      collab_menu->addAction(QStringLiteral("新建临时群（直连，不进归档）…"));
  connect(act_login, &QAction::triggered, this,
          &MainWindow::show_collab_login_dialog);
  connect(act_org, &QAction::triggered, this, &MainWindow::enter_org_view);
  connect(act_collab_logout_, &QAction::triggered, this,
          &MainWindow::logout_collab);
  connect(act_group_new, &QAction::triggered, this,
          [this] { create_group_dialog(); });
  connect(act_dgroup_new, &QAction::triggered, this, [this] { dgroup_dialog(); });
  // R23-3 文件助手（自己↔自己）：备忘录 + 收件箱统一收件，独立文件面会话
  auto* act_files = collab_menu->addAction(QStringLiteral("文件助手…"));
  connect(act_files, &QAction::triggered, this,
          &MainWindow::show_file_assistant);

  // R27-1 个人任务清单：一级菜单入口（设计拍板：与会话/通讯录同级常驻，
  // 不埋设置页；独立文件面会话窗口）
  auto* task_menu = menuBar()->addMenu(QStringLiteral("任务"));
  auto* act_tasks = task_menu->addAction(QStringLiteral("任务清单…"));
  connect(act_tasks, &QAction::triggered, this, [this] {
    if (!task_dialog_) {
      task_dialog_ = new TaskDialog(this);
      // 不设 WA_DeleteOnClose：关窗=hide 后台驻留——30s 到期轮询与
      // 文件面连接持续，提醒经常驻通知中心照发（关窗后台提醒常驻化）
    }
    // 分配候选=组织架构账号（未拉到组织架构时候选只剩「自己」）
    const QJsonDocument od = QJsonDocument::fromJson(last_org_json_.toUtf8());
    QStringList accounts;
    if (od.isObject()) {
      for (const auto& m : od.object().value(QStringLiteral("members"))
                               .toArray()) {
        accounts << m.toObject()
                        .value(QStringLiteral("account"))
                        .toString();
      }
    }
    task_dialog_->set_assignees(accounts);
    task_dialog_->show();
    task_dialog_->raise();
    task_dialog_->activateWindow();
  });
  // 二期·审批（请假起步）：同组常驻入口；判权全在服务端，窗口无候选面
  auto* act_approvals = task_menu->addAction(QStringLiteral("审批…"));
  connect(act_approvals, &QAction::triggered, this, [this] {
    if (!approval_dialog_) {
      approval_dialog_ = new ApprovalDialog(this);
      approval_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    approval_dialog_->show();
    approval_dialog_->raise();
    approval_dialog_->activateWindow();
  });
  // 二期·日报周报：写日报/看下属（直属上级）；判权全在服务端
  auto* act_reports = task_menu->addAction(QStringLiteral("日报周报…"));
  connect(act_reports, &QAction::triggered, this, [this] {
    if (!report_dialog_) {
      report_dialog_ = new ReportDialog(this);
      report_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    report_dialog_->show();
    report_dialog_->raise();
    report_dialog_->activateWindow();
  });
  // 二期·群工具三件（投票/接龙/群任务）：判权全在服务端，窗口只提交与展示
  auto* act_group_tools = task_menu->addAction(QStringLiteral("群工具…"));
  connect(act_group_tools, &QAction::triggered, this, [this] {
    if (!group_tools_dialog_) {
      group_tools_dialog_ = new GroupToolsDialog(this);
      group_tools_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    group_tools_dialog_->show();
    group_tools_dialog_->raise();
    group_tools_dialog_->activateWindow();
  });
  // 「盯」列表（实现单修订 2026-10-14）：入口看到「我在盯哪些」，逐条可取消
  auto* act_watch_list = task_menu->addAction(QStringLiteral("盯列表…"));
  connect(act_watch_list, &QAction::triggered, this,
          [this] { open_watch_list(); });
  // 二期·会话审计：归档在线检索＋查阅日志（auditor 持证；被拒服务端留痕）
  auto* act_audit = collab_menu->addAction(QStringLiteral("会话审计…"));
  connect(act_audit, &QAction::triggered, this, [this] {
    if (!audit_dialog_) {
      audit_dialog_ = new AuditDialog(this);
      audit_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    audit_dialog_->show();
    audit_dialog_->raise();
    audit_dialog_->activateWindow();
  });
  // 管理台·归档检索台（原型 desktop-search）：auditor 整窗控制台——条件
  // 检索＋五列命中表＋上下文/撤回事件/导出留证（每次查阅服务端留痕）
  auto* act_console = collab_menu->addAction(QStringLiteral("归档检索台…"));
  connect(act_console, &QAction::triggered, this, [this] {
    if (!archive_console_) {
      archive_console_ = new ArchiveConsole(this);
      archive_console_->setAttribute(Qt::WA_DeleteOnClose);
    }
    archive_console_->show();
    archive_console_->raise();
    archive_console_->activateWindow();
  });
  // 二期·办公室位置图：抽象平面＋拖拽落位（编辑归 org-admin，服务端现裁）
  auto* act_office = collab_menu->addAction(QStringLiteral("办公室位置图…"));
  connect(act_office, &QAction::triggered, this, [this] {
    if (!office_dialog_) {
      office_dialog_ = new OfficeMapDialog(this);
      office_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    office_dialog_->show();
    office_dialog_->raise();
    office_dialog_->activateWindow();
  });
  // 二期·远程协助：受控方显式确认＋过程持续可见（consent/audit 红线在
  // 服务端，客户端只做门面）
  auto* act_assist = collab_menu->addAction(QStringLiteral("远程协助…"));
  connect(act_assist, &QAction::triggered, this, [this] {
    if (!assist_dialog_) {
      assist_dialog_ = new AssistDialog(this);
      assist_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    assist_dialog_->show();
    assist_dialog_->raise();
    assist_dialog_->activateWindow();
  });

  // —— 设置：开机启动（T4.7；勾选态与登记文件同步）＋主题（R19 · T4.9）——
  auto* opt_menu = menuBar()->addMenu(QStringLiteral("设置"));
  opt_menu_ = opt_menu; // rail 齿轮（直连态）弹同源菜单，入口不分叉
  // —— 个人资料（需求批⑪）：个性签名设置（登录门在对话框入口）——
  auto* act_profile = opt_menu->addAction(QStringLiteral("个人资料…"));
  connect(act_profile, &QAction::triggered, this,
          &MainWindow::show_profile_dialog);
  auto* act_notify = opt_menu->addAction(QStringLiteral("通知偏好…"));
  connect(act_notify, &QAction::triggered, this, [] {
    NotificationCenter::instance().show_settings();
  });
  // —— 已读回执全局开关（需求批⑦）：勾选态即全局缺省；会话级覆盖走
  //     会话右键菜单——空 peer=全局写 QSettings receipts/send_read
  auto* act_receipts = opt_menu->addAction(QStringLiteral("发送已读回执"));
  act_receipts->setCheckable(true);
  act_receipts->setChecked(read_receipts_enabled(QString()));
  connect(act_receipts, &QAction::toggled, this, [this](bool on) {
    set_read_receipts_enabled(QString(), on);
  });
  act_theme_ = opt_menu->addAction(QStringLiteral("主题…"));
  act_theme_->setMenuRole(QAction::PreferencesRole); // macOS 走应用菜单偏好项
  connect(act_theme_, &QAction::triggered, this, &MainWindow::show_theme_settings);
  // —— 品牌物料设置页（设计稿 docs/design/品牌物料.md §4）：org-admin
  //     配牌入口；保存成功后本机 BrandKit 即时换牌（＝本机先见）——
  auto* act_brand = opt_menu->addAction(QStringLiteral("品牌物料…"));
  connect(act_brand, &QAction::triggered, this, [this] {
    if (!brand_settings_dialog_) {
      brand_settings_dialog_ = new BrandSettingsDialog(this);
      brand_settings_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    brand_settings_dialog_->show();
    brand_settings_dialog_->raise();
    brand_settings_dialog_->activateWindow();
  });
  // —— 连接设置（部署批 424：域名解析覆盖，局域网直连）——
  auto* act_conn = opt_menu->addAction(QStringLiteral("连接设置…"));
  connect(act_conn, &QAction::triggered, this,
          &MainWindow::show_conn_settings_dialog);
  // —— 快捷键设置（用户令 2026-10-08 ③：截图键可改＋冲突检测）——
  auto* act_shortcut = opt_menu->addAction(QStringLiteral("快捷键…"));
  connect(act_shortcut, &QAction::triggered, this, [this] {
    if (!shortcut_dialog_) {
      shortcut_dialog_ = new ShortcutSettingsDialog(this, this);
      shortcut_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    shortcut_dialog_->show();
    shortcut_dialog_->raise();
    shortcut_dialog_->activateWindow();
  });
  // —— 离开锁屏设置（用户令 2026-10-08 ④：离开密码＋无操作超时＋启用）——
  auto* act_away = opt_menu->addAction(QStringLiteral("离开与锁屏…"));
  connect(act_away, &QAction::triggered, this, [this] {
    if (!away_dialog_) {
      away_dialog_ = new AwayLockSettingsDialog(this, this);
      away_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    away_dialog_->show();
    away_dialog_->raise();
    away_dialog_->activateWindow();
  });
  // —— 网络与远程设置（用户令 2026-10-08 ⑤：网段黑名单＋远程控制密码）——
  auto* act_net = opt_menu->addAction(QStringLiteral("网络与远程…"));
  connect(act_net, &QAction::triggered, this, [this] {
    if (!net_dialog_) {
      net_dialog_ = new NetRemoteSettingsDialog(this, this);
      net_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    net_dialog_->show();
    net_dialog_->raise();
    net_dialog_->activateWindow();
  });
  opt_menu->addSeparator();
  act_autostart_ =
      opt_menu->addAction(QStringLiteral("开机启动（登录后自动运行）"));
  act_autostart_->setCheckable(true);
  act_autostart_->setChecked(autostart_enabled());
  connect(act_autostart_, &QAction::triggered, this, [this](bool on) {
    set_autostart(on);
    act_autostart_->setChecked(autostart_enabled()); // 落盘失败回滚勾选，不说谎
  });

  // —— 帮助：关于页（A23 图标面：logo＋版本，与窗口/任务栏图标同源）——
  auto* help_menu = menuBar()->addMenu(QStringLiteral("帮助"));
  auto* act_about = help_menu->addAction(QStringLiteral("关于 Memex…"));
  act_about->setMenuRole(QAction::AboutRole); // macOS 走应用菜单「关于」项
  connect(act_about, &QAction::triggered, this, [this] { show_about(); });

  setWindowIcon(brand_icon());
  setup_tray(); // 托盘可用才建（offscreen 等环境跳过）

  // —— 左侧：局域网设备列表（直连态以设备替代联系人）——
  auto* side = new QWidget(this);
  side_ = side;
  side->setObjectName(QStringLiteral("side_column"));
  auto* side_layout = new QVBoxLayout(side);
  side_layout->setContentsMargins(10, 10, 6, 10);
  side_layout->setSpacing(6);

  // 品牌行（服务器配了牌才显＝没配就不变；apply_brand 驱动显隐与内容）
  auto* brand_row = new QWidget(side);
  brand_row_ = brand_row;
  auto* brand_lay = new QHBoxLayout(brand_row);
  brand_lay->setContentsMargins(0, 0, 0, 0);
  brand_lay->setSpacing(6);
  brand_logo_ = new QLabel(brand_row);
  brand_logo_->setFixedSize(20, 20);
  brand_name_ = new QLabel(brand_row);
  brand_lay->addWidget(brand_logo_);
  brand_lay->addWidget(brand_name_);
  brand_lay->addStretch();
  brand_row->setVisible(false); // 默认标兜底：无牌不占位

  auto* side_head = new QHBoxLayout();
  auto* side_title = new QLabel(QStringLiteral("局域网设备"), side);
  side_title_ = side_title;
  device_count_ = new QLabel(side);
  side_head->addWidget(side_title);
  side_head->addStretch();
  side_head->addWidget(device_count_);
  // 发起单聊（原型 .side-head 品牌圆角 + 钮）：协作态可见，账号菜单开聊
  new_chat_btn_ = new QPushButton(QStringLiteral("+"), side);
  new_chat_btn_->setObjectName(QStringLiteral("new_chat_btn"));
  new_chat_btn_->setFixedSize(26, 26);
  new_chat_btn_->setCursor(Qt::PointingHandCursor);
  new_chat_btn_->setToolTip(QStringLiteral("发起单聊"));
  new_chat_btn_->setVisible(false);
  connect(new_chat_btn_, &QPushButton::clicked, this,
          &MainWindow::show_new_chat_menu);
  side_head->addWidget(new_chat_btn_);

  search_box_ = new QLineEdit(side);
  search_box_->setPlaceholderText(QStringLiteral("搜索设备 / IP"));
  search_box_->setClearButtonEnabled(true);

  // 发现口径注脚（原型 side-head：自动发现本网段 x.x.x.0/24 · UDP 2425）：
  // 子网按本机首个非环回 IPv4 推 /24；取不到地址时退「本网段」不编数。
  {
    QString subnet = QStringLiteral("本网段");
    for (const QNetworkInterface& ni : QNetworkInterface::allInterfaces()) {
      if (ni.flags() & QNetworkInterface::IsLoopBack) continue;
      for (const QNetworkAddressEntry& e : ni.addressEntries()) {
        const QHostAddress a = e.ip();
        if (a.protocol() != QAbstractSocket::IPv4Protocol) continue;
        const quint32 v = a.toIPv4Address();
        subnet = QHostAddress(v & 0xFFFFFF00u).toString() + QStringLiteral("/24");
        break;
      }
      if (subnet != QStringLiteral("本网段")) break;
    }
    disc_direct_text_ = QStringLiteral("自动发现 %1 · UDP %2")
                            .arg(subnet, QString::number(kDiscoveryPort));
    disc_hint_ = new QLabel(disc_direct_text_, side);
  }

  device_list_ = new QListWidget(side);
  device_list_->setObjectName(QStringLiteral("device_list"));
  device_list_->setFrameShape(QFrame::NoFrame);
  device_list_->setSelectionMode(QAbstractItemView::SingleSelection);
  // 群条目右键菜单（T4.1）：拉人／公告／退群／解散
  device_list_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(device_list_, &QListWidget::customContextMenuRequested, this,
          [this](const QPoint& pos) { show_group_menu(pos); });

  // —— 直连态「登录协作态」推广卡（原型直连屏侧栏面）：未登录可见，
  //     说明行＋登录按钮（走既有登录对话框，不另起流程）——
  collab_promo_ = new QWidget(side);
  collab_promo_->setObjectName(QStringLiteral("collab_promo"));
  auto* promo_lay = new QVBoxLayout(collab_promo_);
  promo_lay->setContentsMargins(10, 8, 10, 8);
  promo_lay->setSpacing(3);
  auto* promo_row = new QHBoxLayout();
  auto* promo_title = new QLabel(QStringLiteral("登录协作态"), collab_promo_);
  promo_title->setObjectName(QStringLiteral("collab_promo_title"));
  auto* promo_btn = new QPushButton(QStringLiteral("登录"), collab_promo_);
  promo_btn->setObjectName(QStringLiteral("collab_promo_btn"));
  promo_btn->setCursor(Qt::PointingHandCursor);
  connect(promo_btn, &QPushButton::clicked, this,
          &MainWindow::show_collab_login_dialog);
  promo_row->addWidget(promo_title);
  promo_row->addStretch();
  promo_row->addWidget(promo_btn);
  auto* promo_sub =
      new QLabel(QStringLiteral("组织架构、云端历史、消息归档检索"), collab_promo_);
  promo_sub->setObjectName(QStringLiteral("collab_promo_sub"));
  promo_sub->setWordWrap(true);
  promo_lay->addLayout(promo_row);
  promo_lay->addWidget(promo_sub);

  side_layout->addLayout(side_head);
  side_layout->addWidget(search_box_);
  side_layout->addWidget(disc_hint_);
  side_layout->addWidget(collab_promo_);
  side_layout->addWidget(device_list_, 1);
  side_layout->insertWidget(0, brand_row_);

  // —— 右侧：聊天窗 ——（布局令：空态隐藏，open_chat 展开成两栏）
  auto* chat = new QWidget(this);
  chat_panel_ = chat;
  auto* chat_layout = new QVBoxLayout(chat);
  chat_layout->setContentsMargins(0, 0, 0, 0);
  chat_layout->setSpacing(0);

  auto* head = new QWidget(chat);
  head_ = head;
  auto* head_layout = new QHBoxLayout(head);
  head_layout->setContentsMargins(14, 10, 14, 10);
  head_layout->setSpacing(10);
  // 会话头像（原型 .chat-head .avatar.sm 28px 首字）：随会话与主题重刷
  chat_avatar_ = new QLabel(head);
  chat_avatar_->setFixedSize(28, 28);
  chat_avatar_->setAlignment(Qt::AlignCenter);
  chat_title_ = new QLabel(QStringLiteral("直连态"), head);
  chat_meta_ = new QLabel(QStringLiteral("未选择设备"), head);
  auto* local_badge = new QLabel(QStringLiteral("本机保存"), head);
  local_badge_ = local_badge;
  auto* chat_search_btn = new QPushButton(QStringLiteral("🔍"), head);
  chat_search_btn_ = chat_search_btn;
  chat_search_btn->setFlat(true);
  chat_search_btn->setToolTip(
      QStringLiteral("搜索聊天记录（本机）：只记一半、词在中间、多词 AND"));
  connect(chat_search_btn, &QPushButton::clicked, this, [this] {
    if (!message_search_) {
      message_search_ = new MessageSearchDialog(this);
      message_search_->setAttribute(Qt::WA_DeleteOnClose);
      connect(message_search_, &MessageSearchDialog::open_message, this,
              [this](const QString& peer, const QString& msg_id) {
                if (peer.isEmpty()) return;
                // 对端键→会话类型：群键＝group；协作会话列表内＝collab；
                // 其余（含直连设备）＝direct。
                QString kind = QStringLiteral("direct");
                if (peer.startsWith(QStringLiteral("group:"))) {
                  kind = QStringLiteral("group");
                } else if (collab_engine_.is_logged_in() &&
                           collab_peers_.contains(peer)) {
                  kind = QStringLiteral("collab");
                }
                open_chat(kind, peer);
                // 命中定位（尽力而为）：气泡无单条锚点，退一级滚到命中所在日
                if (!msg_id.isEmpty()) {
                  for (const ChatRow& r : chat_rows_) {
                    if (!r.system && r.msg_id == msg_id) {
                      scroll_to_day(
                          QDateTime::fromMSecsSinceEpoch(r.ts_ms).date());
                      break;
                    }
                  }
                }
              });
    }
    message_search_->bind_store(direct_engine_.store());
    message_search_->show();
    message_search_->raise();
    message_search_->activateWindow();
    if (message_search_->search_box()) message_search_->search_box()->setFocus();
  });
  auto* close_chat_btn = new QPushButton(QStringLiteral("✕"), head);
  close_chat_btn->setFlat(true);
  close_chat_btn->setToolTip(QStringLiteral("关闭会话（回到列表态）"));
  connect(close_chat_btn, &QPushButton::clicked, this,
          &MainWindow::close_chat);
  // 头部动作位（原型 .ch-actions 文件/更多）：文件走生产发文件路径；
  // 更多＝既有会话操作菜单（按会话形态给真实项，不摆空钮）。语音无实现
  // 不入面（台账近似项）
  auto* head_file_btn = new QPushButton(QStringLiteral("📄"), head);
  head_file_btn->setFlat(true);
  head_file_btn->setFixedSize(30, 30);
  head_file_btn->setCursor(Qt::PointingHandCursor);
  head_file_btn->setToolTip(QStringLiteral("发送文件（点对点）"));
  connect(head_file_btn, &QPushButton::clicked, this,
          [this] { file_btn_->click(); });
  auto* head_more_btn = new QPushButton(QStringLiteral("⋮"), head);
  head_more_btn->setFlat(true);
  head_more_btn->setFixedSize(30, 30);
  head_more_btn->setCursor(Qt::PointingHandCursor);
  head_more_btn->setToolTip(QStringLiteral("更多会话操作"));
  connect(head_more_btn, &QPushButton::clicked, this, [this, btn = head_more_btn] {
    QMenu menu(this);
    const QString kind = current_kind_;
    if (kind == QStringLiteral("group")) {
      const quint64 gid = current_group_id();
      if (gid) {
        connect(menu.addAction(QStringLiteral("群资料…")),
                &QAction::triggered, this,
                [this, gid] { group_profile_dialog(gid); });
        connect(menu.addAction(QStringLiteral("拉人进群…")),
                &QAction::triggered, this,
                [this, gid] { group_invite_dialog(gid); });
        connect(menu.addAction(QStringLiteral("群公告编辑…")),
                &QAction::triggered, this,
                [this, gid] { group_announce_dialog(gid); });
        connect(menu.addAction(QStringLiteral("导出群记录…")),
                &QAction::triggered, this,
                [this, gid] { group_record_export_dialog(gid); });
      }
      connect(menu.addAction(QStringLiteral("群工具…")), &QAction::triggered,
              this, [this] {
                if (!group_tools_dialog_) {
                  group_tools_dialog_ = new GroupToolsDialog(this);
                  group_tools_dialog_->setAttribute(Qt::WA_DeleteOnClose);
                }
                group_tools_dialog_->show();
                group_tools_dialog_->raise();
                group_tools_dialog_->activateWindow();
              });
    } else {
      QAction* nudge = menu.addAction(QStringLiteral("窗口抖动"));
      connect(nudge, &QAction::triggered, this, [this] { nudge_btn_->click(); });
      QAction* by_date = menu.addAction(QStringLiteral("按日期查看…"));
      connect(by_date, &QAction::triggered, this, [this] { date_btn_->click(); });
      QAction* search = menu.addAction(QStringLiteral("搜索本机记录…"));
      connect(search, &QAction::triggered, this, [this] { chat_search_btn_->click(); });
    }
    menu.exec(btn->mapToGlobal(QPoint(0, btn->height())));
  });
  // 群聊头部动作位（原型 desktop-group.html .ch-actions：群公告/群文件/成员
  // brand 钮）——仅群会话显示；公告编辑与群资料走既有真实弹窗，群文件＝
  // 切右栏（.infopanel）显隐
  auto* head_announce_btn = new QPushButton(QStringLiteral("📢"), head);
  head_announce_btn_ = head_announce_btn;
  head_announce_btn->setFlat(true);
  head_announce_btn->setFixedSize(30, 30);
  head_announce_btn->setCursor(Qt::PointingHandCursor);
  head_announce_btn->setToolTip(QStringLiteral("群公告（编辑）"));
  head_announce_btn->hide();
  connect(head_announce_btn, &QPushButton::clicked, this, [this] {
    const quint64 gid = current_group_id();
    if (gid) group_announce_dialog(gid);
  });
  auto* head_gfiles_btn = new QPushButton(QStringLiteral("🗂"), head);
  head_gfiles_btn_ = head_gfiles_btn;
  head_gfiles_btn->setFlat(true);
  head_gfiles_btn->setFixedSize(30, 30);
  head_gfiles_btn->setCursor(Qt::PointingHandCursor);
  head_gfiles_btn->setToolTip(QStringLiteral("群文件（右侧栏）"));
  head_gfiles_btn->hide();
  connect(head_gfiles_btn, &QPushButton::clicked, this, [this] {
    group_panel_hidden_ = !group_panel_hidden_;
    if (group_panel_) group_panel_->setVisible(!group_panel_hidden_);
  });
  auto* head_members_btn = new QPushButton(QStringLiteral("👥"), head);
  head_members_btn_ = head_members_btn;
  head_members_btn->setFlat(true);
  head_members_btn->setFixedSize(30, 30);
  head_members_btn->setCursor(Qt::PointingHandCursor);
  head_members_btn->setToolTip(QStringLiteral("群成员（群资料）"));
  head_members_btn->hide();
  connect(head_members_btn, &QPushButton::clicked, this, [this] {
    const quint64 gid = current_group_id();
    if (gid) group_profile_dialog(gid);
  });
  head_layout->addWidget(chat_avatar_);
  head_layout->addWidget(chat_title_);
  head_layout->addWidget(chat_meta_);
  head_layout->addStretch();
  head_layout->addWidget(local_badge);
  head_layout->addWidget(chat_search_btn);
  head_layout->addWidget(head_announce_btn);
  head_layout->addWidget(head_gfiles_btn);
  head_layout->addWidget(head_members_btn);
  head_layout->addWidget(head_file_btn);
  head_layout->addWidget(head_more_btn);
  head_layout->addWidget(close_chat_btn);

  // 「未归档」语义标记：常驻不可关（留痕铁律的界面表达）；
  // 文案随形态切换（T2.4）：直连／降级态必须出现「消息不进归档」。
  banner_ = new QLabel(chat);
  banner_->setObjectName(QStringLiteral("mode_banner"));
  banner_->setWordWrap(true);

  chat_view_ = new QTextBrowser(chat);
  chat_view_->setFrameShape(QFrame::NoFrame);
  // 「盯」单：气泡内锚仅用于右键定位行键——关掉左键跟随链接（无导航
  // 副作用；文档内其他链接本就不存在，命名日锚不受影响）
  chat_view_->setOpenLinks(false);
  // 需求批② 表情包＋「盯」单：右键气泡菜单——图片气泡「收藏到表情包」；
  // 可盯文本气泡「盯住本消息／取消盯」（已读行入口置灰——已读无需盯）。
  chat_view_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(chat_view_, &QTextBrowser::customContextMenuRequested, this,
          [this](const QPoint& pos) {
    QTextCursor cur = chat_view_->cursorForPosition(pos);
    const QTextImageFormat img = cur.charFormat().toImageFormat();
    const QString src = img.name(); // file:///…
    if (img.isValid() && src.startsWith(QStringLiteral("file://"))) {
      const QString path = QUrl(src).toLocalFile();
      auto* menu = new QMenu(this);
      QAction* act = menu->addAction(QStringLiteral("收藏到表情包"));
      connect(act, &QAction::triggered, this, [this, path] {
        if (import_emoji(path)) {
          show_status(QStringLiteral("已收藏到表情包：%1（表情面板可见）")
                          .arg(QFileInfo(path).fileName()));
        }
      });
      menu->setAttribute(Qt::WA_DeleteOnClose);
      menu->popup(chat_view_->mapToGlobal(pos));
      return;
    }
    // 可盯行：锚携带行键（meta 行与正文同锚）。取锚不走 anchorAt——Qt 对
    // 表内（气泡是嵌套表）锚的命中区会空间错位（探针实录：字形区全空、
    // 命中区漂到表外，右键气泡主体永远取不到键）；cursorForPosition 的
    // 字符格式锚在顶层与嵌套表上都按字形区可靠解析。
    const QString key = watch_key_from_href(
        chat_view_->cursorForPosition(pos).charFormat().anchorHref());
    if (key.isEmpty()) {
      return; // 非可盯区域：不弹菜单（维持既有行为）
    }
    auto* menu = new QMenu(this);
    if (watched_.contains(key)) {
      QAction* act = menu->addAction(QStringLiteral("取消盯"));
      connect(act, &QAction::triggered, this, [this, key] {
        unwatch_key(key);
      });
    } else {
      const bool already_read = [&] {
        for (const ChatRow& r : chat_rows_) {
          if (!r.system && r.watch_key == key &&
              r.receipt == QStringLiteral("read")) {
            return true;
          }
        }
        return false;
      }();
      QAction* act = menu->addAction(QStringLiteral("盯住本消息"));
      act->setEnabled(!already_read); // 已读＝置灰（盯与已读同生命周期已尽）
      if (already_read) {
        act->setStatusTip(QStringLiteral("该消息已被读"));
      }
      connect(act, &QAction::triggered, this, [this, key] {
        watch_current_row(key);
      });
    }
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(chat_view_->mapToGlobal(pos));
  });

  auto* input_row = new QWidget(chat);
  input_row_ = input_row;
  auto* input_layout = new QHBoxLayout(input_row);
  input_layout->setContentsMargins(10, 8, 10, 8);
  auto* file_btn = new QPushButton(QStringLiteral("发文件"), input_row);
  file_btn_ = file_btn;
  // 需求批⑤ 发文件夹：递归整目录（含子目录）走既有文件通道，对端按相对
  // 路径重建结构；进度按「第 x/N 个文件」聚合（引擎逐文件串行发）。
  auto* folder_btn = new QPushButton(QStringLiteral("发文件夹"), input_row);
  folder_btn_ = folder_btn;
  folder_btn->setToolTip(
      QStringLiteral("递归发送整个文件夹（含子目录，对端按相对路径重建）"));
  // T4.4 截图与标注：按钮＋ Ctrl+Alt+A 快捷键（与「发文件」同发送口径）
  auto* shot_btn = new QPushButton(QStringLiteral("截图"), input_row);
  shot_btn_ = shot_btn;
  shot_btn->setToolTip(
      QStringLiteral("截图并标注（%1）").arg(screenshot_shortcut()));
  // 截图快捷键（用户令 2026-10-08：可在设置改键——QSettings
  // shortcuts/screenshot，默认 Ctrl+Alt+A；冲突检测见 apply_…）
  shot_sc_ = new QShortcut(QKeySequence(screenshot_shortcut()), this);
  shot_sc_->setContext(Qt::WindowShortcut);
  connect(shot_btn, &QPushButton::clicked, this, [this] { start_screenshot(); });
  connect(shot_sc_, &QShortcut::activated, this,
          [this] { start_screenshot(); });
  // T4.5 表情：内置（按频次排序）＋自定义表情包导入（走文件通道发送）
  auto* emoji_btn = new QPushButton(QStringLiteral("表情"), input_row);
  emoji_btn_ = emoji_btn;
  connect(emoji_btn, &QPushButton::clicked, this,
          [this] { show_emoji_panel(); });
  // 需求批①文字颜色：选区着色（弹取色器，标记入输入框可继续编辑）
  auto* color_btn = new QPushButton(QStringLiteral("颜色"), input_row);
  color_btn_ = color_btn;
  color_btn->setToolTip(QStringLiteral("选中文字后点此设置颜色"));
  connect(color_btn, &QPushButton::clicked, this,
          [this] { request_input_color(); });
  auto* nudge_btn = new QPushButton(QStringLiteral("振屏"), input_row);
  nudge_btn_ = nudge_btn;
  nudge_btn->setToolTip(
      QStringLiteral("发送窗口抖动提醒（单聊；同一会话 10 秒一次）"));
  connect(nudge_btn, &QPushButton::clicked, this,
          [this] { send_nudge_to_current_chat(); });
  // 需求批⑧ 按日期查看：日历选择器跳转＋范围筛选（协作/群登录态自动
  // 向服务端补拉该窗，回包落本地索引后重渲）
  auto* date_btn = new QPushButton(QStringLiteral("按日期"), input_row);
  date_btn_ = date_btn;
  date_btn->setToolTip(QStringLiteral(
      "按日期跳转或范围筛选聊天记录（协作/群会话自动向服务端补拉）"));
  connect(date_btn, &QPushButton::clicked, this,
          [this] { show_date_dialog(); });
  input_layout->addWidget(shot_btn);
  input_layout->addWidget(file_btn);
  input_layout->addWidget(folder_btn);
  input_layout->addWidget(emoji_btn);
  input_layout->addWidget(color_btn);
  input_layout->addWidget(nudge_btn);
  input_layout->addWidget(date_btn);
  input_box_ = new QLineEdit(input_row);
  input_box_->setObjectName(QStringLiteral("input_box"));
  input_box_->setPlaceholderText(QStringLiteral("输入消息…"));
  send_btn_ = new QPushButton(QStringLiteral("发送"), input_row);
  // 「Enter 发送」提示（原型 .input-toolbar .hint：右缘 11px 次要色）
  auto* enter_hint = new QLabel(QStringLiteral("Enter 发送"), input_row);
  enter_hint->setStyleSheet(
      QStringLiteral("color:%1; font-size:11px;")
          .arg(ThemeManager::instance().tokens().text_muted.name()));
  // 「@ 唤起成员选择」提示（原型 desktop-group.html .input-toolbar .hint）：
  // 群会话专用（update_banner 控显隐；颜色随主题在 apply_theme_styles 重刷）
  at_hint_ = new QLabel(QStringLiteral("@ 唤起成员选择"), input_row);
  at_hint_->setStyleSheet(
      QStringLiteral("color:%1; font-size:11px;")
          .arg(ThemeManager::instance().tokens().text_muted.name()));
  at_hint_->hide();
  // 未归档口径小胶囊（原型 .input-foot .pill-outline「直连 · 无归档」）：
  // 随形态换字，归档会话不挂
  noarch_pill_ = new QLabel(input_row);
  input_layout->addWidget(input_box_, 1);
  input_layout->addWidget(at_hint_);
  input_layout->addWidget(enter_hint);
  input_layout->addWidget(noarch_pill_);
  input_layout->addWidget(send_btn_);

  // 输入区固定说明条（原型 .input-note）：随形态换文案（update_banner 同
  // 一状态机），跨态/直连＝警示底不可关闭，协作＝素底归档说明
  input_note_ = new QLabel(chat);
  input_note_->setWordWrap(true);

  chat_layout->addWidget(head);
  chat_layout->addWidget(banner_);
  chat_layout->addWidget(chat_view_, 1);
  chat_layout->addWidget(input_note_);
  chat_layout->addWidget(input_row);

  auto* central = new QWidget(this);
  auto* layout = new QHBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  // —— 最左：图标导航栏（原型桌面屏 rail 面，用户令 2026-10-10 原型走查
  //     P0：主导航缺失）。分组与顺序严格按原型，见 rebuild_rail()。——
  rail_ = new RailNav(central);
  rail_->setObjectName(QStringLiteral("rail_nav"));
  connect(rail_, &RailNav::item_clicked, this, &MainWindow::rail_clicked);
  layout->addWidget(rail_);
  side->setFixedWidth(260);
  layout->addWidget(side);
  // chat 高 stretch（100）：会话态吸收几乎全部余量。列表态（chat 隐藏）不再
  // 用尾部 addStretch 吸收余量——那会在右缘留整片空白（2026-10-14 用户报
  // 启动横窗右侧空白）；改为列表列本身 Expanding 占满 rail 之外的宽度
  //（close_chat 放开上限，open_chat 收回 260 固定宽）。
  layout->addWidget(chat, 100);
  // —— 群右栏（原型 desktop-group.html .infopanel：群成员/群文件/归档说明
  //     虚线卡，240px 固定宽）：仅群会话显示，内容与显隐归 update_group_panel ——
  group_panel_ = new QWidget(central);
  group_panel_->setObjectName(QStringLiteral("group_panel"));
  group_panel_->setFixedWidth(240);
  group_panel_lay_ = new QVBoxLayout(group_panel_);
  group_panel_lay_->setContentsMargins(16, 18, 16, 18);
  group_panel_lay_->setSpacing(14);
  group_panel_->hide();
  layout->addWidget(group_panel_);

  // —— 文件传输面（原型 desktop-files.html 整屏）：rail「文件传输」切视图，
  //     与会话侧栏/聊天面板互斥——侧栏换「传输通道」页（220px）、主区换
  //     传输列表（头部 meta/标签/传输卡/归档说明卡）。侧栏内容随主题与发现
  //     表全量重建（rebuild_transfer_side），传输卡随台账重建
  //     （refresh_transfers_view），此处只搭骨架。——
  transfer_side_ = new QWidget(central);
  transfer_side_->setObjectName(QStringLiteral("transfer_side"));
  transfer_side_->setFixedWidth(220);
  {
    auto* tsl = new QVBoxLayout(transfer_side_);
    tsl->setContentsMargins(10, 10, 6, 10);
    tsl->setSpacing(6);
  }
  transfer_side_->hide();

  transfer_page_ = new QWidget(central);
  transfer_page_->setObjectName(QStringLiteral("transfer_page"));
  {
    auto* tpl = new QVBoxLayout(transfer_page_);
    tpl->setContentsMargins(0, 0, 0, 0);
    tpl->setSpacing(0);
    // 头部（原型 .chat-head：名＋meta＋「发送文件」brand 钮）
    auto* th = new QWidget(transfer_page_);
    auto* thl = new QHBoxLayout(th);
    thl->setContentsMargins(14, 10, 14, 10);
    thl->setSpacing(10);
    auto* tname = new QLabel(QStringLiteral("文件传输"), th);
    tname->setObjectName(QStringLiteral("tx_title"));
    thl->addWidget(tname);
    tx_meta_ = new QLabel(th);
    thl->addWidget(tx_meta_);
    thl->addStretch();
    // 原型为 SVG 上箭头＋文字；Xvfb 无 emoji 字体，⬆/🔒 类符号渲染成
    // 豆腐块，改用基础箭头字形（DejaVu 全覆盖）
    tx_send_btn_ = new QPushButton(QStringLiteral("↑ 发送文件"), th);
    tx_send_btn_->setObjectName(QStringLiteral("tx_send_btn"));
    tx_send_btn_->setCursor(Qt::PointingHandCursor);
    tx_send_btn_->setToolTip(
        QStringLiteral("发送到当前会话（先在会话中选择对象）"));
    connect(tx_send_btn_, &QPushButton::clicked, this, [this] {
      if (current_peer_.isEmpty()) {
        show_status(QStringLiteral("先在会话中选择对象，再发送文件"));
        return;
      }
      if (current_kind_ == QStringLiteral("group") ||
          current_kind_ == QStringLiteral("dgroup")) {
        show_status(QStringLiteral("群会话暂不支持文件发送（文件走单聊点对点）"));
        return;
      }
      if (current_kind_ != QStringLiteral("collab") && !direct_send_allowed()) {
        return; // T3.4 策略闸门（与发文件同口径）
      }
      const QString path = QFileDialog::getOpenFileName(
          this, QStringLiteral("选择要发送的文件"));
      if (path.isEmpty()) return;
      transfer_send_file(path);
    });
    thl->addWidget(tx_send_btn_);
    tpl->addWidget(th);
    // 标签行（原型 .tx-tabs：全部/传输中 N/已完成/已暂停——已暂停如实
    // 空态，引擎无暂停 API，不造假控件语义）
    auto* tabs = new QWidget(transfer_page_);
    auto* tab_l = new QHBoxLayout(tabs);
    tab_l->setContentsMargins(18, 10, 18, 0);
    tab_l->setSpacing(4);
    const QStringList tab_names = {QStringLiteral("全部"),
                                   QStringLiteral("传输中"),
                                   QStringLiteral("已完成"),
                                   QStringLiteral("已暂停")};
    for (int i = 0; i < 4; ++i) {
      tx_tabs_[i] = new QPushButton(tab_names[i], tabs);
      tx_tabs_[i]->setCursor(Qt::PointingHandCursor);
      tx_tabs_[i]->setFlat(true);
      const int idx = i;
      connect(tx_tabs_[i], &QPushButton::clicked, this, [this, idx] {
        transfer_filter_ = idx;
        refresh_transfers_view();
      });
      tab_l->addWidget(tx_tabs_[i]);
    }
    tab_l->addStretch();
    tpl->addWidget(tabs);
    // 传输卡列表（滚动区）
    auto* scroll = new QScrollArea(transfer_page_);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    auto* list_host = new QWidget(scroll);
    list_host->setStyleSheet(QStringLiteral("background:transparent;"));
    tx_list_lay_ = new QVBoxLayout(list_host);
    tx_list_lay_->setContentsMargins(18, 14, 18, 14);
    tx_list_lay_->setSpacing(10);
    tx_empty_ = new QLabel(list_host);
    tx_empty_->setWordWrap(true);
    tx_list_lay_->addWidget(tx_empty_);
    tx_list_lay_->addStretch();
    scroll->setWidget(list_host);
    tpl->addWidget(scroll, 1);
    // 归档说明卡（原型 tx-note 同位：口径如实——本体不入任何归档，
    // 协作态授权留痕可追溯，直连态完全离线）
    auto* note = new QLabel(
        QStringLiteral("文件本体不经服务端、无归档；协作态发送经服务端授权"
                       "留痕——「谁在什么时间向谁传了哪个文件」可追溯，直连态"
                       "完全离线。"),
        transfer_page_);
    note->setObjectName(QStringLiteral("tx_note"));
    note->setWordWrap(true);
    auto* note_wrap = new QWidget(transfer_page_);
    auto* nwl = new QHBoxLayout(note_wrap);
    nwl->setContentsMargins(18, 0, 18, 16);
    nwl->addWidget(note);
    tpl->addWidget(note_wrap);
  }
  transfer_page_->hide();
  layout->addWidget(transfer_side_);
  layout->addWidget(transfer_page_, 100);

  // —— 组织架构面（原型 desktop-org.html 整屏三栏）：rail「组织架构」切
  //     视图——左＝部门树（搜索＋计数＋可见性虚线卡），中＝部门头＋成员卡
  //     网格，右＝成员档案（汇报链路＋kv＋动作钮）。树/网格/档案随 org
  //     数据与主题全量重建（rebuild_org_view），此处只搭骨架。——
  org_side_ = new QWidget(central);
  org_side_->setObjectName(QStringLiteral("org_side"));
  org_side_->setFixedWidth(240);
  {
    org_tree_lay_ = new QVBoxLayout(org_side_);
    org_tree_lay_->setContentsMargins(8, 10, 6, 10);
    org_tree_lay_->setSpacing(6);
    org_search_ = new QLineEdit(org_side_);
    org_search_->setPlaceholderText(QStringLiteral("搜索部门 / 成员"));
    org_search_->setClearButtonEnabled(true);
    org_tree_lay_->addWidget(org_search_);
    // 树与说明卡由 rebuild_org_view 填充（常驻复用说明卡）
    org_note_ = new QLabel(org_side_);
    org_note_->setObjectName(QStringLiteral("org_note"));
    org_note_->setWordWrap(true);
  }
  org_side_->hide();

  org_page_ = new QWidget(central);
  org_page_->setObjectName(QStringLiteral("org_page"));
  {
    auto* opl = new QVBoxLayout(org_page_);
    opl->setContentsMargins(0, 0, 0, 0);
    opl->setSpacing(0);
    // 部门头（原型 .side-head：部门 · N 人＋负责人胶囊＋「发起群聊」brand 钮）
    auto* oh = new QWidget(org_page_);
    auto* ohl = new QHBoxLayout(oh);
    ohl->setContentsMargins(14, 10, 14, 10);
    ohl->setSpacing(10);
    org_head_title_ = new QLabel(oh);
    org_head_title_->setObjectName(QStringLiteral("org_head_title"));
    ohl->addWidget(org_head_title_);
    org_head_pill_ = new QLabel(oh);
    org_head_pill_->setObjectName(QStringLiteral("org_head_pill"));
    ohl->addWidget(org_head_pill_);
    ohl->addStretch();
    auto* grp_btn = new QPushButton(QStringLiteral("发起群聊"), oh);
    grp_btn->setObjectName(QStringLiteral("org_group_btn"));
    grp_btn->setCursor(Qt::PointingHandCursor);
    connect(grp_btn, &QPushButton::clicked, this,
            [this] { create_group_dialog(); });
    ohl->addWidget(grp_btn);
    opl->addWidget(oh);
    // 成员卡网格（滚动区，两列流）
    auto* oscroll = new QScrollArea(org_page_);
    oscroll->setFrameShape(QFrame::NoFrame);
    oscroll->setWidgetResizable(true);
    auto* ohost = new QWidget(oscroll);
    ohost->setStyleSheet(QStringLiteral("background:transparent;"));
    org_grid_lay_ = new QVBoxLayout(ohost);
    org_grid_lay_->setContentsMargins(14, 12, 14, 12);
    org_grid_lay_->setSpacing(10);
    org_grid_empty_ = new QLabel(ohost);
    org_grid_empty_->setWordWrap(true);
    org_grid_lay_->addWidget(org_grid_empty_);
    org_grid_lay_->addStretch();
    oscroll->setWidget(ohost);
    opl->addWidget(oscroll, 1);
  }
  org_page_->hide();

  org_profile_ = new QWidget(central);
  org_profile_->setObjectName(QStringLiteral("org_profile"));
  org_profile_->setFixedWidth(260);
  {
    org_profile_lay_ = new QVBoxLayout(org_profile_);
    org_profile_lay_->setContentsMargins(14, 18, 14, 14);
    org_profile_lay_->setSpacing(8);
  }
  org_profile_->hide();
  layout->addWidget(org_side_);
  layout->addWidget(org_page_, 100);
  layout->addWidget(org_profile_);
  setCentralWidget(central);

  // 主题：控件样式统一在这里刷（构造时先按当前令牌落一次，之后跟随
  // ThemeManager::theme_changed 重刷，切换即时生效）。
  apply_theme_styles();

  // 需求批⑨查找联系人：可检索字段＝昵称/账号/设备 id/IP/群名/群成员
  // （UserRole+6，\n 分行）；模糊＝子串、精确＝整行相等（contains 超集，
  // 两种输入都命中）；分组头随组内可见项折叠，不留孤悬标题行。
  connect(search_box_, &QLineEdit::textChanged, this, [this](const QString& t) {
    const QString needle = t.trimmed();
    for (int i = 0; i < device_list_->count(); ++i) {
      auto* item = device_list_->item(i);
      const bool selectable =
          !item->data(Qt::UserRole).toString().isEmpty(); // 分组头＝空 id
      if (!selectable) continue; // 头行在第二遍按组内可见性折叠
      const QString hay = item->data(Qt::UserRole + 6).toString();
      const bool hit = needle.isEmpty() ||
                       (!hay.isEmpty() &&
                        hay.contains(needle, Qt::CaseInsensitive));
      item->setHidden(!hit);
    }
    // 第二遍：分组头＝其后到下一分组头之间还有可见项才显示
    for (int i = 0; i < device_list_->count(); ++i) {
      auto* item = device_list_->item(i);
      if (!item->data(Qt::UserRole).toString().isEmpty()) continue;
      bool any_visible = false;
      for (int j = i + 1; j < device_list_->count(); ++j) {
        auto* next = device_list_->item(j);
        if (next->data(Qt::UserRole).toString().isEmpty()) break; // 下一组
        if (!next->isHidden()) { any_visible = true; break; }
      }
      item->setHidden(!needle.isEmpty() && !any_visible);
    }
  });
  // 好友/会话列表：点击打开会话（点开＝面板展开）；再点当前会话项＝
  // 关回列表态（经典 IM 三栏切换）。会话开着与否看状态变量（current_peer_
  // 与面板显隐同步变化，close_chat 清/open_chat 设；离屏未 show 的窗口里
  // 子件 isVisible() 恒 false，不能拿面板显隐当判据）。selectionChanged
  // 键盘导航不打开，回车/双击激活走 itemActivated 同一路径。
  const auto list_open = [this](QListWidgetItem* item) {
    if (!item) return;
    const QString id = item->data(Qt::UserRole).toString();
    if (id.isEmpty()) return; // 分组标题行不可选
    const QString kind = item->data(Qt::UserRole + 1).toString();
    if (!current_peer_.isEmpty() && id == current_peer_ &&
        kind == current_kind_) {
      close_chat(); // 再点当前好友＝回列表态
      return;
    }
    open_chat(kind, id);
  };
  connect(device_list_, &QListWidget::itemClicked, this, list_open);
  connect(device_list_, &QListWidget::itemActivated, this, list_open);
  connect(send_btn_, &QPushButton::clicked, this, [this] {
    const QString text = input_box_->text().trimmed();
    if (text.isEmpty() || current_peer_.isEmpty()) return;
    if (send_in_current_chat(text)) input_box_->clear();
  });
  connect(input_box_, &QLineEdit::returnPressed, this,
          [this] { send_btn_->click(); });
  // —— @ 成员选择器（原型 desktop-group.html .input-toolbar .hint「@ 唤起
  // 成员选择」）——群会话输入出现「@词头」（@ 起至串尾的无空白段）弹成员
  // 菜单（显示名，按词头过滤，≤12 项）；菜单为 Popup 型抢键，键入经
  // eventFilter 转发回输入框（连续输入继续过滤），选中回填「@显示名 」。
  at_menu_ = new QMenu(this);
  at_menu_->installEventFilter(this);
  connect(input_box_, &QLineEdit::textChanged, this, [this](const QString& t) {
    if (!at_menu_) return;
    // 菜单分发中键入会同步触发本 lambda——清 actions 推迟一帧（分发中清
    // 空 QMenu 会悬垂/崩溃），重建与显隐都走 rebuild_at_menu
    if (in_at_key_forward_) return;
    rebuild_at_menu(t);
  });
  connect(file_btn, &QPushButton::clicked, this, [this] {
    if (current_peer_.isEmpty()) {
      show_status(QStringLiteral("先选择设备再发送文件"));
      return;
    }
    if (current_kind_ == QStringLiteral("group") ||
        current_kind_ == QStringLiteral("dgroup")) {
      // 群文件传输不在本期范围：文件仍走点对点直连（单聊）
      show_status(QStringLiteral("群会话暂不支持文件发送（文件走单聊点对点）"));
      return;
    }
    if (current_kind_ != QStringLiteral("collab") &&
        !direct_send_allowed()) { // T3.4 策略闸门（文件与文字同口径）
      return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择要发送的文件"));
    if (path.isEmpty()) return;
    transfer_send_file(path); // 发送缝（传输面「发送文件」钮同一路径）
  });
  // 需求批⑤ 发文件夹：点击＝守卫先行（无会话等场景不应弹目录选择框），
  // 目录选定后走发送缝（与测试同一路径）
  connect(folder_btn, &QPushButton::clicked, this, [this] {
    if (!folder_send_allowed()) return;
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择要发送的文件夹"));
    if (dir.isEmpty()) return;
    send_folder_to_current_chat(dir);
  });
}

void MainWindow::wire_engines() {
  connect(&direct_engine_, &DirectEngine::peers_changed, this, [this] {
    cross_sweep(); // T4.2：对端离线／对端登录 → 跨态会话闭环
    update_banner(); // 跨态判定随宣告刷新（对端登录态变化即时反映）
    refresh_devices();
    show_status(status_hint_); // 在线数变化，刷新引擎态前缀
  });
  connect(&direct_engine_, &DirectEngine::message_received, this,
          [this](const QString& from, const QString& text, qint64 ts_ms,
                 quint64 seq) {
            cross_touch(from); // T4.2：首条收发即上报会话建立
            if (from == current_peer_ &&
                current_kind_ == QStringLiteral("direct")) {
              append_message(from, text, ts_ms, false,
                             QStringLiteral("direct"));
              // 已读上报（需求批⑦·已读灰勾单）：会话开着即视为已览，
              // 是否上报受会话/全局已读回执开关裁决（与协作态同款）
              if (seq != 0 && read_receipts_enabled(from))
                direct_engine_.mark_read(from.toStdString(), seq);
            } else {
              // 未读角标（原型行 n-count）：非当前会话到达即计数并重渲行
              ++unread_counts_[from];
              refresh_devices();
              show_status(QStringLiteral("来自 %1 的新消息").arg(from));
            }
          });
  // T4.7 系统通知独立接入（与渲染分支解耦：窗口未激活时任何直连新消息
  // 都通知，不随分支改写而丢失）
  connect(&direct_engine_, &DirectEngine::message_received, this,
          [this](const QString& from, const QString& text, qint64, quint64) {
            if (!isActiveWindow()) {
              event_notify(NotifyPrefs::load().popup_message,
                           SoundEvent::Message, QStringLiteral("新消息"),
                           QStringLiteral("来自 %1：%2").arg(from, text));
              alert_attention(); // 任务栏/窗口闪烁（开关与合并窗在实现内）
            }
          });
  connect(&direct_engine_, &DirectEngine::text_delivered, this,
          [this](quint64 seq, bool ok) {
            set_delivery_state(
                ok ? QStringLiteral("已送达（seq %1）").arg(seq)
                   : QStringLiteral("送达失败（seq %1）").arg(seq));
            // 直连送达回执落气泡（需求批⑦·已读灰勾单）：store 侧由引擎
            // 落 delivered，界面按发送序号命中行改灰单勾（失败不冒充送达）
            if (ok) apply_direct_receipt(seq, QStringLiteral("delivered"));
          });
  // 直连已读上报回执（需求批⑦·已读灰勾单）：store 侧由引擎落 read
  //（只升不降），界面把灰单勾升为灰双勾；状态行同协作态口径提示
  connect(&direct_engine_, &DirectEngine::message_read, this,
          [this](quint64 seq, const QString& reader) {
            apply_direct_receipt(seq, QStringLiteral("read"));
            set_delivery_state(QStringLiteral("对方已读 ✓✓（%1）").arg(reader));
            // 「盯」：READ 落终态即查盯住集合并触发（无盯则静默穿过）
            watch_read_fired(direct_watch_key(reader, seq), reader);
          });
  connect(&direct_engine_, &DirectEngine::file_progress, this,
          [this](const QString& id, quint64 done, quint64 total) {
            update_tx_progress(id, done, total); // 传输台账（文件传输面）
            if (total == 0) return;
            // 状态栏节流：按 5% 步进刷新
            const int pct = static_cast<int>(done * 100 / total);
            static int last_pct = -1;
            if (pct / 5 != last_pct / 5 || pct == 100) {
              last_pct = pct;
              show_status(QStringLiteral("文件传输进度：%1%").arg(pct));
            }
          });
  // 振屏（需求批⑥）：直连域到达——抖窗＋提示音，当前会话渲染系统行
  connect(&direct_engine_, &DirectEngine::nudge_received, this,
          [this](const QString& from, quint64 /*seq*/, qint64 ts_ms) {
            on_nudge_received(from, ts_ms, QStringLiteral("direct"));
          });
  connect(&direct_engine_, &DirectEngine::file_finished, this,
          [this](const QString& id, bool ok, const QString& error) {
            last_file_error_ = error; // 授权回执验收面（失败原因留存可断言）
            finish_tx_entry(id, ok, error); // 传输台账（文件传输面）
            show_status(ok ? QStringLiteral("文件已送达")
                           : QStringLiteral("文件传输中断：%1").arg(error));
            // 文件传输成功→弹通知窗（事件开关组；默认关＝成功是常态）
            if (ok) {
              event_notify(NotifyPrefs::load().popup_transfer_done,
                           SoundEvent::File, QStringLiteral("文件传输成功"),
                           id.right(8));
            }
            // T4.4：截图临时文件在传输结束后清理（成功／失败均清）
            const auto shot = shot_paths_.constFind(id);
            if (shot != shot_paths_.cend()) {
              QFile::remove(shot.value());
              shot_paths_.erase(shot);
            }
          });
  // 需求批⑤ 文件夹作业聚合：文件粒度计数刷状态（引擎逐文件串行）
  connect(&direct_engine_, &DirectEngine::directory_progress, this,
          [this](const QString& job_id, quint64 done, quint64 total) {
            const auto it = folder_jobs_.find(job_id);
            if (it == folder_jobs_.end()) return;
            it->done = done;
            it->total = total;
            // 传输台账（文件传输面）：文件夹作业按文件粒度计数
            for (TransferEntry& e : transfers_) {
              if (e.id == job_id && e.is_folder) {
                e.files_done = done;
                e.files_total = total;
                break;
              }
            }
            if (sidebar_mode_ == QStringLiteral("transfers"))
              refresh_transfers_view();
            show_status(QStringLiteral("文件夹传输：%1（第 %2/%3 个文件）")
                            .arg(it->name, QString::number(done + 1),
                                 QString::number(total)));
          });
  connect(&direct_engine_, &DirectEngine::directory_finished, this,
          [this](const QString& job_id, bool ok) {
            const auto it = folder_jobs_.find(job_id);
            const QString name = it == folder_jobs_.end() ? job_id : it->name;
            folder_jobs_.remove(job_id);
            finish_tx_entry(job_id, ok, QString()); // 传输台账（文件传输面）
            if (ok) {
              append_system_line(
                  QStringLiteral("[文件夹] %1 发送完成").arg(esc(name)));
              show_status(QStringLiteral("文件夹发送完成：%1").arg(name));
            } else {
              show_status(
                  QStringLiteral(
                      "文件夹发送失败：%1（已传部分保留，重发自动续传）")
                      .arg(name));
            }
          });
  connect(&direct_engine_, &DirectEngine::file_received, this,
          [this](const QString& id, const QString& peer_id,
                 const QString& path) {
            last_received_file_ =
                peer_id + QLatin1Char('|') + path; // 验收面
            // 传输台账（文件传输面）：收侧落账＝已完成（接收端整文件
            // SHA-256 校验通过才落盘，foot 如实标校验）
            {
              const Peer rp = direct_engine_.peer(peer_id.toStdString());
              const bool via_collab = !rp.account.empty();
              TransferEntry e;
              e.id = id;
              e.name = QFileInfo(path).fileName();
              e.kind = via_collab ? QStringLiteral("collab")
                                  : QStringLiteral("direct");
              e.peer = via_collab
                           ? QString::fromStdString(rp.account)
                           : peer_id;
              e.path = path;
              e.bytes_done = e.bytes_total =
                  static_cast<quint64>(QFileInfo(path).size());
              const qint64 now = QDateTime::currentMSecsSinceEpoch();
              e.ts_ms = now;
              e.done_ms = now;
              e.state = 1; // 已完成
              e.outgoing = false;
              e.peer_display = tx_peer_display(e.kind, e.peer);
              track_transfer(e);
            }
            // 图片消息（需求批④）：图片扩展名且属于当前会话＝气泡内直接
            // 渲染图片；否则按普通文件系统行（口径不混淆）。
            // 会话归属按发送方设备号（peer_id）判，transfer id 是 UUID
            // 不指向任何会话。
            static const QStringList kImageExt = {
                QStringLiteral("png"), QStringLiteral("jpg"),
                QStringLiteral("jpeg"), QStringLiteral("gif"),
                QStringLiteral("bmp"), QStringLiteral("webp")};
            const bool is_image =
                kImageExt.contains(QFileInfo(path).suffix().toLower());
            bool in_session = false;
            if (is_image) {
              if (current_kind_ == QStringLiteral("direct")) {
                in_session = (peer_id == current_peer_);
              } else if (current_kind_ == QStringLiteral("collab")) {
                const Peer p = direct_engine_.peer(peer_id.toStdString());
                in_session =
                    !p.account.empty() &&
                    QString::fromStdString(p.account) == current_peer_;
              }
            }
            if (in_session) {
              append_image_message(id, path,
                                   QDateTime::currentMSecsSinceEpoch(),
                                   /*outgoing=*/false,
                                   QStringLiteral("direct"));
              show_status(QStringLiteral("图片消息已接收：%1").arg(path));
            } else {
              append_file_card(
                  id, QFileInfo(path).fileName(),
                  QStringLiteral("%1 · 直传 · 已接收")
                      .arg(human_size(QFileInfo(path).size())),
                  QDateTime::currentMSecsSinceEpoch(),
                  /*outgoing=*/false);
              show_status(QStringLiteral("文件已接收：%1").arg(path));
            }
            // 收到文件→弹通知窗（与消息同款激活门：激活中不打扰）
            if (!isActiveWindow()) {
              event_notify(NotifyPrefs::load().popup_file_arrive,
                           SoundEvent::File,
                           is_image ? QStringLiteral("收到图片")
                                    : QStringLiteral("收到文件"),
                           QFileInfo(path).fileName());
            }
          });

  // 平台-10 文件旁路授权门（蓝图§十九）：文件不经服务器，判权必须经
  // 服务器——发送前先问服务端统一 AuthorizationService（四问：可发/
  // 可收/跨部门/再转发），裁决回流关单。协作未登录＝服务端不可达域，
  // 协作引擎本地即拒（fail-closed：文本是归档面可降级，文件是权限面
  // 不可绕权）。
  direct_engine_.set_file_authorizer(
      [this](const memex::client::FileAuthzRequest& r) {
        collab_engine_.file_authz(r.req, QString::fromStdString(r.to_account),
                                  r.size, QString::fromStdString(r.name),
                                  QString(), r.forward);
      });
  connect(&collab_engine_, &CollabEngine::file_authz_result, this,
          [this](quint64 req, bool allowed, const QString& reason, bool) {
            direct_engine_.file_authz_resolved(req, allowed, reason);
          });
}

// 协作信号接入界面（T2.4）：形态切换提示、降级提示、协作消息渲染。
// 降级口径：服务端不可达＝回落直连态，必须明示「消息不进归档」（A11）。
void MainWindow::wire_collab() {
  connect(&collab_engine_, &CollabEngine::logged_in, this,
          [this](const QString& account, const QString& display) {
            // T4.2：本端登录态写进发现宣告——对端据此判定跨态与显示账号
            direct_engine_.set_collab_account(account.toStdString());
            collab_login_ms_ = QDateTime::currentMSecsSinceEpoch(); // A8 归档起点
            act_collab_logout_->setEnabled(true);
            collab_was_logged_in_ = true;
            kick_text_.clear(); // T4.3：新会话清除旧互踢提示
            collab_degraded_ = false; // 平台-7：登录恢复即退出降级态
            // 在线表以服务端推送为准（登录广播先于 LOGIN_RESULT 到达，
            // 推送已含自己——此处不清，否则会擦掉刚收到的推送）。
            seed_collab_peers();
            update_banner();
            refresh_devices();
            append_system_line(
                QStringLiteral("已切换协作态（%1）：此后消息经服务端转发并"
                               "全量归档")
                    .arg(esc(display)));
            if (current_kind_ == QStringLiteral("direct")) {
              // A8 客户端面：跨态直连会话归档自行（此前直连消息不进归档）
              append_system_line(QStringLiteral(
                  "归档自 %1（本次协作态登录时刻）：此前跨态直连消息不进"
                  "归档，转协作会话后自登录时刻起计入")
                      .arg(QDateTime::currentDateTime().toString(
                          QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
            }
            show_status(QStringLiteral("协作态已登录：%1").arg(display));
            // 登录即拉组织架构（策略与建群数据源）与群列表（T3.4／T4.1）
            collab_engine_.query_org();
            collab_engine_.query_groups();
            // 需求批⑫：文件面同源自动登录（口令复用；端口走 QSettings，
            // 未配置＝静默跳过——头像操作时再明示失败）
            files_ensure_login();
          });
  connect(&collab_engine_, &CollabEngine::login_failed, this,
          [this](const QString& reason) {
            act_collab_logout_->setEnabled(false);
            if (reason == QStringLiteral("无法连接服务器")) {
              // 平台-7 降级：登录尝试因服务端不可达而失败＝本应归档而
              // 不得（用户意图是协作态）——与曾否登录无关；显式登出回
              // 直连不算降级（logout_collab 清标志）。标志先置再刷横幅
              collab_degraded_ = true;
              update_banner();
              append_system_line(QStringLiteral(
                  "服务端不可达：已回落直连态，消息不进归档"
                  "（点对点传输，仍可正常收发）"));
              show_status(
                  QStringLiteral("服务端不可达，已回落直连态：消息不进归档"));
            } else {
              update_banner();
              show_status(QStringLiteral("协作登录失败：%1").arg(reason));
            }
          });
  connect(&collab_engine_, &CollabEngine::connection_lost, this, [this] {
    // 平台-7：登录中断线＝DEGRADED（本应归档而服务端不可达）；被顶下线
    // （kicked 先行清了登录态）与服务端无涉，不算降级
    if (collab_was_logged_in_) collab_degraded_ = true;
    collab_was_logged_in_ = false;
    online_accounts_.clear(); // 断线即未知在线态（重连推送后刷新）
    update_banner();
    refresh_devices();
    show_status(QStringLiteral(
        "服务端连接断开，直连态仍可用：消息不进归档（自动重连中）"));
  });
  connect(&collab_engine_, &CollabEngine::reconnected, this, [this] {
    collab_was_logged_in_ = true;
    update_banner();
    refresh_devices();
    show_status(QStringLiteral("协作态已恢复：消息重新进入归档"));
  });
  connect(&collab_engine_, &CollabEngine::kicked, this,
          [this](const QString& reason, const QString& replaced_by) {
            cross_end_all(); // T4.2：被踢即会话终点（此刻引擎尚未断开，
                             // CROSS_LOG 帧随断开前队列刷出）
            collab_was_logged_in_ = false;
            online_accounts_.clear(); // 在线表随会话失效（重登后推送刷新）
            act_collab_logout_->setEnabled(false);
            // kicked 信号先于引擎 logged_in_=false 到达：横幅与列表刷新
            // 排队到状态落定后（否则仍按登录态渲染协作横幅，状态说谎）。
            QMetaObject::invokeMethod(
                this,
                [this] {
                  update_banner();
                  refresh_devices();
                },
                Qt::QueuedConnection);
            // T4.3 桌面单点在线提示：常驻文案（测试断言面）＋非模态弹窗
            //（模态会阻塞自动化验收；关闭按钮常驻可查）。
            kick_text_ = QStringLiteral("已被顶替下线（%1）：%2")
                             .arg(replaced_by, reason);
            set_delivery_state(kick_text_);
            // T4.7：互踢同时走系统通知（托盘气泡；无托盘仅记录不断言面）
            tray_notify(QStringLiteral("已被顶替下线"), kick_text_);
            auto* box = new QMessageBox(
                QMessageBox::Warning, QStringLiteral("已被顶替下线"),
                kick_text_ + QStringLiteral("\n同账号在另一台设备登录，"
                                            "本机会话已断开（消息不进归档）。"),
                QMessageBox::Ok, this);
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->setModal(false);
            box->show();
          });
  connect(&collab_engine_, &CollabEngine::message_received, this,
          [this](const QString& from, const QString& text, qint64 ts_ms,
                 const QString& msg_id) {
            if (current_kind_ == QStringLiteral("collab") &&
                from == current_peer_) {
              // 本地库已由引擎落库（同库），这里只做界面渲染
              append_message(from, text, ts_ms, false,
                             QStringLiteral("collab"));
              // T4.3：当前会话开着=已读，立即上报（发送方收 READ_NOTICE）；
              // 需求批⑦：是否上报受会话/全局已读回执开关裁决
              if (!msg_id.isEmpty() && read_receipts_enabled(from))
                collab_engine_.mark_read(msg_id);
            } else {
              // 未读角标（非当前会话；本分支尾统一 refresh_devices 重渲行）
              ++unread_counts_[from];
              show_status(QStringLiteral("来自 %1 的协作消息").arg(from));
            }
            collab_peers_.insert(from);
            refresh_devices();
          });
  // T4.7 系统通知独立接入（与渲染分支解耦：窗口未激活时任何协作新消息
  // 都通知，不随分支改写而丢失）
  connect(&collab_engine_, &CollabEngine::message_received, this,
          [this](const QString& from, const QString&, qint64, const QString&) {
            // T4.10：通知（发送方＝kNoticeSender）由通知中心分级自管，不在此
            // 重复托盘气泡（普通默认不弹，重要／紧急由通知中心直发）
            if (from ==
                QString::fromUtf8(memex::protocol::kNoticeSender)) {
              return;
            }
            if (!isActiveWindow()) {
              event_notify(NotifyPrefs::load().popup_message,
                           SoundEvent::Message, QStringLiteral("新消息"),
                           QStringLiteral("来自 %1 的协作消息").arg(from));
              alert_attention();
            }
          });
  connect(&collab_engine_, &CollabEngine::text_delivered, this,
          [this](quint64 seq, bool ok) {
            // 已读是终态：后到的受理回执不回退状态（自发自收回环时先已读后回执）
            if (delivery_text_.contains(QStringLiteral("已读"))) return;
            set_delivery_state(
                ok ? QStringLiteral("协作消息已送达（seq %1）").arg(seq)
                   : QStringLiteral("协作消息送达超时（seq %1）").arg(seq));
          });
  // 振屏（需求批⑥）：协作域到达——抖窗＋提示音，当前会话渲染系统行
  connect(&collab_engine_, &CollabEngine::nudge_received, this,
          [this](const QString& from, qint64 ts_ms) {
            on_nudge_received(from, ts_ms, QStringLiteral("collab"));
          });
  connect(&collab_engine_, &CollabEngine::org_received, this,
          [this](const QString& org_json) {
            last_org_json_ = org_json;
            // 需求批⑪：成员签名入缓存（会话列表悬浮 tooltip 数据源）
            member_signatures_.clear();
            // 需求批⑩：成员在线时长入缓存（tooltip 与个人资料展示面）
            member_online_.clear();
            // 需求批⑫：成员头像版本戳入缓存（0=未设置——默认头像；ver 变
            // 即排队下载 256 档，本地缩放缓存四档）
            member_avatar_ver_.clear();
            const nlohmann::json j = nlohmann::json::parse(
                org_json.toStdString(), nullptr, false);
            if (!j.is_discarded() && j.contains("members")) {
              for (const auto& m : j["members"]) {
                const QString a = QString::fromStdString(
                    m.value("account", std::string{}));
                if (!a.isEmpty()) {
                  member_signatures_.insert(
                      a, QString::fromStdString(
                             m.value("signature", std::string{})));
                  member_online_.insert(
                      a, QStringLiteral("在线时长：今日 %1 · 本周 %2 · 本月 %3")
                             .arg(format_online_s(m.value("online_day_s", 0)),
                                  format_online_s(m.value("online_week_s", 0)),
                                  format_online_s(
                                      m.value("online_month_s", 0))));
                  member_avatar_ver_.insert(a, m.value("avatar_ver", 0));
                }
              }
            }
            for (auto it = member_avatar_ver_.constBegin();
                 it != member_avatar_ver_.constEnd(); ++it) {
              queue_avatar_download(it.key(), it.value());
            }
            apply_policy(org_json);
            refresh_devices(); // 签名 tooltip 随数据到达即时刷新
            if (org_dialog_pending_) org_dialog_pending_ = false;
            if (sidebar_mode_ == QStringLiteral("org")) {
              rebuild_org_view(); // 组织架构面在屏：随数据即时刷新
            } else {
              show_status(QStringLiteral("组织架构已更新"));
            }
          });
  connect(&collab_engine_, &CollabEngine::fav_received, this,
          [this](const QString& fav_json) { apply_favs(fav_json); });
  // 需求批⑪：签名回执——成功即重拉组织架构（缓存与悬浮 tooltip 刷新）
  connect(&collab_engine_, &CollabEngine::profile_result, this,
          [this](bool ok, const QString& reason, const QString&) {
            if (ok) {
              collab_engine_.query_org();
              show_status(QStringLiteral("个性签名已保存"));
            } else {
              show_status(QStringLiteral("签名设置失败（%1）").arg(reason));
            }
          });
  // —— T4.1 群聊 ——
  connect(&collab_engine_, &CollabEngine::group_result, this,
          [this](bool ok, const QString& reason, const QString& op,
                 quint64 group_id) {
            if (ok) {
              show_status(QStringLiteral("群操作成功（%1，群 %2）")
                              .arg(op, QString::number(group_id)));
              // 群组-1：资料（主题/目的）落定后重拉群列表对齐 meta 展示
              if (op == QStringLiteral("profile")) {
                collab_engine_.query_groups();
              }
            } else {
              show_status(QStringLiteral("群操作失败（%1）：%2").arg(op, reason));
            }
          });
  connect(&collab_engine_, &CollabEngine::groups_received, this,
          [this](const QString& groups_json) { apply_groups(groups_json); });
  connect(&collab_engine_, &CollabEngine::group_message_received, this,
          [this](const QString& group_key, const QString& sender,
                 const QString& text, qint64 ts_ms, const QString& msg_id) {
            if (current_kind_ == QStringLiteral("group") &&
                group_key == current_peer_) {
              append_message(sender, text, ts_ms, false,
                             QStringLiteral("collab"));
              // T4.3：群会话开着=已读（发送方按读者逐条收 READ_NOTICE）；
              // 需求批⑦：开关裁决同单聊
              if (!msg_id.isEmpty() && read_receipts_enabled(group_key))
                collab_engine_.mark_read(msg_id);
            } else {
              // 未读角标：群键计数（打开该群即清）
              ++unread_counts_[group_key];
              refresh_devices();
              const QString gname = groups_.value(group_key.mid(6).toULongLong())
                                        .name;
              show_status(QStringLiteral("来自群「%1」%2 的消息")
                              .arg(gname.isEmpty() ? group_key : gname, sender));
            }
          });
  // T4.7 系统通知独立接入（与渲染分支解耦：窗口未激活时任何群新消息
  // 都通知，不随分支改写而丢失）
  connect(&collab_engine_, &CollabEngine::group_message_received, this,
          [this](const QString& group_key, const QString& sender,
                 const QString&, qint64, const QString&) {
            // T4.10：群通知同样由通知中心分级自管（不重复托盘气泡）
            if (sender ==
                QString::fromUtf8(memex::protocol::kNoticeSender)) {
              return;
            }
            if (!isActiveWindow()) {
              const QString gname =
                  groups_.value(group_key.mid(6).toULongLong()).name;
              event_notify(NotifyPrefs::load().popup_message,
                           SoundEvent::Message, QStringLiteral("新消息"),
                           QStringLiteral("来自群「%1」%2 的消息")
                               .arg(gname.isEmpty() ? group_key : gname,
                                    sender));
              alert_attention();
            }
          });
  // —— T4.10 分级推送接入：通知中心按个人偏好裁决普通／重要／紧急三级
  // （普通＝仅站内消息已渲染；重要＝托盘强提醒；紧急＝置顶确认弹窗）——
  connect(&collab_engine_, &CollabEngine::notice_received,
          &NotificationCenter::instance(), &NotificationCenter::on_notice);
  connect(&NotificationCenter::instance(),
          &NotificationCenter::want_tray_notify, this,
          [this](const QString& title, const QString& text) {
            tray_notify(title, text); // 通知中心裁决出的强提醒，不看激活态
          });

  // —— T4.3 已读回执与在线状态 ——
  connect(&collab_engine_, &CollabEngine::message_read, this,
          [this](const QString& msg_id, const QString& reader,
                 qint64 /*read_ms*/) {
            // READ_NOTICE 只发给原发送方：在此处即「对方已读我发出的消息」
            set_delivery_state(
                QStringLiteral("对方已读 ✓✓（%1）").arg(reader));
            // 需求批⑦：消息旁状态可见——落行重渲（引擎已落本地库，
            // 历史重载同构）
            apply_message_receipt(msg_id, QStringLiteral("read"));
            // 「盯」：协作域同口径（c:<msg_id> 键触发）
            watch_read_fired(collab_watch_key(msg_id), reader);
          });
  // —— 需求批⑦ 送达级回执：状态行＋消息旁 ✓ ——
  connect(&collab_engine_, &CollabEngine::message_delivered, this,
          [this](const QString& msg_id, const QString& delivered_to,
                 qint64 /*delivered_ms*/) {
            set_delivery_state(
                QStringLiteral("已送达对方 ✓（%1）").arg(delivered_to));
            apply_message_receipt(msg_id, QStringLiteral("delivered"));
          });
  // —— 需求批⑦ 回执态补查回包：逐条对齐当前会话的行 ——
  connect(&collab_engine_, &CollabEngine::receipts_received, this,
          [this](const QString& receipts_json) {
            const QJsonDocument doc = QJsonDocument::fromJson(
                receipts_json.toUtf8());
            if (!doc.isArray()) return;
            for (const QJsonValue& v : doc.array()) {
              const QJsonObject e = v.toObject();
              const QString mid = e.value(QStringLiteral("msg_id")).toString();
              if (mid.isEmpty()) continue;
              const bool read = e.value(QStringLiteral("readers"))
                                    .toArray().isEmpty() == false;
              const bool delivered = e.value(QStringLiteral("delivered_to"))
                                         .toArray().isEmpty() == false;
              if (!read && !delivered) continue; // 未送达不标注
              apply_message_receipt(
                  mid, read ? QStringLiteral("read")
                            : QStringLiteral("delivered"));
              // 「盯」：补拉对齐出的已读同样触发（离线期 READ 的兜底面）
              if (read) watch_read_fired(collab_watch_key(mid));
            }
          });
  connect(&collab_engine_, &CollabEngine::presence_changed, this,
          [this](const QStringList& accounts) {
            // 联系人上线通知（用户令 2026-10-08）：推送前对比新增账号，
            // 逐个按开关弹（自己也在广播里，排除）
            const QStringList joined =
                presence_joined(online_accounts_.values(), accounts);
            // 逐个插入（不依赖 QSet 区间构造的可移植性）
            online_accounts_.clear();
            for (const QString& a : accounts) online_accounts_.insert(a);
            const QString me = collab_engine_.account();
            const NotifyPrefs p = NotifyPrefs::load();
            for (const QString& a : joined) {
              if (a == me) continue;
              event_notify(p.popup_peer_online, SoundEvent::Online,
                           QStringLiteral("联系人上线"), a);
            }
            refresh_devices(); // 协作会话行在线标识随推送刷新
          });
  // —— 需求批⑧ 日期范围补拉回包：落库已完成（引擎），此处只管重渲 ——
  connect(&collab_engine_, &CollabEngine::history_received, this,
          [this](const QString& peer, qint64, qint64, int) {
            if (peer != current_peer_ || chat_panel_->isHidden()) return;
            if (filter_from_ms_ > 0 || filter_until_ms_ > 0) {
              render_filtered_history(); // 筛选窗内新行并入
              return;
            }
            if (pending_jump_.isValid()) {
              render_open_history(); // 跳转日数据到齐——重渲后再定位
              scroll_to_day(pending_jump_);
            }
            // 既不筛选也无挂起跳转：补拉行静默落库（下次打开/筛选可见）
          });
}

// —— T2.4 模式切换公共入口 ——

void MainWindow::login_collab(const QString& host, quint16 port,
                              const QString& account,
                              const QString& password) {
  // 域名解析覆盖（部署批 424）：命中规则回 IP，未命中原样；collab_host_
  // 存已解析值，文件面同源回退（空 collab_host_ 读设置处）不再二次处理
  const QString resolved = resolve_conn_host(host);
  collab_host_ = resolved; // 需求批⑫：文件面同源主机默认（端口走 QSettings）
  collab_engine_.login(resolved, port, account, password);
}

void MainWindow::logout_collab() {
  cross_end_all();          // T4.2：登出前闭环全部跨态会话（end 帧先于 LOGOUT）
  collab_engine_.logout();
  direct_engine_.set_collab_account(""); // 登出即广播「未登录」（对端即时闭环）
  act_collab_logout_->setEnabled(false);
  collab_was_logged_in_ = false;
  collab_degraded_ = false; // 平台-7：显式登出＝主动回直连，非降级
  online_accounts_.clear(); // T4.3：登出即未知在线态
  delivery_text_.clear();   // T4.3：发送状态随会话失效
  // 需求批⑫：文件面会话随登出作废（在途头像挂起态一并清）
  if (files_client_) files_client_->logout();
  avatar_pick_pending_ = false;
  avatar_pending_accounts_.clear();
  update_banner();
  append_system_line(QStringLiteral(
      "已登出协作态，回到直连态：消息不进归档（本地历史保留，合并展示）"));
  show_status(QStringLiteral("协作态已登出：当前直连态，消息不进归档"));
  refresh_devices();
}

void MainWindow::open_collab_peer(const QString& account) {
  if (account.isEmpty()) return;
  if (account.startsWith(QStringLiteral("group:"))) { // 历史群会话键
    open_group(account);
    return;
  }
  collab_peers_.insert(account);
  refresh_devices();
  open_chat(QStringLiteral("collab"), account);
}

bool MainWindow::send_in_current_chat(const QString& text) {
  if (text.isEmpty() || current_peer_.isEmpty()) return false;
  // 协作单聊与服务端群聊同一口径：to=账号或 "group:<群号>"，均经服务端归档
  if (current_kind_ == QStringLiteral("collab") ||
      current_kind_ == QStringLiteral("group")) {
    const quint64 seq = collab_engine_.send_text(current_peer_, text);
    if (seq == 0) {
      show_status(QStringLiteral(
          "发送失败：协作态未登录（可先登录协作态；当前消息不进归档）"));
      return false;
    }
    // 需求批⑦：发出即带 msg_id（与服务端同式派生）——送达/已读回执
    // 通知按它命中本行更新气泡旁状态
    append_message(collab_engine_.account(), text,
                   QDateTime::currentMSecsSinceEpoch(), true,
                   QStringLiteral("collab"),
                   CollabEngine::msg_id_for(collab_engine_.account(), seq));
    set_delivery_state(QStringLiteral("发送中…（seq %1）").arg(seq));
    return true;
  }
  // 免服务端临时群（直连态）：逐设备点对点扇出，不进归档
  if (current_kind_ == QStringLiteral("dgroup")) {
    if (!direct_send_allowed()) return false; // T3.4 策略闸门（与直连同口径）
    const QStringList members = dgroup_members_.value(current_peer_);
    int sent = 0;
    for (const QString& dev : members) {
      if (direct_engine_.send_text(dev.toStdString(), text.toStdString()) > 0) {
        ++sent;
      }
    }
    if (sent == 0) {
      show_status(QStringLiteral("临时群发送失败：无可达成员"));
      return false;
    }
    append_message(QString::fromStdString(direct_engine_.device_id()), text,
                   QDateTime::currentMSecsSinceEpoch(), true,
                   QStringLiteral("direct"));
    if (sent < members.size()) {
      show_status(QStringLiteral("临时群部分送达：%1/%2 台设备")
                      .arg(sent)
                      .arg(members.size()));
    }
    set_delivery_state(QStringLiteral("临时群已扇出（%1/%2 台设备）")
                           .arg(sent)
                           .arg(members.size()));
    return true;
  }
  if (!direct_send_allowed()) return false; // T3.4 策略闸门
  const std::string peer = current_peer_.toStdString();
  // seq 一次受理：0＝对端不可达（不落行不冒充已发送）；>0＝已受理落库
  //（送达失败仍保留本机记录），seq 随行——直连回执按它命中本行
  const quint64 sent_seq = direct_engine_.send_text(peer, text.toStdString());
  if (sent_seq == 0) {
    show_status(QStringLiteral("发送失败：对端不可达"));
    return false;
  }
  cross_touch(current_peer_); // T4.2：跨态会话首触即上报建立
  // T4.5：登录态直连会话的最近联系上报（peer=设备标识，与服务端群/账号同表）
  if (collab_engine_.is_logged_in()) {
    collab_engine_.fav_cmd(QStringLiteral("touch"), current_peer_);
  }
  append_message(QString::fromStdString(direct_engine_.device_id()), text,
                 QDateTime::currentMSecsSinceEpoch(), true,
                 QStringLiteral("direct"), QString(), QString(), sent_seq);
  set_delivery_state(QStringLiteral("发送中…（直连点对点）"));
  return true;
}

void MainWindow::show_emoji_panel() {
  auto* dlg = new QDialog(this, Qt::Popup);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowTitle(QStringLiteral("表情"));
  auto* grid = new QGridLayout(dlg);
  grid->setContentsMargins(8, 8, 8, 8);
  grid->setHorizontalSpacing(4);
  grid->setVerticalSpacing(4);
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  QStringList builtin;
  builtin << QStringLiteral("😀") << QStringLiteral("😄") << QStringLiteral("😂")
          << QStringLiteral("🙂") << QStringLiteral("😉") << QStringLiteral("😊")
          << QStringLiteral("😍") << QStringLiteral("😘") << QStringLiteral("😎")
          << QStringLiteral("🤔") << QStringLiteral("😅") << QStringLiteral("😢")
          << QStringLiteral("😭") << QStringLiteral("😡") << QStringLiteral("👍")
          << QStringLiteral("👎") << QStringLiteral("👏") << QStringLiteral("🙏")
          << QStringLiteral("💪") << QStringLiteral("🎉") << QStringLiteral("❤️")
          << QStringLiteral("🔥") << QStringLiteral("✅") << QStringLiteral("❌");
  // 常用＝按本地使用频次降序（T4.5）
  std::sort(builtin.begin(), builtin.end(), [&settings](const QString& a, const QString& b) {
    return settings.value(QStringLiteral("emoji_use/") + a, 0).toInt() >
           settings.value(QStringLiteral("emoji_use/") + b, 0).toInt();
  });
  int i = 0;
  for (const QString& e : builtin) {
    auto* b = new QPushButton(e, dlg);
    b->setFixedSize(34, 34);
    b->setStyleSheet(QStringLiteral("QPushButton{border:none;font-size:18px;}"));
    connect(b, &QPushButton::clicked, this, [this, e, dlg] {
      input_box_->insert(e);
      input_box_->setFocus();
      // QSettings 不可拷贝，原实现按引用捕获 show_emoji_panel 的栈对象——
      // 面板常驻期间该引用已悬垂，点内置表情即 UB（BUG-003 排查抓到）；
      // 用时重建（组织/应用名与建面板排序处同口径）
      QSettings settings(QCoreApplication::organizationName(),
                         QCoreApplication::applicationName());
      const QString key = QStringLiteral("emoji_use/") + e;
      settings.setValue(key, settings.value(key, 0).toInt() + 1);
      dlg->close();
    });
    grid->addWidget(b, i / 8, i % 8);
    ++i;
  }
  // 图标组（需求批③）：符号图标插入消息——与表情面板同源（②表情包面板
  // 后续扩展即此面板），简版先行：点击即入输入框光标处，不记频次
  {
    const QStringList icons = {
        QStringLiteral("⚠"),  QStringLiteral("☎"),  QStringLiteral("✉"),
        QStringLiteral("🔒"), QStringLiteral("🔑"), QStringLiteral("📅"),
        QStringLiteral("📌"), QStringLiteral("📎"), QStringLiteral("⏰"),
        QStringLiteral("⭐"), QStringLiteral("💡"), QStringLiteral("🚀"),
        QStringLiteral("➤"),  QStringLiteral("✔"),  QStringLiteral("✘"),
        QStringLiteral("★")};
    const int label_row = (i + 7) / 8; // 空满当前行，标签整行占位
    auto* label =
        new QLabel(QStringLiteral("—— 图标（插入消息） ——"), dlg);
    grid->addWidget(label, label_row, 0, 1, 8);
    int r = label_row + 1;
    int c = 0;
    for (const QString& ic : icons) {
      auto* b = new QPushButton(ic, dlg);
      b->setFixedSize(34, 34);
      b->setStyleSheet(
          QStringLiteral("QPushButton{border:none;font-size:17px;}"));
      connect(b, &QPushButton::clicked, this, [this, ic, dlg] {
        input_box_->insert(ic);
        input_box_->setFocus();
        dlg->close();
      });
      grid->addWidget(b, r, c);
      if (++c >= 8) { c = 0; ++r; }
    }
    i = r * 8 + c; // 自定义表情从新行接排
  }
  // 自定义表情包：emoji 目录（emoji_dir，测试缝可覆盖）下的图片，
  // 点击即按文件通道发送
  const QString dir = emoji_dir();
  QDir().mkpath(dir);
  const QStringList files = QDir(dir).entryList(
      {QStringLiteral("*.png"), QStringLiteral("*.jpg"),
       QStringLiteral("*.jpeg"), QStringLiteral("*.gif")},
      QDir::Files);
  int row = i / 8;
  int col = i % 8;
  for (const QString& f : files) {
    auto* b = new QPushButton(dlg);
    b->setFixedSize(40, 40);
    b->setIcon(QIcon(dir + QLatin1Char('/') + f));
    b->setIconSize(QSize(36, 36));
    const QString path = dir + QLatin1Char('/') + f;
    connect(b, &QPushButton::clicked, this, [this, path, dlg] {
      dlg->close();
      if (current_peer_.isEmpty()) {
        show_status(QStringLiteral("先选择会话再发送自定义表情"));
        return;
      }
      if (current_kind_ == QStringLiteral("group") ||
          current_kind_ == QStringLiteral("dgroup")) {
        show_status(QStringLiteral("群会话暂不支持自定义表情（走文件单聊）"));
        return;
      }
      const std::string tid =
          direct_engine_.send_file(current_peer_.toStdString(), path);
      if (tid.empty()) {
        show_status(QStringLiteral("自定义表情发送失败"));
        return;
      }
      append_system_line(QStringLiteral("[表情] %1")
                             .arg(esc(QFileInfo(path).fileName())));
    });
    grid->addWidget(b, row, col);
    if (++col >= 8) { col = 0; ++row; }
  }
  auto* imp = new QPushButton(QStringLiteral("导入自定义表情…"), dlg);
  connect(imp, &QPushButton::clicked, this, [this, dlg] {
    const QString src = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择表情图片"), QString(),
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.gif)"));
    if (src.isEmpty()) return;
    if (import_emoji(src)) dlg->close(); // 导入失败留下面板＋状态栏报因
  });
  grid->addWidget(imp, row + 1, 0, 1, 4);
  // 需求批② 表情包：云素材管理（上传/清单/删除/下载到本地表情目录）；
  // 收藏随账号走——换机登录即得，下载落 emoji 目录即可在面板发送。
  auto* cloud = new QPushButton(QStringLiteral("云表情包…"), dlg);
  connect(cloud, &QPushButton::clicked, this, [this, dlg] {
    dlg->close();
    EmojiPackDialog d(this, emoji_dir());
    d.exec();
  });
  grid->addWidget(cloud, row + 1, 4, 1, 4);
  // 锚定到「表情」按钮正下方并抬窗激活：Qt::Popup 默认位置交给 WM 摆，
  // 部分环境摆到屏外/父窗后面＝「点表情没反应」（BUG-003）
  if (emoji_btn_) {
    dlg->move(
        emoji_btn_->mapToGlobal(QPoint(0, emoji_btn_->height() + 4)));
  }
  dlg->show();
  dlg->raise();
  dlg->activateWindow();
}

// —— 需求批① 文字颜色 ——
void MainWindow::inject_message(const QString& from_id, const QString& text,
                                bool outgoing, const QString& msg_id,
                                const QString& receipt, qint64 ts_ms,
                                quint64 seq) {
  append_message(from_id, text,
                ts_ms > 0 ? ts_ms : QDateTime::currentMSecsSinceEpoch(),
                outgoing, QStringLiteral("direct"), msg_id, receipt, seq);
}

void MainWindow::inject_image(const QString& from_id, const QString& image_path,
                              bool outgoing) {
  append_image_message(from_id, image_path, QDateTime::currentMSecsSinceEpoch(),
                       outgoing, QStringLiteral("direct"));
}

void MainWindow::request_input_color() {
  if (!input_box_->hasSelectedText()) {
    show_status(QStringLiteral("先选中要着色的文字，再点「颜色」"));
    return;
  }
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  const QColor last = QColor(settings.value(QStringLiteral("input_color"))
                                 .toString());
  const QColor picked = QColorDialog::getColor(
      last.isValid() ? last : QColor(QStringLiteral("#e16531")), this,
      QStringLiteral("文字颜色"), QColorDialog::ShowAlphaChannel);
  if (!picked.isValid()) return; // 取消＝无动作
  settings.setValue(QStringLiteral("input_color"), picked.name());
  apply_input_color(picked);
}

void MainWindow::apply_input_color(const QColor& color) {
  if (!input_box_->hasSelectedText()) {
    show_status(QStringLiteral("先选中要着色的文字，再点「颜色」"));
    return;
  }
  const QString marked = QStringLiteral("〔%1〕%2〔/〕")
                             .arg(color.name(), input_box_->selectedText());
  input_box_->insert(marked); // 替换选区（QLineEdit::insert 即替换当前选区）
  input_box_->setFocus();
}

// —— T4.5 自定义表情：目录与导入 ——
QString MainWindow::emoji_dir() {
  // 测试覆盖：MEMEX_TEST_EMOJI_DIR 指向临时目录（与 MEMEX_TEST_AUTOSTART_DIR
  // 同口径），避免污染真实应用数据目录
  const QByteArray override_dir = qgetenv("MEMEX_TEST_EMOJI_DIR");
  if (!override_dir.isEmpty()) return QString::fromUtf8(override_dir);
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
         QStringLiteral("/emoji");
}

bool MainWindow::import_emoji(const QString& src) {
  static const QSet<QString> kExt{QStringLiteral("png"), QStringLiteral("jpg"),
                                  QStringLiteral("jpeg"),
                                  QStringLiteral("gif")};
  const QFileInfo si(src);
  if (!si.exists() || !si.isFile() || !kExt.contains(si.suffix().toLower())) {
    show_status(QStringLiteral("导入失败：不是支持的表情图片（png/jpg/jpeg/gif）"));
    return false;
  }
  const QString dir = emoji_dir();
  if (!QDir().mkpath(dir)) {
    show_status(QStringLiteral("导入失败：无法创建目录 %1").arg(dir));
    return false;
  }
  const QString dst = dir + QLatin1Char('/') + si.fileName();
  if (QFile::exists(dst) && !QFile::remove(dst)) {
    show_status(QStringLiteral("导入失败：无法覆盖同名文件 %1").arg(dst));
    return false;
  }
  if (!QFile::copy(src, dst)) {
    show_status(QStringLiteral("导入失败：无法写入 %1").arg(dir));
    return false;
  }
  show_status(QStringLiteral("已导入表情：%1（重新打开面板可见）")
                  .arg(si.fileName()));
  return true;
}

// —— T4.2 跨态互通 ——
// 跨态＝恰一边登录协作态（我已登录而对端未登录，或反之）。对端登录态取
// 自发现宣告的 account 字段（空=未登录；仅作显示与判定，不参与路由）。
// 跨态会话固定标「未归档」且不可关闭（A7）；已登录端上报会话建立/结束
// 日志（时间/双方/时长，无内容）——未登录端无通道，由对端上报。
bool MainWindow::is_cross_state(const QString& device_id) const {
  if (device_id.isEmpty()) return false;
  const Peer p = direct_engine_.peer(device_id.toStdString());
  if (p.device_id.empty()) return false; // 未发现：无从判定，不按跨态计
  return collab_engine_.is_logged_in() != !p.account.empty();
}

// 会话首触（收／发第一条）：已登录端上报 start；表内已有即跳过（幂等）。
void MainWindow::cross_touch(const QString& device_id) {
  if (device_id.isEmpty() || cross_open_.contains(device_id)) return;
  if (!collab_engine_.is_logged_in()) return;
  const Peer p = direct_engine_.peer(device_id.toStdString());
  if (p.device_id.empty() || !p.account.empty()) return; // 未发现／非跨态
  const qint64 started_ms = QDateTime::currentMSecsSinceEpoch();
  cross_open_.insert(device_id, started_ms);
  const QString name = p.name.empty()
                           ? QString::fromStdString(p.device_id).left(8)
                           : QString::fromStdString(p.name);
  collab_engine_.cross_log(QStringLiteral("start"), device_id, name,
                           started_ms, 0);
}

// 会话闭环：上报 end 并出表。服务端按 (账号, 设备, 建立时刻) 匹配最早
// 未结束行，对端名不参与匹配（离线后取不到名，传空即可）。
void MainWindow::cross_end(const QString& device_id) {
  const auto it = cross_open_.constFind(device_id);
  if (it == cross_open_.cend()) return;
  const qint64 started_ms = it.value();
  cross_open_.remove(device_id);
  if (!collab_engine_.is_logged_in()) return; // 通道已断，无法上报
  collab_engine_.cross_log(QStringLiteral("end"), device_id, QString(),
                           started_ms,
                           QDateTime::currentMSecsSinceEpoch());
}

void MainWindow::cross_end_all() {
  const QStringList keys = cross_open_.keys();
  for (const QString& id : keys) cross_end(id);
}

// 发现表变化巡检：对端离线或对端已登录（不再跨态）→ 会话闭环上报。
void MainWindow::cross_sweep() {
  const QStringList keys = cross_open_.keys();
  for (const QString& id : keys) {
    const Peer p = direct_engine_.peer(id.toStdString());
    if (p.device_id.empty() || !p.account.empty()) cross_end(id);
  }
}

// 验收面：打开（或发起）与某局域网设备的直连会话。
void MainWindow::open_direct_peer(const QString& device_id) {
  if (device_id.isEmpty()) return;
  open_chat(QStringLiteral("direct"), device_id);
}

bool MainWindow::has_direct_peer(const QString& device_id) const {
  return !device_id.isEmpty() && direct_engine_.has_peer(device_id.toStdString());
}

// —— T3.4 策略开关（下发自服务端，按本人部门解析）——
// 直连发送闸门：未登录发直连＝免登录使用；已登录发直连＝与未登录设备通信。
bool MainWindow::direct_send_allowed() {
  if (!collab_engine_.is_logged_in() && !anonymous_allowed_) {
    show_status(QStringLiteral(
        "当前策略：需登录协作态后使用（免登录使用已被管理员禁止）"));
    return false;
  }
  if (collab_engine_.is_logged_in() && !cross_state_allowed_) {
    show_status(QStringLiteral(
        "当前策略：禁止与未登录设备通信（该会话未进归档）"));
    return false;
  }
  return true;
}

// —— T4.4 截图与标注 ——
// 截图是本机捕获（BUG-001 用户拍板 2026-10-05）：不设设备/会话前置——
// 截屏→本地预览/标注→发送到当前会话（直连/协作走既有文件通道）；
// 落点校验移到发送端：无会话、群会话（文件通道本期仅单聊/协作）才在
// 确认后拦，策略闸门（免登录／跨态）与「发文件」同口径。
void MainWindow::start_screenshot() { screenshot_tool_.start(); }

QString MainWindow::screenshot_shortcut() {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  const QString v = settings.value(QStringLiteral("shortcuts/screenshot"),
                                   QStringLiteral("Ctrl+Alt+A"))
                        .toString();
  return v.isEmpty() ? QStringLiteral("Ctrl+Alt+A") : v;
}

QString MainWindow::apply_screenshot_shortcut(const QKeySequence& seq) {
  if (seq.isEmpty()) return QStringLiteral("键序列为空");
  // 冲突检测：应用内其他 QShortcut／带键 QAction（系统级占用测不到——
  // 全局钩子按平台能力另批，文档注明）
  const auto shorts = findChildren<QShortcut*>();
  for (const QShortcut* sc : shorts) {
    if (sc != shot_sc_ && !sc->key().isEmpty() && sc->key() == seq) {
      return QStringLiteral("与已有快捷键冲突（%1）").arg(seq.toString());
    }
  }
  for (const QAction* act : findChildren<QAction*>()) {
    if (act->shortcuts().contains(seq)) {
      return QStringLiteral("与菜单快捷键冲突（%1）").arg(seq.toString());
    }
  }
  if (shot_sc_) shot_sc_->setKey(seq);
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  settings.setValue(QStringLiteral("shortcuts/screenshot"), seq.toString());
  return {};
}

// 截图确认后发送到当前会话：PNG 临时文件走既有文件通道（与「发文件」同路径）。
void MainWindow::on_screenshot_confirmed(const QString& path) {
  if (!send_shot_to_current_chat(path)) {
    // 发送路径的临时文件清理挂在传输结束回调；这里未进传输，自己兜
    QFile::remove(path);
  }
}

// 直连文件通道的目标设备：direct 会话＝当前 peer；collab 会话＝按对端
// 账号匹配宣告中设备（同网段互见才有通道；找不到＝空，调用方给文案）。
QString MainWindow::direct_peer_target() const {
  if (current_kind_ != QStringLiteral("collab")) return current_peer_;
  for (const Peer& p : direct_engine_.peers()) {
    if (!p.account.empty() &&
        QString::fromStdString(p.account) == current_peer_) {
      return QString::fromStdString(p.device_id);
    }
  }
  return QString();
}

// —— 需求批⑤ 文件夹发送：守卫与发送缝（按钮点击与测试同一路径）——
// 守卫（单源）：无会话／群会话／策略闸门；false＝已给状态文案
bool MainWindow::folder_send_allowed() {
  if (current_peer_.isEmpty()) {
    show_status(QStringLiteral("先选择设备再发送文件夹"));
    return false;
  }
  if (current_kind_ == QStringLiteral("group") ||
      current_kind_ == QStringLiteral("dgroup")) {
    show_status(QStringLiteral("群会话暂不支持文件夹发送（走单聊点对点）"));
    return false;
  }
  if (current_kind_ != QStringLiteral("collab") && !direct_send_allowed()) {
    return false; // T3.4 策略闸门（与发文件同口径，拒绝文案策略面已给）
  }
  return true;
}

// 发送缝：递归遍历走直连文件通道＋作业聚合状态入表。返回作业 id，
// 空＝未发出（守卫拦／目录空／对端不可达，状态文案已给）。
QString MainWindow::send_folder_to_current_chat(const QString& dir) {
  if (!folder_send_allowed()) return {};
  if (!QFileInfo(dir).isDir()) return {};
  const QString job =
      direct_engine_.send_directory(current_peer_.toStdString(), dir);
  if (job.isEmpty()) {
    show_status(
        QStringLiteral("文件夹发送失败（目录为空、对端不可达或未授权）"));
    return {};
  }
  FolderJobState st;
  st.name = QDir(dir).dirName();
  folder_jobs_.insert(job, st);
  // 传输台账（文件传输面）：文件夹作业在途条目（文件粒度计数随
  // directory_progress 刷）
  {
    TransferEntry e;
    e.id = job;
    e.name = st.name;
    e.kind = current_kind_;
    e.peer = current_peer_;
    e.peer_display = tx_peer_display(e.kind, e.peer);
    e.path = dir;
    e.ts_ms = QDateTime::currentMSecsSinceEpoch();
    e.is_folder = true;
    track_transfer(e);
  }
  append_system_line(
      QStringLiteral("[文件夹] %1 开始发送（递归含子目录）").arg(esc(st.name)));
  show_status(QStringLiteral("文件夹发送中：%1").arg(st.name));
  return job;
}

// 振屏发送缝（需求批⑥，与「振屏」按钮单源）：守卫＋防刷限频（同一会话
// 10s 冷却）→ 协作走 NUDGE 帧经服务端归档＋在线即投，直连走点对点空体
// NUDGE。true＝已发出（本端标记行由引擎落库，界面此处渲染系统行）。
bool MainWindow::send_nudge_to_current_chat() {
  if (current_peer_.isEmpty()) {
    show_status(QStringLiteral("先选择会话再发送振屏"));
    return false;
  }
  if (current_kind_ == QStringLiteral("group") ||
      current_kind_ == QStringLiteral("dgroup")) {
    show_status(QStringLiteral("群会话暂不支持振屏（走单聊点对点）"));
    return false;
  }
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (now - last_nudge_sent_.value(current_peer_, 0) < 10000) {
    show_status(QStringLiteral("振屏发送太频繁（同一会话 10 秒一次）"));
    return false;
  }
  const quint64 seq = current_kind_ == QStringLiteral("collab")
                          ? collab_engine_.send_nudge(current_peer_)
                          : direct_engine_.send_nudge(
                                direct_peer_target().toStdString());
  if (seq == 0) {
    show_status(QStringLiteral("振屏发送失败（对端不可达或通道不可用）"));
    return false;
  }
  last_nudge_sent_.insert(current_peer_, now);
  const QString source = current_kind_ == QStringLiteral("collab")
                             ? QStringLiteral("collab")
                             : QStringLiteral("direct");
  append_message(current_peer_, QStringLiteral("[振屏]"),
                 QDateTime::currentMSecsSinceEpoch(), true, source);
  if (current_kind_ == QStringLiteral("collab")) {
    set_delivery_state(QStringLiteral("发送中…（seq %1）").arg(seq));
  }
  show_status(QStringLiteral("振屏已发送"));
  return true;
}

// 振屏到达（需求批⑥，直连/协作两域单源）：当前会话即时渲染（append_message
// 对标记文本特判为居中系统行，历史重载同构）＋抖窗与提示音（效果限频 2s）；
// 窗口非激活时托盘通知（复用消息事件开关与提示音档位）。
void MainWindow::on_nudge_received(const QString& from, qint64 ts_ms,
                                   const QString& source) {
  const bool in_chat =
      from == current_peer_ &&
      (current_kind_ == QStringLiteral("direct") ||
       current_kind_ == QStringLiteral("collab")) &&
      ((source == QStringLiteral("collab")) ==
       (current_kind_ == QStringLiteral("collab")));
  if (in_chat) {
    append_message(from, QStringLiteral("[振屏]"), ts_ms, false, source);
  } else {
    show_status(QStringLiteral("来自 %1 的振屏").arg(from));
  }
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (now - last_nudge_effect_ms_ >= 2000) {
    last_nudge_effect_ms_ = now;
    ++shake_count_;
    shake_window();
  }
  if (!isActiveWindow()) {
    event_notify(NotifyPrefs::load().popup_message, SoundEvent::Message,
                 QStringLiteral("振屏"),
                 QStringLiteral("来自 %1 的窗口抖动").arg(from));
  }
}

// 窗口抖动（需求批⑥）：短促左右移位后复位。move 在离屏/测试环境同样
// 可行；效果计数在 on_nudge_received 已记（提示音复用消息档位，无独立音频）。
void MainWindow::shake_window() {
  const QPoint orig = pos();
  constexpr int kAmplitude = 8;  // 单侧移位像素
  constexpr int kSteps = 8;      // 抖动步数（约 320ms）
  for (int i = 0; i < kSteps; ++i) {
    const int dx = (i % 2 == 0) ? kAmplitude : -kAmplitude;
    QTimer::singleShot(i * 40, this,
                       [this, orig, dx] { move(orig.x() + dx, orig.y()); });
  }
  QTimer::singleShot(kSteps * 40, this,
                     [this, orig] { move(orig.x(), orig.y()); });
}

// 截图发送流（需求批④）：持久拷贝（图片消息本机保留——传输完成后临时
// 件清理不裂图）→直连文件通道→本端图片气泡（图片消息，非系统行）。
bool MainWindow::send_shot_to_current_chat(const QString& path) {
  if (current_peer_.isEmpty()) {
    show_status(QStringLiteral("截图已取消：先选择会话再截图发送"));
    return false;
  }
  if (current_kind_ == QStringLiteral("group") ||
      current_kind_ == QStringLiteral("dgroup")) {
    show_status(QStringLiteral(
        "群会话暂不支持截图发送（文件通道本期仅单聊/协作）"));
    return false;
  }
  if (current_kind_ != QStringLiteral("collab") && !direct_send_allowed()) {
    return false;
  }
  const QString target = direct_peer_target();
  if (target.isEmpty()) {
    show_status(QStringLiteral(
        "对方未发现直连通道（同网段未互见），截图无法发送"));
    return false;
  }
  // 持久拷贝：AppDataLocation/sent-shots/（时间戳防重名；失败=仍走原
  // 文件路径发，气泡退化系统行——临时件传完清理不指向它）
  const QString shots_dir =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
      QStringLiteral("/sent-shots");
  QDir().mkpath(shots_dir);
  const QString persist =
      shots_dir + QLatin1Char('/') +
      QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmsszzz")) +
      QLatin1Char('-') + QFileInfo(path).fileName();
  const bool copied = QFile::copy(path, persist);
  const std::string tid = direct_engine_.send_file(target.toStdString(), path);
  if (tid.empty()) {
    // 授权链修复：同步拒（未授权 fail-closed）或通道即败＝立刻失败，
    // 不再乐观上屏（此前被拒仍显示「已发送」气泡）
    show_status(QStringLiteral("截图发送失败（未授权或通道不可用）"));
    return false;
  }
  shot_paths_.insert(QString::fromStdString(tid), path);
  const QString name = QFileInfo(path).fileName();
  file_sent_.insert(name, 0);
  // 传输台账（文件传输面）：截图也是真实字节流传输，如实入账
  {
    TransferEntry e;
    e.id = QString::fromStdString(tid);
    e.name = name;
    e.kind = QStringLiteral("direct");
    e.peer = target;
    e.peer_display = tx_peer_display(e.kind, e.peer);
    e.path = path;
    e.bytes_total = static_cast<quint64>(QFileInfo(path).size());
    e.ts_ms = QDateTime::currentMSecsSinceEpoch();
    track_transfer(e);
  }
  if (copied) {
    append_image_message(current_peer_, persist,
                         QDateTime::currentMSecsSinceEpoch(),
                         /*outgoing=*/true, QStringLiteral("direct"));
    show_status(QStringLiteral("截图已发送至当前会话：%1").arg(name));
  } else {
    append_system_line(QStringLiteral("[截图] %1 开始发送").arg(esc(name)));
    show_status(QStringLiteral("截图已发送至当前会话：%1").arg(name));
  }
  return true;
}

// 解析本人生效策略：本人部门 → 逐级上级部门 → 全局行 → 默认宽松
void MainWindow::apply_policy(const QString& org_json) {
  nlohmann::json j =
      nlohmann::json::parse(org_json.toStdString(), nullptr, false);
  if (j.is_discarded()) return;
  // 本人部门路径
  QString dept;
  const QString me = collab_engine_.account();
  if (j.contains("members")) {
    for (const auto& m : j["members"]) {
      if (QString::fromStdString(m.value("account", std::string{})) == me) {
        dept = QString::fromStdString(m.value("department_path", std::string{}));
        break;
      }
    }
  }
  const auto match = [&](const QString& path) -> const nlohmann::json* {
    if (!j.contains("policies")) return nullptr;
    for (const auto& p : j["policies"]) {
      if (QString::fromStdString(p.value("department_path", std::string{})) ==
          path) {
        return &p;
      }
    }
    return nullptr;
  };
  const nlohmann::json* row = nullptr;
  QString path = dept;
  while (!path.isEmpty()) {
    if (const nlohmann::json* hit = match(path)) {
      row = hit;
      break;
    }
    const int pos = path.lastIndexOf(QChar('/'));
    if (pos <= 0) break;
    path.truncate(pos);
  }
  if (!row) row = match(QString()); // 全局行
  if (row) {
    anonymous_allowed_ = row->value("allow_anonymous", true);
    cross_state_allowed_ = row->value("allow_cross_state", true);
  } else {
    anonymous_allowed_ = true; // 未配置＝默认宽松
    cross_state_allowed_ = true;
  }
}

bool MainWindow::collab_logged_in() const {
  return collab_engine_.is_logged_in();
}

// —— T4.1 群聊 ——

QString MainWindow::groups_json() const { return last_groups_json_; }

// 服务端无群列表推送（GROUP_DATA 仅应答 GROUP_QUERY），群变更他端发起时
// 由此重拉（登录时已自动拉一次）
void MainWindow::refresh_groups() { collab_engine_.query_groups(); }

// 组织架构全部账号（建群／拉人数据源；未拉到组织架构时为空，弹窗走手输）
QStringList MainWindow::org_accounts() const {
  QStringList out;
  nlohmann::json j = nlohmann::json::parse(last_org_json_.toStdString(),
                                           nullptr, false);
  if (j.is_discarded() || !j.contains("members")) return out;
  for (const auto& m : j["members"]) {
    const QString a =
        QString::fromStdString(m.value("account", std::string{}));
    if (!a.isEmpty()) out << a;
  }
  return out;
}

// 建群（服务端群）：群名＋成员多选（组织架构账号＋手输补位）
void MainWindow::create_group_dialog() {
  if (!collab_engine_.is_logged_in()) {
    show_status(QStringLiteral("建群需登录协作态（群消息经服务端扇出并归档）"));
    return;
  }
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("新建群聊"));
  dlg.resize(380, 520);
  auto* form = new QFormLayout(&dlg);
  auto* name = new QLineEdit(&dlg);
  name->setPlaceholderText(QStringLiteral("例如：研发部日常"));
  form->addRow(QStringLiteral("群名"), name);
  // 群组-1：主题（一句话）＋目的（多行说明）均可留空（定案①跳过＝
  // 直接建群；空主题落空串＝群名下方不显示该行）
  auto* topic = new QLineEdit(&dlg);
  topic->setPlaceholderText(QStringLiteral("一句话主题（可空）"));
  form->addRow(QStringLiteral("主题"), topic);
  auto* purpose = new QTextEdit(&dlg);
  purpose->setPlaceholderText(QStringLiteral("群目的说明（可空）"));
  purpose->setFixedHeight(64);
  form->addRow(QStringLiteral("目的"), purpose);
  auto* members = new QListWidget(&dlg);
  const QString me = collab_engine_.account();
  for (const QString& a : org_accounts()) {
    if (a == me) continue;
    auto* item = new QListWidgetItem(a, members);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
  }
  form->addRow(QStringLiteral("成员"), members);
  auto* extra = new QLineEdit(&dlg);
  extra->setPlaceholderText(QStringLiteral("其他账号，逗号分隔（可空）"));
  form->addRow(QString(), extra);
  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  form->addRow(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (dlg.exec() != QDialog::Accepted) return;
  const QString gname = name->text().trimmed();
  if (gname.isEmpty()) {
    show_status(QStringLiteral("建群取消：群名不能为空"));
    return;
  }
  QStringList picked;
  for (int i = 0; i < members->count(); ++i) {
    if (members->item(i)->checkState() == Qt::Checked) {
      picked << members->item(i)->text();
    }
  }
  for (const QString& part :
       extra->text().split(QChar(','), Qt::SkipEmptyParts)) {
    const QString a = part.trimmed();
    if (!a.isEmpty() && !picked.contains(a)) picked << a;
  }
  collab_engine_.create_group(gname, picked, topic->text().trimmed(),
                              purpose->toPlainText().trimmed());
  show_status(QStringLiteral("建群请求已发送（%1 名成员）").arg(picked.size()));
}

// 打开群会话（group_key 形如 "group:7"）
void MainWindow::open_group(const QString& group_key) {
  if (group_key.isEmpty()) return;
  const quint64 gid =
      group_key.mid(QStringLiteral("group:").size()).toULongLong();
  if (!groups_.contains(gid)) {
    show_status(QStringLiteral("群 %1 不在列表（可能已退群／解散）").arg(gid));
    return;
  }
  open_chat(QStringLiteral("group"), group_key);
}

// 常用联系人到达（T4.5）：缓存并刷新列表（星标置顶排序在 refresh 里按此算）
void MainWindow::apply_favs(const QString& fav_json) {
  last_fav_json_ = fav_json;
  refresh_devices();
}

QString MainWindow::fav_json() const { return last_fav_json_; }

bool MainWindow::fav_is_starred_(const QString& peer) const {
  nlohmann::json j = nlohmann::json::parse(last_fav_json_.toStdString(),
                                           nullptr, false);
  if (j.is_discarded() || !j.is_array()) return false;
  for (const auto& e : j) {
    if (e.value("peer", std::string{}) == peer.toStdString()) {
      return e.value("starred", false);
    }
  }
  return false;
}

qint64 MainWindow::fav_last_ms_(const QString& peer) const {
  nlohmann::json j = nlohmann::json::parse(last_fav_json_.toStdString(),
                                           nullptr, false);
  if (j.is_discarded() || !j.is_array()) return 0;
  for (const auto& e : j) {
    if (e.value("peer", std::string{}) == peer.toStdString()) {
      return e.value("last_ms", 0LL);
    }
  }
  return 0;
}

void MainWindow::toggle_current_fav_star() {
  if (current_kind_ != QStringLiteral("collab") &&
      current_kind_ != QStringLiteral("group")) {
    show_status(QStringLiteral("常用联系人星标仅适用于协作会话／服务端群"));
    return;
  }
  if (!collab_engine_.is_logged_in()) {
    show_status(QStringLiteral("未登录协作态，常用联系人不可用"));
    return;
  }
  const QString peer = current_peer_;
  if (peer.isEmpty()) return;
  collab_engine_.fav_cmd(fav_is_starred_(peer) ? QStringLiteral("unstar")
                                               : QStringLiteral("star"),
                         peer);
}

// 群列表数据到达：解析入 groups_，刷新列表与当前群会话标题
void MainWindow::apply_groups(const QString& groups_json) {
  last_groups_json_ = groups_json;
  nlohmann::json j = nlohmann::json::parse(groups_json.toStdString(),
                                           nullptr, false);
  if (j.is_discarded() || !j.is_array()) {
    show_status(QStringLiteral("群列表数据解析失败"));
    return;
  }
  groups_.clear();
  for (const auto& g : j) {
    GroupEntry e;
    e.name = QString::fromStdString(g.value("name", std::string{}));
    e.owner = QString::fromStdString(g.value("owner", std::string{}));
    e.announcement =
        QString::fromStdString(g.value("announcement", std::string{}));
    e.topic = QString::fromStdString(g.value("topic", std::string{}));
    e.purpose = QString::fromStdString(g.value("purpose", std::string{}));
    if (g.contains("members")) {
      for (const auto& m : g["members"]) {
        e.members << QString::fromStdString(m.get<std::string>());
      }
    }
    groups_.insert(g.value("group_id", 0), std::move(e));
  }
  // 当前打开的群：标题/meta/右栏随最新数据刷新；群没了则收尾提示
  if (current_kind_ == QStringLiteral("group")) {
    if (GroupEntry* g = current_group()) {
      chat_title_->setText(g->name);
      chat_title_->setToolTip(
          QStringLiteral("群 %1").arg(current_group_id()));
      chat_meta_->setText(group_meta_text(*g));
      update_group_panel();
      update_banner(); // 公告变化→横幅随换（群公告占横幅口径）
    } else {
      append_system_line(
          QStringLiteral("该群已退出或已解散：本会话仅供查看历史"));
    }
  }
  refresh_devices();
}

quint64 MainWindow::current_group_id() const {
  if (current_kind_ != QStringLiteral("group")) return 0;
  return current_peer_.mid(QStringLiteral("group:").size()).toULongLong();
}

MainWindow::GroupEntry* MainWindow::current_group() {
  const quint64 gid = current_group_id();
  auto it = groups_.find(gid);
  return gid > 0 && it != groups_.end() ? &it.value() : nullptr;
}

// 通讯录账号→显示名（org 未下发＝空）：群右栏成员行与群主名用
QString MainWindow::roster_display(const QString& account) const {
  const QJsonDocument od = QJsonDocument::fromJson(last_org_json_.toUtf8());
  if (!od.isObject()) return QString();
  for (const auto& m :
       od.object().value(QStringLiteral("members")).toArray()) {
    const QJsonObject mo = m.toObject();
    if (mo.value(QStringLiteral("account")).toString() == account) {
      const QString d = mo.value(QStringLiteral("display_name")).toString();
      if (!d.isEmpty()) return d;
    }
  }
  return QString();
}

// 群会话头 meta 行（原型 desktop-group.html .ch-meta「N 人 · 群主：X ·
// 全量归档」）；主题/目的为群组-1 功能面，非空缀尾（原型未覆盖，保留）
QString MainWindow::group_meta_text(const GroupEntry& g) const {
  QString owner = roster_display(g.owner);
  if (owner.isEmpty()) owner = g.owner;
  QString meta = QStringLiteral("%1 人 · 群主：%2 · 全量归档")
                     .arg(g.members.size())
                     .arg(owner);
  if (!g.topic.isEmpty()) {
    meta += QStringLiteral("　·　主题：%1").arg(g.topic);
  }
  if (!g.purpose.isEmpty()) {
    QString p = g.purpose;
    p.replace(QChar('\n'), QStringLiteral("／"));
    meta += QStringLiteral("　·　目的：%1").arg(p);
  }
  return meta;
}

// 群右栏（原型 desktop-group.html .infopanel）：群成员（≤6 行＋「查看全部」
// ＝群资料窗）、群文件（会话窗口内的文件卡——文件传输不落消息库，会话内存
// 态如实口径）、尾卡归档说明虚线框。重建式刷新：数据与主题变化都重摆。
void MainWindow::update_group_panel() {
  if (!group_panel_ || !group_panel_lay_) return;
  const bool group_session = current_kind_ == QStringLiteral("group") &&
                             chat_open_ && collab_engine_.is_logged_in();
  const GroupEntry* g = group_session ? current_group() : nullptr;
  if (!g) {
    group_panel_->hide();
    return;
  }
  group_panel_->setVisible(!group_panel_hidden_);
  while (group_panel_lay_->count() > 0) {
    QLayoutItem* it = group_panel_lay_->takeAt(0);
    if (it->widget()) it->widget()->deleteLater();
    delete it;
  }
  const ThemeTokens& t = tk();
  auto add_h4 = [this, &t](const QString& text) {
    auto* h = new QLabel(text, group_panel_);
    h->setStyleSheet(QStringLiteral("font-size:12px; font-weight:700; "
                                    "color:%1;")
                         .arg(t.text_muted.name()));
    group_panel_lay_->addWidget(h);
  };

  // 群成员（原型 .member-row：28px 圆角 8 头像＋名＋.role 群主胶囊）
  add_h4(QStringLiteral("群成员 · %1").arg(g->members.size()));
  const QString me = collab_engine_.account();
  int shown = 0;
  for (const QString& acc : g->members) {
    if (shown >= 6) break;
    ++shown;
    QString display = roster_display(acc);
    if (display.isEmpty()) display = acc;
    auto* row = new QWidget(group_panel_);
    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);
    auto* av = new QLabel(avatar_initial(display), row);
    av->setFixedSize(28, 28);
    av->setAlignment(Qt::AlignCenter);
    av->setStyleSheet(QStringLiteral("background:%1; color:%2; "
                                     "font-weight:700; font-size:11px; "
                                     "border-radius:8px;")
                          .arg(avatar_hue(display).name(),
                               t.on_brand.name()));
    auto* nm = new QLabel(acc == me
                              ? QStringLiteral("%1（我）").arg(display)
                              : display,
                          row);
    nm->setStyleSheet(
        QStringLiteral("font-size:12.5px; color:%1;").arg(t.text.name()));
    lay->addWidget(av);
    lay->addWidget(nm, 1);
    if (acc == g->owner) {
      auto* role = new QLabel(QStringLiteral("群主"), row);
      role->setStyleSheet(
          QStringLiteral("color:%1; background:%2; font-size:10.5px; "
                         "border-radius:999px; padding:1px 7px;")
              .arg(t.brand_text.name(), t.brand_wash.name()));
      lay->addWidget(role, 0, Qt::AlignRight);
    }
    group_panel_lay_->addWidget(row);
  }
  if (g->members.size() > 6) {
    // 「查看全部 N 人」：开群资料窗（成员全集在彼处）
    auto* more = new QPushButton(
        QStringLiteral("查看全部 %1 人").arg(g->members.size()),
        group_panel_);
    more->setFlat(true);
    more->setCursor(Qt::PointingHandCursor);
    more->setStyleSheet(
        QStringLiteral("color:%1; font-size:11.5px; padding:6px 0;")
            .arg(t.text_muted.name()));
    connect(more, &QPushButton::clicked, this,
            [this, gid = current_group_id()] { group_profile_dialog(gid); });
    group_panel_lay_->addWidget(more, 0, Qt::AlignHCenter);
  }

  // 群文件（原型：📄 文件名行×3）——取本会话窗口内最近 3 个文件卡
  add_h4(QStringLiteral("群文件"));
  QStringList files;
  for (const ChatRow& r : chat_rows_) {
    if (r.file_card) files << r.text;
  }
  if (files.isEmpty()) {
    auto* empty = new QLabel(
        QStringLiteral("暂无群文件（会话内收发的文件在此列出）"),
        group_panel_);
    empty->setWordWrap(true);
    empty->setStyleSheet(
        QStringLiteral("font-size:11px; color:%1;").arg(t.text_muted.name()));
    group_panel_lay_->addWidget(empty);
  } else {
    const QFontMetrics fm(font());
    for (auto it = files.crbegin();
         it != files.crend() && it - files.crbegin() < 3; ++it) {
      auto* f = new QLabel(QStringLiteral("%1  %2").arg(
                               file_icon_char(*it),
                               fm.elidedText(*it, Qt::ElideRight, 170)),
                           group_panel_);
      f->setStyleSheet(
          QStringLiteral("font-size:12px; color:%1;").arg(t.text.name()));
      group_panel_lay_->addWidget(f);
    }
  }
  group_panel_lay_->addStretch();
  // 归档说明虚线卡（原型 .infopanel 尾卡：11px 三行说明）
  auto* card = new QLabel(
      QStringLiteral("群消息与群文件元数据全量归档，可按群成员与关键词检"
                     "索。直连临时群（见列表）不经服务端、不归档。"),
      group_panel_);
  card->setWordWrap(true);
  card->setStyleSheet(
      QStringLiteral("border:1px dashed %1; border-radius:10px; "
                     "padding:10px 12px; font-size:11px; color:%2;")
          .arg(t.surface_alt.name(), t.text_muted.name()));
  group_panel_lay_->addWidget(card);
}

// @ 成员选择菜单（原型 desktop-group.html「@ 唤起成员选择」）：输入「@词头」
// 时弹成员菜单（显示名按词头过滤，≤12 项）；无词头/非群会话/无成员＝收起。
// 选中回填「@显示名 」并停用菜单（词头消失，textChanged 自动收）。
void MainWindow::rebuild_at_menu(const QString& text) {
  if (!at_menu_) return;
  const bool group_ok =
      current_kind_ == QStringLiteral("group") &&
      collab_engine_.is_logged_in();
  static const QRegularExpression tail_re(
      QStringLiteral("@([A-Za-z0-9_.\\-\\x{4e00}-\\x{9fa5}]*)$"));
  const auto m = tail_re.match(text);
  const GroupEntry* g = group_ok ? current_group() : nullptr;
  if (!g || !m.hasMatch()) {
    at_menu_->hide();
    return;
  }
  const QString partial = m.captured(1);
  const int at_idx = m.capturedStart(1) - 1;
  const QString me = collab_engine_.account();
  at_menu_->clear();
  int added = 0;
  for (const QString& acc : g->members) {
    if (acc == me) continue; // @ 自己无意义
    QString d = roster_display(acc);
    if (d.isEmpty()) d = acc;
    if (!partial.isEmpty() &&
        !d.contains(partial, Qt::CaseInsensitive) &&
        !acc.contains(partial, Qt::CaseInsensitive)) {
      continue;
    }
    QAction* act =
        at_menu_->addAction(QStringLiteral("%1（%2）").arg(d, acc));
    connect(act, &QAction::triggered, this, [this, d, at_idx] {
      QString cur = input_box_->text();
      cur = cur.left(at_idx) + QStringLiteral("@") + d + QStringLiteral(" ");
      input_box_->setText(cur);
      input_box_->setCursorPosition(cur.size());
      input_box_->setFocus();
      at_menu_->hide();
    });
    if (++added >= 12) break;
  }
  if (added == 0) {
    at_menu_->hide();
    return;
  }
  if (!at_menu_->isVisible()) {
    at_menu_->adjustSize();
    const QPoint pos = input_box_->mapToGlobal(QPoint(0, 0));
    at_menu_->popup(QPoint(pos.x(),
                           pos.y() - at_menu_->sizeHint().height() - 4));
  }
}

// —— 文件传输面（原型 desktop-files.html 整屏）——
// 进入：rail「文件传输」切视图（侧栏换「传输通道」页、主区换传输列表）。
// 列表态窄窗（320）放宽到工作面宽——侧栏 220＋传输列表需要横向余量；
// 会话态宽窗保持原尺寸。
void MainWindow::enter_transfers_view() {
  const bool entering = sidebar_mode_ != QStringLiteral("transfers");
  sidebar_mode_ = QStringLiteral("transfers");
  if (entering) {
    setMaximumWidth(QWIDGETSIZE_MAX); // 释放列表态右缘上限（close_chat 设）
    if (!isMaximized() && width() < 700) resize(960, qMax(height(), 560));
  }
  if (rail_) rail_->set_active(QStringLiteral("files"));
  apply_view_mode();
}

// 离开：有会话＝复展开聊天面板（会话态几何本就在）；无会话＝回列表态
// （窄窗＋右缘上限，与 ✕ 关闭同一几何口径）。
void MainWindow::leave_transfers_view() {
  if (sidebar_mode_ != QStringLiteral("transfers")) return;
  sidebar_mode_ = QStringLiteral("sessions");
  if (rail_) rail_->set_active(QStringLiteral("messages"));
  apply_view_mode();
  if (!current_peer_.isEmpty() && chat_open_) {
    side_->setFixedWidth(260);
    chat_panel_->show();
    update_group_panel();
  } else {
    close_chat();
  }
}

// —— 组织架构面（原型 desktop-org.html 三栏整屏）——
// 进入：登录守卫→置态→数据未到即请求（到达回调里重建）→在屏即重建。
// rebuild 只发生在此处／ORG_DATA 回调／主题切换——apply_view_mode 只管
// 显隐（show_org_profile 尾部回显隐不得再触发 rebuild，成环实录 SEGFAULT）。
void MainWindow::enter_org_view() {
  if (!collab_engine_.is_logged_in()) {
    show_status(QStringLiteral("组织架构需登录协作态后查看"));
    return;
  }
  const bool entering = sidebar_mode_ != QStringLiteral("org");
  sidebar_mode_ = QStringLiteral("org");
  if (entering) {
    setMaximumWidth(QWIDGETSIZE_MAX); // 释放列表态右缘上限（同传输面）
    if (!isMaximized() && width() < 700) resize(960, qMax(height(), 560));
    if (last_org_json_.isEmpty()) {
      org_dialog_pending_ = true; // 数据到达回调里 rebuild_org_view
      collab_engine_.query_org();
    }
  }
  if (rail_) rail_->set_active(QStringLiteral("org"));
  if (entering) rebuild_org_view();
  apply_view_mode();
}

bool MainWindow::org_view_active() const {
  return org_side_ && org_page_ && !org_side_->isHidden() &&
         !org_page_->isHidden();
}

// 离开：回会话态（有会话＝复展开聊天面板，无会话＝回列表态）。
void MainWindow::leave_org_view() {
  if (sidebar_mode_ != QStringLiteral("org")) return;
  sidebar_mode_ = QStringLiteral("sessions");
  if (rail_) rail_->set_active(QStringLiteral("messages"));
  apply_view_mode();
  if (!current_peer_.isEmpty() && chat_open_) {
    side_->setFixedWidth(260);
    chat_panel_->show();
    update_group_panel();
  } else {
    close_chat();
  }
}

// 部门子树成员计数（path 空=全量；子树=本路径或以「路径/」开头）。
int MainWindow::org_dept_member_count(const QString& path) const {
  int n = 0;
  for (const OrgMember& m : org_members_) {
    if (path.isEmpty() || m.dept == path ||
        m.dept.startsWith(path + QStringLiteral("/")))
      ++n;
  }
  return n;
}

// 部门负责人推导（无独立字段，不造假）：部门子树内、直属上级不在本部门
// 子树内的成员唯一时给出（组织树意义上传出部门的那个人）；否则省略胶囊。
QString MainWindow::org_dept_head(const QString& path) const {
  QString head;
  for (const OrgMember& m : org_members_) {
    const bool in_dept =
        m.dept == path || m.dept.startsWith(path + QStringLiteral("/"));
    if (!in_dept || m.manager.isEmpty()) continue;
    bool manager_outside = true;
    for (const OrgMember& x : org_members_) {
      if (x.account != m.manager) continue;
      if (x.dept == path || x.dept.startsWith(path + QStringLiteral("/")))
        manager_outside = false;
      break;
    }
    if (!manager_outside) continue;
    if (!head.isEmpty()) return {}; // 多候选＝推导不唯一，不冒充
    head = m.name.isEmpty() ? m.account : m.name;
  }
  return head;
}

// 全量重建（org 数据到达／进入视图／主题切换）：缓存解析→左栏树→中栏
// 网格→右栏档案。清空重建循三招模板：先 hide 再 deleteLater、新件显式
// show、尾部 activate 同步结算（嵌套事件泵抓图实录）。
void MainWindow::rebuild_org_view() {
  if (!org_side_ || !org_page_) return;
  const ThemeTokens& t = tk();
  // —— 数据缓存（树/网格/档案共用）——
  org_members_.clear();
  org_dept_paths_.clear();
  {
    nlohmann::json j = nlohmann::json::parse(last_org_json_.toStdString(),
                                             nullptr, false);
    if (!j.is_discarded()) {
      if (j.contains("departments")) {
        for (const auto& d : j["departments"]) {
          const QString p =
              QString::fromStdString(d.value("path", std::string{}));
          if (!p.isEmpty()) org_dept_paths_ << p;
        }
      }
      if (j.contains("members")) {
        for (const auto& m : j["members"]) {
          OrgMember om;
          om.account =
              QString::fromStdString(m.value("account", std::string{}));
          om.name =
              QString::fromStdString(m.value("display_name", std::string{}));
          om.title =
              QString::fromStdString(m.value("title", std::string{}));
          om.dept = QString::fromStdString(
              m.value("department_path", std::string{}));
          om.manager =
              QString::fromStdString(m.value("manager", std::string{}));
          om.role = QString::fromStdString(m.value("role", std::string{}));
          if (!om.account.isEmpty()) org_members_.append(om);
        }
      }
    }
  }
  // —— 左栏：树＋可见性说明卡（org_tree_lay_ 里树销毁重建，说明卡常驻）——
  // 先断开旧过滤 connect 再清词（org_search_ 常驻，重建累积 connect 会泄漏；
  // 陈旧过滤词不带入新树）
  disconnect(org_search_, &QLineEdit::textChanged, this, nullptr);
  org_search_->clear();
  while (org_tree_lay_->count() > 0) {
    QLayoutItem* it = org_tree_lay_->takeAt(0);
    if (QWidget* w = it ? it->widget() : nullptr) {
      if (w != org_note_ && w != org_search_) {
        w->hide();
        w->deleteLater();
      }
    }
    delete it;
  }
  // 常驻件回挂（清空循环把它们也摘了下来）
  org_search_->show();
  org_tree_lay_->addWidget(org_search_);
  org_tree_ = new QTreeWidget(org_side_);
  org_tree_->setObjectName(QStringLiteral("org_tree"));
  org_tree_->setHeaderHidden(true);
  org_tree_->setRootIsDecorated(false);
  org_tree_->setIndentation(14);
  org_tree_->setFrameShape(QFrame::NoFrame);
  org_tree_->setColumnCount(2); // 0=部门名（拉伸） 1=子树计数（固定宽右对齐）
  org_tree_->header()->setStretchLastSection(false);
  org_tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  org_tree_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
  org_tree_->header()->resizeSection(1, 38);
  org_tree_->setStyleSheet(
      QStringLiteral("QTreeWidget { background:transparent; border:none; "
                     "font-size:12.5px; color:%1; }"
                     "QTreeWidget::item { padding:5px 6px; border-radius:8px; }"
                     "QTreeWidget::item:selected { background:%2; color:%3; }")
          .arg(t.text.name(), t.brand_tint.name(), t.text.name()));
  // 全路径逐级挂树（同旧弹窗算法），节点带子树成员计数
  std::map<QString, QTreeWidgetItem*> nodes;
  {
    // 根节点「全部成员」（路径空=全员；唯一回全量视图的入口）
    auto* root = new QTreeWidgetItem(org_tree_->invisibleRootItem());
    root->setText(0, QStringLiteral("全部成员"));
    root->setData(0, Qt::UserRole, QString());
    root->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    nodes.emplace(QString(), root);
  }
  for (const QString& path : org_dept_paths_) {
    const QStringList segs = path.split(QChar('/'));
    QTreeWidgetItem* parent = nullptr;
    QString prefix;
    for (const QString& seg : segs) {
      prefix = prefix.isEmpty() ? seg : prefix + QChar('/') + seg;
      auto it = nodes.find(prefix);
      if (it == nodes.end()) {
        auto* item = new QTreeWidgetItem(
            parent ? parent : org_tree_->invisibleRootItem());
        item->setText(0, seg);
        item->setData(0, Qt::UserRole, prefix);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        it = nodes.emplace(prefix, item).first;
      }
      parent = it->second;
    }
  }
  for (auto& [path, item] : nodes) {
    item->setText(1, QString::number(org_dept_member_count(path)));
    item->setForeground(
        1, QBrush(t.text_muted)); // 计数弱化（正文令牌色由树样式统管 0 列）
  }
  org_tree_->expandAll();
  connect(org_tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
    auto* sel = org_tree_ ? org_tree_->currentItem() : nullptr;
    org_selected_dept_ = sel ? sel->data(0, Qt::UserRole).toString()
                             : QString();
    refresh_org_members();
  });
  org_tree_->show();
  org_tree_lay_->addWidget(org_tree_, 1);
  // 可见性策略说明卡（服务端真实裁剪口径：隐藏部门/成员数据不出服务端）
  org_note_->setText(
      QStringLiteral("通讯录可见性按部门策略下发：被隐藏的部门与成员不出现在"
                     "此处、不可被搜索；受限部门只能看到本部门。"));
  org_note_->setStyleSheet(
      QStringLiteral("border:1px dashed %1; border-radius:10px; padding:10px "
                     "12px; font-size:11px; color:%2;")
          .arg(t.border.name(), t.text_muted.name()));
  org_note_->show();
  org_tree_lay_->addWidget(org_note_);
  org_tree_lay_->activate();
  // 搜索：部门名/成员名/账号任一命中即保留该部门子树（含祖先链）
  connect(org_search_, &QLineEdit::textChanged, this, [this](const QString&) {
    if (!org_tree_) return;
    const QString needle = org_search_->text().trimmed();
    QTreeWidgetItemIterator it(org_tree_);
    while (*it) {
      QTreeWidgetItem* item = *it;
      ++it;
      const QString path = item->data(0, Qt::UserRole).toString();
      bool hit = needle.isEmpty();
      if (!hit) hit = item->text(0).contains(needle, Qt::CaseInsensitive);
      if (!hit) {
        for (const OrgMember& m : org_members_) {
          const bool in_subtree =
              m.dept == path || m.dept.startsWith(path + QStringLiteral("/"));
          if (in_subtree &&
              (m.name.contains(needle, Qt::CaseInsensitive) ||
               m.account.contains(needle, Qt::CaseInsensitive))) {
            hit = true;
            break;
          }
        }
      }
      item->setHidden(!hit);
    }
    // 祖先链回显：命中节点的父链不藏
    QTreeWidgetItemIterator it2(org_tree_);
    while (*it2) {
      QTreeWidgetItem* item = *it2;
      ++it2;
      if (item->isHidden()) continue;
      for (QTreeWidgetItem* p = item->parent(); p; p = p->parent())
        p->setHidden(false);
    }
  });
  // —— 中栏＋右栏 ——
  refresh_org_members();
  show_org_profile(org_selected_member_);
}

// 中栏：部门头（部门 · N 人＋负责人胶囊）＋成员卡网格（两列流）。
void MainWindow::refresh_org_members() {
  if (!org_grid_lay_ || !org_grid_empty_ || !org_head_title_) return;
  const ThemeTokens& t = tk();
  const QString me = collab_engine_.account();
  // 部门成员集（子树；空路径=全部可见成员）
  QList<const OrgMember*> dept_members;
  for (const OrgMember& m : org_members_) {
    const bool in = org_selected_dept_.isEmpty() ||
                    m.dept == org_selected_dept_ ||
                    m.dept.startsWith(org_selected_dept_ +
                                      QStringLiteral("/"));
    if (in) dept_members.append(&m);
  }
  const QString dept_label =
      org_selected_dept_.isEmpty()
          ? QStringLiteral("全部成员")
          : org_selected_dept_.section(QChar('/'), -1);
  org_head_title_->setText(QStringLiteral("%1 · %2 人")
                               .arg(dept_label)
                               .arg(dept_members.size()));
  org_head_title_->setStyleSheet(
      QStringLiteral("font-size:15px; font-weight:700; color:%1;")
          .arg(t.text.name()));
  const QString head = org_dept_head(org_selected_dept_);
  if (head.isEmpty()) {
    org_head_pill_->clear();
    org_head_pill_->hide();
  } else {
    org_head_pill_->setText(QStringLiteral("部门负责人：%1").arg(head));
    org_head_pill_->setStyleSheet(
        QStringLiteral("color:%1; border:1px solid %2; border-radius:999px;"
                       " padding:2px 10px; font-size:11.5px;")
            .arg(t.text_muted.name(), t.border.name()));
    org_head_pill_->show();
  }
  // 清空重建（org_grid_empty_ 常驻复用；三招模板）
  while (org_grid_lay_->count() > 0) {
    QLayoutItem* it = org_grid_lay_->takeAt(0);
    if (QWidget* w = it ? it->widget() : nullptr) {
      if (w != org_grid_empty_) {
        w->hide();
        w->deleteLater();
      }
    }
    delete it;
  }
  if (dept_members.isEmpty()) {
    org_grid_empty_->setText(
        org_members_.isEmpty()
            ? QStringLiteral("组织架构数据为空（服务端未下发成员）")
            : QStringLiteral("该部门暂无成员"));
    org_grid_empty_->setStyleSheet(
        QStringLiteral("color:%1; font-size:11.5px;")
            .arg(t.text_muted.name()));
    org_grid_empty_->show();
    org_grid_lay_->addWidget(org_grid_empty_);
    org_grid_lay_->addStretch();
    org_grid_lay_->activate();
    return;
  }
  // 排序：显示名，本人在最后压轴（原型「张伟（我）」卡）
  std::sort(dept_members.begin(), dept_members.end(),
            [me](const OrgMember* a, const OrgMember* b) {
              const bool am = a->account == me, bm = b->account == me;
              if (am != bm) return bm; // 「我」排最后
              return a->name < b->name;
            });
  const auto online_set = online_accounts();
  auto* row = new QHBoxLayout();
  row->setSpacing(10);
  int col = 0;
  for (const OrgMember* pm : dept_members) {
    const OrgMember& m = *pm;
    const bool is_me = m.account == me;
    auto* card = new QWidget(org_grid_empty_->parentWidget());
    // objectName 先于样式表落定（#org_me_card 选择器不随后改名重求值）
    card->setObjectName(is_me ? QStringLiteral("org_me_card")
                              : QStringLiteral("org_card_") + m.account);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    card->setStyleSheet(
        QStringLiteral(
            "QWidget { background:%1; border:1px solid %2; border-radius:10px; }"
            "QWidget#org_me_card { background:%3; border:1px solid %4; }")
            .arg(t.surface_raised.name(), t.border.name(),
                 t.brand_wash.name(), t.brand.name()));
    auto* cl = new QHBoxLayout(card);
    cl->setContentsMargins(10, 9, 10, 9);
    cl->setSpacing(10);
    auto* av = new QLabel(card);
    av->setFixedSize(44, 44);
    av->setAlignment(Qt::AlignCenter);
    av->setStyleSheet(
        QStringLiteral("border-radius:12px; font-size:16px; font-weight:700; "
                       "background:%1; color:%2;")
            .arg(t.brand_tint.name(), t.brand_text.name()));
    const qint64 ver = member_avatar_ver_.value(m.account);
    const QPixmap px = avatar_for(m.account, ver, 44);
    if (!px.isNull()) av->setPixmap(px);
    else av->setText(m.name.isEmpty() ? m.account.left(1) : m.name.left(1));
    cl->addWidget(av);
    auto* tl = new QVBoxLayout();
    tl->setSpacing(1);
    auto* name_l =
        new QLabel(is_me ? QStringLiteral("%1（我）")
                           .arg(m.name.isEmpty() ? m.account : m.name)
                         : (m.name.isEmpty() ? m.account : m.name),
                   card);
    name_l->setStyleSheet(QStringLiteral("font-size:13px; font-weight:700; "
                                         "color:%1;")
                              .arg(t.text.name()));
    tl->addWidget(name_l);
    // 副行：部门末级 · 职务（缺项如实省略）
    QString sub;
    if (!m.dept.isEmpty()) sub += m.dept.section(QChar('/'), -1);
    if (!m.title.isEmpty())
      sub += sub.isEmpty() ? m.title
                           : QStringLiteral(" · ") + m.title;
    if (sub.isEmpty()) sub = QStringLiteral("（未建档）");
    auto* sub_l = new QLabel(sub, card);
    sub_l->setStyleSheet(
        QStringLiteral("font-size:11px; color:%1;").arg(t.text_muted.name()));
    tl->addWidget(sub_l);
    cl->addLayout(tl, 1);
    cl->addStretch();
    // 在线点（协作在线表；离线＝灰空心）
    auto* dot = new QLabel(card);
    dot->setFixedSize(9, 9);
    dot->setStyleSheet(QStringLiteral("background:%1; border-radius:4px;")
                           .arg(online_set.contains(m.account)
                                    ? t.success.name()
                                    : t.border.name()));
    dot->setToolTip(online_set.contains(m.account) ? QStringLiteral("在线")
                                                   : QStringLiteral("离线"));
    cl->addWidget(dot, 0, Qt::AlignTop);
    card->setCursor(Qt::PointingHandCursor);
    // 点卡选档案（事件过滤：鼠标释放即选中，卡片本体无 clicked 信号）
    const QString account = m.account;
    card->installEventFilter(
        new OrgCardClick([this, account] { show_org_profile(account); },
                         card));
    // 已可见父下新建子件显式 show；两列换行
    card->show();
    row->addWidget(card, 1);
    if (++col == 2) {
      auto* roww = new QWidget(org_grid_empty_->parentWidget());
      auto* rl = new QHBoxLayout(roww);
      rl->setContentsMargins(0, 0, 0, 0);
      rl->setSpacing(10);
      // 将 row 内件挪入 roww
      while (row->count() > 0) {
        QLayoutItem* it = row->takeAt(0);
        if (QWidget* w = it->widget()) {
          w->setParent(roww);
          w->show();
          rl->addWidget(w, 1);
        }
        delete it;
      }
      roww->show();
      org_grid_lay_->addWidget(roww);
      row = new QHBoxLayout();
      row->setSpacing(10);
      col = 0;
    }
  }
  if (row->count() > 0) {
    auto* roww = new QWidget(org_grid_empty_->parentWidget());
    auto* rl = new QHBoxLayout(roww);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(10);
    while (row->count() > 0) {
      QLayoutItem* it = row->takeAt(0);
      if (QWidget* w = it->widget()) {
        w->setParent(roww);
        w->show();
        rl->addWidget(w, 1);
      }
      delete it;
    }
    roww->show();
    org_grid_lay_->addWidget(roww);
  }
  delete row;
  org_grid_lay_->addStretch();
  org_grid_lay_->activate();
}

// 右栏成员档案：头像/职责行/状态徽标/汇报链路（沿 manager 指针上溯，
// 环防御）/档案 kv/动作钮（发消息、常用联系人星标）/数据口径注脚。
void MainWindow::show_org_profile(const QString& account) {
  org_selected_member_ = account;
  if (!org_profile_ || !org_profile_lay_) return;
  const ThemeTokens& t = tk();
  // 清空重建（三招模板）
  while (org_profile_lay_->count() > 0) {
    QLayoutItem* it = org_profile_lay_->takeAt(0);
    if (QWidget* w = it ? it->widget() : nullptr) {
      w->hide();
      w->deleteLater();
    }
    delete it;
  }
  const OrgMember* m = nullptr;
  for (const OrgMember& x : org_members_)
    if (x.account == account) m = &x;
  if (!m) { // 无选中（或成员不在可见集）：档案栏收起。只藏本栏——不回调
    // apply_view_mode（其 org 分支会再 rebuild_org_view → 再进本函数，
    // 空选中＝无限递归压栈，实录 SEGFAULT 在 QLabel 布局测量段）
    if (org_profile_) org_profile_->hide();
    org_profile_lay_->activate();
    return;
  }
  const bool online = online_accounts().contains(account);
  const bool starred = fav_is_starred_(account);
  const QString disp = m->name.isEmpty() ? m->account : m->name;
  auto add_label = [&](const QString& text, const QString& css) {
    auto* l = new QLabel(text, org_profile_);
    l->setStyleSheet(css);
    l->show();
    org_profile_lay_->addWidget(l);
    return l;
  };
  // 头像（64px，radius 18）＋名＋职责行
  auto* av = new QLabel(org_profile_);
  av->setFixedSize(64, 64);
  av->setAlignment(Qt::AlignCenter);
  av->setStyleSheet(
      QStringLiteral("background:%1; color:%2; border-radius:18px; "
                     "font-size:24px; font-weight:800;")
          .arg(t.brand_tint.name(), t.brand_text.name()));
  const QPixmap px =
      avatar_for(m->account, member_avatar_ver_.value(m->account), 64);
  if (!px.isNull()) av->setPixmap(px);
  else av->setText(avatar_initial(disp));
  av->show();
  org_profile_lay_->addWidget(av, 0, Qt::AlignLeft);
  add_label(disp, QStringLiteral("font-size:16px; font-weight:800; color:%1;")
                       .arg(t.text.name()));
  {
    QString duties = m->dept;
    if (!m->title.isEmpty())
      duties += duties.isEmpty() ? m->title
                                 : QStringLiteral(" · ") + m->title;
    if (duties.isEmpty()) duties = QStringLiteral("（未建档）");
    add_label(duties, QStringLiteral("font-size:11.5px; color:%1;")
                          .arg(t.text_muted.name()));
  }
  // 状态徽标行：在线/离线＋星标（真实状态）
  {
    auto* bl = new QHBoxLayout();
    bl->setSpacing(6);
    auto* badge = new QLabel(
        online ? QStringLiteral("● 在线") : QStringLiteral("● 离线"),
        org_profile_);
    badge->setStyleSheet(
        QStringLiteral("color:%1; font-size:11px; font-weight:700;")
            .arg(online ? t.success.name() : t.text_muted.name()));
    badge->show();
    bl->addWidget(badge);
    if (starred) {
      auto* sp = new QLabel(QStringLiteral("常用联系人"), org_profile_);
      sp->setStyleSheet(
          QStringLiteral("color:%1; border:1px solid %2; border-radius:999px;"
                         " padding:1px 8px; font-size:10.5px;")
              .arg(t.brand_text.name(), t.brand_tint.name()));
      sp->show();
      bl->addWidget(sp);
    }
    bl->addStretch();
    auto* bw = new QWidget(org_profile_);
    auto* bwl = new QHBoxLayout(bw);
    bwl->setContentsMargins(0, 0, 0, 0);
    while (bl->count() > 0) {
      QLayoutItem* it = bl->takeAt(0);
      if (QWidget* w = it->widget()) {
        w->setParent(bw);
        w->show();
        bwl->addWidget(w);
      }
      delete it;
    }
    delete bl;
    bw->show();
    org_profile_lay_->addWidget(bw);
  }
  // 汇报链路（沿 manager 上溯；环防御；缺失如实止步）
  {
    auto* head = add_label(
        QStringLiteral("汇报链路"),
        QStringLiteral("font-size:12px; font-weight:700; color:%1;")
            .arg(t.text_muted.name()));
    head->setContentsMargins(0, 8, 0, 0);
    QStringList chain; // account 逐级（本人在前，上级依次在后）
    QSet<QString> seen;
    QString cur = m->account;
    for (int hop = 0; hop < 6 && !cur.isEmpty() && !seen.contains(cur);
         ++hop) {
      seen.insert(cur);
      chain << cur;
      const OrgMember* x = nullptr;
      for (const OrgMember& y : org_members_)
        if (y.account == cur) x = &y;
      cur = x ? x->manager : QString();
    }
    for (int i = 0; i < chain.size(); ++i) {
      const OrgMember* x = nullptr;
      for (const OrgMember& y : org_members_)
        if (y.account == chain[i]) x = &y;
      const QString cname = x ? (x->name.isEmpty() ? x->account : x->name)
                              : chain[i];
      auto* ci = new QWidget(org_profile_);
      auto* cl = new QHBoxLayout(ci);
      cl->setContentsMargins(0, 0, 0, 0);
      cl->setSpacing(8);
      auto* cav = new QLabel(avatar_initial(cname), ci);
      cav->setFixedSize(26, 26);
      cav->setAlignment(Qt::AlignCenter);
      // 链路头像用令牌色（avatar_hue 散色进样式表会撞「样式全令牌」门禁）
      cav->setStyleSheet(
          QStringLiteral("background:%1; color:%2; border-radius:8px; "
                         "font-size:12px; font-weight:700;")
              .arg(t.brand_tint.name(), t.brand_text.name()));
      cav->show();
      cl->addWidget(cav);
      auto* cinfo = new QVBoxLayout();
      cinfo->setSpacing(0);
      auto* cn = new QLabel(cname, ci);
      cn->setStyleSheet(QStringLiteral("font-size:12.5px; font-weight:700; "
                                       "color:%1;")
                            .arg(t.text.name()));
      cn->show();
      cinfo->addWidget(cn);
      QString crole;
      if (x) {
        crole = x->dept.section(QChar('/'), -1);
        if (!x->title.isEmpty())
          crole += crole.isEmpty() ? x->title
                                   : QStringLiteral(" · ") + x->title;
      }
      if (i > 0 && crole.isEmpty()) crole = QStringLiteral("直属上级");
      if (!crole.isEmpty()) {
        auto* cr = new QLabel(crole, ci);
        cr->setStyleSheet(
            QStringLiteral("font-size:10.5px; color:%1;")
                .arg(t.text_muted.name()));
        cr->show();
        cinfo->addWidget(cr);
      }
      cl->addLayout(cinfo, 1);
      ci->show();
      org_profile_lay_->addWidget(ci);
      if (i + 1 < chain.size()) {
        auto* ln = new QLabel(QStringLiteral("│"), org_profile_);
        ln->setStyleSheet(QStringLiteral("color:%1; font-size:11px;")
                              .arg(t.border.name()));
        ln->setContentsMargins(0, 0, 0, 0);
        ln->show();
        org_profile_lay_->addWidget(ln, 0, Qt::AlignLeft);
      }
    }
  }
  // 档案 kv（真实字段：直属上级/部门/职务/在线时长；办公位置/手机协议
  // 未开——如实不摆，不造假行）
  {
    const auto kv = [&](const QString& k, QString v, bool brand = false) {
      auto* row = new QWidget(org_profile_);
      auto* rl = new QHBoxLayout(row);
      rl->setContentsMargins(0, 0, 0, 0);
      auto* kl = new QLabel(k, row);
      kl->setStyleSheet(QStringLiteral("font-size:11.5px; color:%1;")
                            .arg(t.text_muted.name()));
      kl->show();
      rl->addWidget(kl);
      rl->addStretch();
      // 缓存串自带「在线时长：」前缀（own_online 同源）——键名已给出，
      // 值里剥掉前缀免得键值重复；长值换行不截断
      if (v.startsWith(QStringLiteral("在线时长：")))
        v = v.mid(QStringLiteral("在线时长：").size());
      auto* vl = new QLabel(v, row);
      vl->setWordWrap(true);
      vl->setStyleSheet(QStringLiteral("font-size:11.5px; font-weight:600; "
                                       "color:%1;")
                            .arg(brand ? t.brand_text.name() : t.text.name()));
      vl->show();
      rl->addWidget(vl);
      row->show();
      org_profile_lay_->addWidget(row);
    };
    const auto mgr_of = [&](const QString& a) -> const OrgMember* {
      for (const OrgMember& y : org_members_)
        if (y.account == a) return &y;
      return nullptr;
    };
    if (!m->manager.isEmpty()) {
      const OrgMember* mgr = mgr_of(m->manager);
      kv(QStringLiteral("直属上级"),
         mgr ? (mgr->name.isEmpty() ? mgr->account : mgr->name) : m->manager,
         true);
    }
    if (!m->dept.isEmpty()) kv(QStringLiteral("部门"), m->dept);
    if (!m->title.isEmpty()) kv(QStringLiteral("职务"), m->title);
    const QString ol = member_online_.value(m->account);
    if (!ol.isEmpty()) kv(QStringLiteral("在线时长"), ol);
  }
  // 动作钮：发消息（brand）＋常用联系人星标（描边）
  {
    auto* acts = new QHBoxLayout();
    acts->setSpacing(8);
    auto* msg = new QPushButton(QStringLiteral("发消息"), org_profile_);
    msg->setCursor(Qt::PointingHandCursor);
    msg->setStyleSheet(
        QStringLiteral("QPushButton { background:%1; color:%2; border:none; "
                       "border-radius:8px; padding:7px 16px; "
                       "font-size:12.5px; font-weight:700; }"
                       "QPushButton:hover { background:%3; }")
            .arg(t.brand.name(), t.on_brand.name(), t.brand_text.name()));
    const QString acct = m->account;
    connect(msg, &QPushButton::clicked, this, [this, acct] {
      leave_org_view();
      open_collab_peer(acct);
    });
    msg->show();
    acts->addWidget(msg, 1);
    auto* fav = new QPushButton(starred ? QStringLiteral("取消常用联系人")
                                        : QStringLiteral("设为常用联系人"),
                                org_profile_);
    fav->setCursor(Qt::PointingHandCursor);
    fav->setStyleSheet(
        QStringLiteral("QPushButton { background:transparent; color:%1; "
                       "border:1px solid %2; border-radius:8px; "
                       "padding:7px 12px; font-size:12px; }"
                       "QPushButton:hover { background:%3; }")
            .arg(t.brand_text.name(), t.brand_tint.name(),
                 t.brand_wash.name()));
    connect(fav, &QPushButton::clicked, this, [this, acct, starred] {
      collab_engine_.fav_cmd(starred ? QStringLiteral("unstar")
                                     : QStringLiteral("star"),
                             acct);
    });
    fav->show();
    acts->addWidget(fav, 1);
    auto* aw = new QWidget(org_profile_);
    auto* awl = new QHBoxLayout(aw);
    awl->setContentsMargins(0, 4, 0, 0);
    while (acts->count() > 0) {
      QLayoutItem* it = acts->takeAt(0);
      if (QWidget* w = it->widget()) {
        w->setParent(aw);
        w->show();
        awl->addWidget(w, 1);
      }
      delete it;
    }
    delete acts;
    aw->show();
    org_profile_lay_->addWidget(aw);
  }
  // 数据口径注脚（直属上级为权威表字段——服务端真实模型）。注脚是长句，
  // 面板定宽下必须换行（QLabel 默认不换行＝右侧截断，实录截尾）
  if (auto* fn = add_label(
          QStringLiteral("直属上级为独立数据字段，由管理后台维护"
                         "（可批量导入），不从部门归属推导。"),
          QStringLiteral("font-size:10.5px; color:%1; line-height:1.6;")
              .arg(t.text_muted.name())))
    fn->setWordWrap(true);
  org_profile_lay_->addStretch();
  org_profile_lay_->activate();
  // 档案栏显隐就地收口（不回调 apply_view_mode——其 org 分支会再
  // rebuild_org_view → 再进本函数，选中有效成员同样成环）
  if (sidebar_mode_ == QStringLiteral("org")) org_profile_->show();
}

// 视图编排：传输面 ↔ 会话面互斥显隐。会话侧栏/聊天面板的各 setVisible
// 调用（update_side_head 等）以本函数收口——任何刷新后传输面显隐不被
// 侧栏控件显隐写穿。
void MainWindow::apply_view_mode() {
  const bool tx = sidebar_mode_ == QStringLiteral("transfers");
  const bool org = sidebar_mode_ == QStringLiteral("org");
  if (side_) side_->setVisible(!tx && !org);
  if (transfer_side_) transfer_side_->setVisible(tx);
  if (transfer_page_) transfer_page_->setVisible(tx);
  if (org_side_) org_side_->setVisible(org);
  if (org_page_) org_page_->setVisible(org);
  // 档案栏随选中态（原型 .profile-side：选中成员即在；未选中不占位）
  if (org_profile_)
    org_profile_->setVisible(org && !org_selected_member_.isEmpty());
  if (tx) {
    if (chat_panel_) chat_panel_->hide();
    if (group_panel_) group_panel_->hide();
    rebuild_transfer_side();
    refresh_transfers_view();
  }
  if (org) {
    if (chat_panel_) chat_panel_->hide();
    if (group_panel_) group_panel_->hide();
    // 不在此 rebuild：进图（enter_org_view）／数据（ORG_DATA）／主题
    // （apply_theme_styles）三入口各自触发；此处若重建会与
    // show_org_profile 尾部的显隐回调成环（无限递归实录）
  }
  // 显隐切换后立即重算中栏几何：不靠事件循环消化 LayoutRequest（截图腿
  // 曾抓到未结算的布局——传输侧栏被排到聊天面板旧位之后，窗中留空洞）
  if (QWidget* c = centralWidget()) {
    if (QLayout* l = c->layout()) l->activate();
  }
}

// 台账入账：同 id 覆盖（进度终态同键回流）；传输面可见即刷新。
void MainWindow::track_transfer(const TransferEntry& e) {
  if (e.id.isEmpty()) return;
  bool replaced = false;
  for (TransferEntry& old : transfers_) {
    if (old.id == e.id) {
      old = e;
      replaced = true;
      break;
    }
  }
  if (!replaced) transfers_.append(e);
  if (sidebar_mode_ == QStringLiteral("transfers")) refresh_transfers_view();
}

// 进度差分：字节累计＋瞬时速度（两次进度事件的字节差/时差）。
void MainWindow::update_tx_progress(const QString& id, quint64 done,
                                    quint64 total) {
  if (id.isEmpty()) return;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (TransferEntry& e : transfers_) {
    if (e.id != id) continue;
    if (e.last_prog_ms > 0 && done > e.bytes_done) {
      const double dt = double(now - e.last_prog_ms);
      if (dt > 0) e.speed_bps = (done - e.bytes_done) * 1000.0 / dt;
    }
    e.bytes_done = done;
    e.bytes_total = total;
    e.last_prog_ms = now;
    break;
  }
  if (sidebar_mode_ == QStringLiteral("transfers")) refresh_transfers_view();
}

// 终态：ok＝已完成（字节收满＋完成时刻）；失败且非取消＝已中断（原因入
// 副注）；已取消的终态回执不改判（用户主动中止，取消即终态语义）。
void MainWindow::finish_tx_entry(const QString& id, bool ok,
                                 const QString& note) {
  if (id.isEmpty()) return;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (TransferEntry& e : transfers_) {
    if (e.id != id) continue;
    if (ok) {
      e.state = 1;
      e.done_ms = now;
      e.speed_bps = 0;
      if (e.bytes_total > 0) e.bytes_done = e.bytes_total;
      if (e.is_folder && e.files_total > 0) e.files_done = e.files_total;
    } else if (e.state != 3) {
      e.state = 2;
      e.note = note;
    }
    break;
  }
  if (sidebar_mode_ == QStringLiteral("transfers")) refresh_transfers_view();
}

// 传输卡对端显示名：协作＝通讯录显示名（空回落账号）；直连＝设备号
//（直连态无显示名映射，同会话列表行口径）。
QString MainWindow::tx_peer_display(const QString& kind,
                                    const QString& peer) const {
  if (kind == QStringLiteral("collab")) {
    const QString d = roster_display(peer);
    return d.isEmpty() ? peer : d;
  }
  return peer;
}

// 侧栏「传输通道」全量重建（行/卡样式内嵌令牌色：主题切换与进入视图都
// 走这里）。通道行＝点对点直传（当前唯一可用通道，active 态）＋共享空间
//（全工程无功能体，行落位但置灰——同 rail 共享空间钮口径，不冒充可用）。
void MainWindow::rebuild_transfer_side() {
  if (!transfer_side_) return;
  auto* lay = qobject_cast<QVBoxLayout*>(transfer_side_->layout());
  if (!lay) return;
  const ThemeTokens& t = tk();
  while (lay->count() > 0) {
    QLayoutItem* it = lay->takeAt(0);
    if (it->widget()) {
      // 先摘画面再延后销毁：DeferredDelete 在嵌套泵里可能迟迟不消化，
      // 旧行残留绘制会盖住重建内容（首代行浅色残影实录）
      it->widget()->hide();
      it->widget()->deleteLater();
    }
    delete it;
  }
  auto* ts_title = new QLabel(QStringLiteral("传输通道"), transfer_side_);
  ts_title->setStyleSheet(QStringLiteral("font-weight:600; font-size:14px; "
                                         "color:%1;")
                              .arg(t.text.name()));
  // 已可见父下新建的子件不随 addWidget 自动显示——逐件显式 show
  ts_title->show();
  lay->addWidget(ts_title);
  // 通道行（原型 .side-list .row：40px 头像 radius 12＋名/副行）
  const auto chan_row = [&t](QWidget* parent, const QString& initial,
                             const QString& name, const QString& sub,
                             bool active) {
    auto* row = new QWidget(parent);
    auto* rl = new QHBoxLayout(row);
    rl->setContentsMargins(8, 6, 8, 6);
    rl->setSpacing(10);
    auto* av = new QLabel(initial, row);
    av->setFixedSize(40, 40);
    av->setAlignment(Qt::AlignCenter);
    // 通道行头像用令牌色（avatar_hue 散色进样式表会撞「样式全令牌」门禁）
    av->setStyleSheet(QStringLiteral("background:%1; color:%2; font-weight:700;"
                                     " font-size:15px; border-radius:12px;")
                          .arg(active ? t.brand_tint.name()
                                      : t.surface_alt.name(),
                               active ? t.brand_text.name()
                                      : t.text_muted.name()));
    rl->addWidget(av, 0, Qt::AlignTop);
    auto* main = new QWidget(row);
    auto* ml = new QVBoxLayout(main);
    ml->setContentsMargins(0, 0, 0, 0);
    ml->setSpacing(2);
    auto* name_l = new QLabel(name, main);
    name_l->setStyleSheet(
        QStringLiteral("font-weight:600; font-size:13px; color:%1;")
            .arg(active ? t.text.name() : t.text_muted.name()));
    ml->addWidget(name_l);
    auto* sub_l = new QLabel(sub, main);
    sub_l->setStyleSheet(QStringLiteral("font-size:11px; color:%1;")
                             .arg(t.text_muted.name()));
    ml->addWidget(sub_l);
    rl->addWidget(main, 1);
    if (active) {
      row->setStyleSheet(QStringLiteral("background:%1; border-radius:10px;")
                             .arg(t.brand_tint.name()));
    }
    return row;
  };
  auto* peer_row = chan_row(transfer_side_, QStringLiteral("⇅"),
                            QStringLiteral("点对点直传"),
                            QStringLiteral("默认 · 不经服务端"), true);
  peer_row->show();
  lay->addWidget(peer_row);
  auto* share_row =
      chan_row(transfer_side_, QStringLiteral("档"), QStringLiteral("共享空间"),
               QStringLiteral("Samba · 未接入"), false);
  share_row->setToolTip(QStringLiteral("共享空间无功能体（本版仅点对点直传）"));
  share_row->show();
  lay->addWidget(share_row);
  // 旁路设计虚线卡（原型同位卡：口径如实——字节流点对点，协作态授权
  // 留痕在服务端，直连态完全离线）
  auto* bypass = new QLabel(
      QStringLiteral("<span style=\"font-weight:700; color:%1;\">旁路设计"
                     "</span>：文件字节流始终在局域网内点对点直传，不经服务"
                     "端；协作态发送前经服务端授权留痕（文件名、大小、哈希、"
                     "收发方）。")
          .arg(t.text.name()),
      transfer_side_);
  bypass->setWordWrap(true);
  bypass->setStyleSheet(
      QStringLiteral("border:1px dashed %1; border-radius:10px; "
                     "padding:11px 12px; font-size:11px; color:%2;")
          .arg(t.surface_alt.name(), t.text_muted.name()));
  bypass->show();
  lay->addWidget(bypass);
  // 伙伴发现品牌卡：N 取发现表实时数（每次进视图/发现表变化刷新）
  tx_peers_hint_ = new QLabel(
      QStringLiteral("本机已发现同网段直传伙伴 %1 台。")
          .arg(static_cast<int>(direct_engine_.peers().size())),
      transfer_side_);
  tx_peers_hint_->setWordWrap(true);
  tx_peers_hint_->setStyleSheet(
      QStringLiteral("background:%1; color:%2; border-radius:10px; "
                     "padding:11px 12px; font-size:11px;")
          .arg(t.brand_wash.name(), t.brand_wash_text.name()));
  tx_peers_hint_->show();
  lay->addWidget(tx_peers_hint_);
  lay->addStretch();
  // 重建后同步结算布局（同 refresh_transfers_view：不等事件泵消化
  // LayoutRequest，新行滞留默认几何实录）
  lay->activate();
}

// 传输列表全量重建：头部 meta（正在传输/今日完成/累计，全部取自真实
// 台账）＋标签（计数＋active 态）＋传输卡（图标/名/副行/状态胶囊/进度条/
// 脚行/操作）。空态与「已暂停」标签如实说明（台账会话内存级；引擎无
// 暂停 API）。
void MainWindow::refresh_transfers_view() {
  if (!tx_list_lay_ || !tx_meta_) return;
  const ThemeTokens& t = tk();
  int doing = 0;
  int done_today = 0;
  quint64 total_bytes = 0;
  const qint64 day_start_ms =
      QDateTime(QDate::currentDate(), QTime(0, 0)).toMSecsSinceEpoch();
  for (const TransferEntry& e : transfers_) {
    if (e.state == 0) ++doing;
    if (e.state == 1) {
      total_bytes += e.bytes_total;
      if (e.done_ms >= day_start_ms) ++done_today;
    }
  }
  tx_meta_->setText(QStringLiteral("正在传输 %1 · 今日完成 %2 · 累计 %3")
                        .arg(QString::number(doing),
                             QString::number(done_today),
                             human_size(static_cast<qint64>(total_bytes))));
  tx_meta_->setStyleSheet(QStringLiteral("color:%1; font-size:12px;")
                              .arg(t.text_muted.name()));
  tx_tabs_[1]->setText(QStringLiteral("传输中 %1").arg(doing));
  for (int i = 0; i < 4; ++i) {
    if (!tx_tabs_[i]) continue;
    if (i == transfer_filter_) {
      tx_tabs_[i]->setStyleSheet(
          QStringLiteral("background:%1; color:%2; border:none; "
                         "border-radius:8px; padding:6px 14px; "
                         "font-size:12.5px; font-weight:700;")
              .arg(t.brand_wash.name(), t.brand_text.name()));
    } else {
      tx_tabs_[i]->setStyleSheet(
          QStringLiteral("background:transparent; color:%1; border:none; "
                         "border-radius:8px; padding:6px 14px; "
                         "font-size:12.5px;")
              .arg(t.text_muted.name()));
    }
  }
  // 清空重建（tx_empty_ 常驻复用，卡片销毁重建）。先 hide 再延后销毁：
  // DeferredDelete 在嵌套事件泵里可能迟迟不消化，旧卡残留绘制会盖住重建
  // 内容（首代浅色卡在深色抓图里残影实录）
  while (tx_list_lay_->count() > 0) {
    QLayoutItem* it = tx_list_lay_->takeAt(0);
    if (QWidget* w = it ? it->widget() : nullptr) {
      if (w != tx_empty_) {
        w->hide();
        w->deleteLater();
      }
    }
    delete it;
  }
  QList<const TransferEntry*> shown;
  for (const TransferEntry& e : transfers_) {
    const bool pass = transfer_filter_ == 0
                          ? true
                          : transfer_filter_ == 1
                                ? e.state == 0
                                : transfer_filter_ == 2 ? e.state == 1
                                                        : false;
    if (pass) shown.append(&e);
  }
  std::sort(shown.begin(), shown.end(),
            [](const TransferEntry* a, const TransferEntry* b) {
              const bool ad = a->state == 0, bd = b->state == 0;
              if (ad != bd) return ad; // 传输中置顶（原型首卡在传）
              const qint64 ta = a->state == 1 ? a->done_ms : a->ts_ms;
              const qint64 tb = b->state == 1 ? b->done_ms : b->ts_ms;
              return ta > tb; // 新事件在上
            });
  if (shown.isEmpty()) {
    tx_empty_->setText(
        transfer_filter_ == 3
            ? QStringLiteral("本版传输不支持暂停——取消后重发自动断点续传"
                             "（对端保留已传分块与哈希校验点）")
            : QStringLiteral("暂无传输记录（会话内发送/接收的文件在此列出；"
                             "台账为本机本次运行内，重启从零累计）"));
    tx_empty_->setStyleSheet(QStringLiteral("color:%1; font-size:11.5px;")
                                 .arg(t.text_muted.name()));
    tx_empty_->show();
    tx_list_lay_->addWidget(tx_empty_);
    tx_list_lay_->addStretch();
    return;
  }
  const QString op_plain = QStringLiteral(
      "QPushButton { background:transparent; color:%1; border:1px solid %2; "
      "border-radius:6px; padding:2px 9px; font-size:11.5px; }"
      "QPushButton:hover { background:%3; }");
  const QString plain_a =
      op_plain.arg(t.text_muted.name(), t.border.name(), t.surface_alt.name());
  const QString brand_a =
      op_plain.arg(t.brand_text.name(), t.brand_tint.name(),
                   t.brand_wash.name());
  const QString muted_l =
      QStringLiteral("font-size:11.5px; color:%1;").arg(t.text_muted.name());
  for (const TransferEntry* pe : shown) {
    const TransferEntry& e = *pe;
    auto* card = new QFrame(tx_empty_->parentWidget());
    // 纵向封顶＝按内容高：滚动区可视高大于内容总高时，多余空间全让
    // 给尾部 stretch，卡片不被拉高（首版实况被拉成半空高卡实录）
    card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    card->setStyleSheet(
        QStringLiteral("QFrame { background:%1; border:1px solid %2; "
                       "border-radius:10px; }")
            .arg(t.surface_raised.name(), t.border.name()));
    auto* cl = new QVBoxLayout(card);
    cl->setContentsMargins(15, 13, 15, 13);
    cl->setSpacing(8);
    // 首行：图标＋名/副行＋状态胶囊（原型 .tx-top）
    auto* top = new QHBoxLayout();
    top->setSpacing(10);
    const bool ok_look = e.state == 1;
    auto* icon = new QLabel(
        e.is_folder ? QStringLiteral("▸") : avatar_initial(e.name), card);
    icon->setFixedSize(36, 36);
    icon->setAlignment(Qt::AlignCenter);
    icon->setStyleSheet(
        QStringLiteral("background:%1; color:%2; border-radius:10px; "
                       "font-size:15px; font-weight:800;")
            .arg((e.is_folder || ok_look) ? t.success_wash.name()
                                          : t.brand_tint.name(),
                 (e.is_folder || ok_look) ? t.success.name()
                                          : t.brand_text.name()));
    top->addWidget(icon, 0, Qt::AlignTop);
    auto* tit = new QVBoxLayout();
    tit->setSpacing(1);
    QString name_text = e.name;
    if (e.is_folder) { // 原型 .f-name「目录名/（文件夹 · N 个文件）」
      name_text += QStringLiteral("/（文件夹");
      if (e.files_total > 0)
        name_text += QStringLiteral(" · %1 个文件").arg(e.files_total);
      name_text += QStringLiteral("）");
    }
    auto* name_l = new QLabel(name_text, card);
    name_l->setStyleSheet(
        QStringLiteral("font-weight:700; font-size:13.5px; color:%1;")
            .arg(t.text.name()));
    tit->addWidget(name_l);
    const QString dir_word =
        e.outgoing ? QStringLiteral("发给") : QStringLiteral("来自");
    QString sub;
    if (e.state == 1) {
      sub = QStringLiteral("%1 %2 · %3 · 完成 %4")
                .arg(dir_word, e.peer_display,
                     human_size(static_cast<qint64>(e.bytes_total)),
                     hhmm(e.done_ms));
    } else if (e.state == 2) {
      sub = QStringLiteral("%1 %2 · %3")
                .arg(dir_word, e.peer_display,
                     e.note.isEmpty() ? QStringLiteral("传输中断") : e.note);
    } else if (e.state == 3) {
      sub = QStringLiteral("%1 %2 · 已取消").arg(dir_word, e.peer_display);
    } else {
      sub = QStringLiteral("%1 %2 · 直传中").arg(dir_word, e.peer_display);
    }
    auto* sub_l = new QLabel(sub, card);
    sub_l->setStyleSheet(muted_l);
    tit->addWidget(sub_l);
    top->addLayout(tit, 1);
    QString pill_text;
    QColor pill_fg, pill_bg;
    switch (e.state) {
      case 1:
        pill_text = QStringLiteral("已完成");
        pill_fg = t.success;
        pill_bg = t.success_wash;
        break;
      case 2:
        pill_text = QStringLiteral("已中断");
        pill_fg = t.warning;
        pill_bg = t.warning_wash;
        break;
      case 3:
        pill_text = QStringLiteral("已取消");
        pill_fg = t.text_muted;
        pill_bg = t.surface_alt;
        break;
      default:
        pill_text = QStringLiteral("传输中");
        pill_fg = t.brand_text;
        pill_bg = t.brand_tint;
    }
    auto* pill = new QLabel(pill_text, card);
    pill->setStyleSheet(
        QStringLiteral("color:%1; background:%2; border-radius:999px; "
                       "padding:1px 8px; font-size:10.5px; font-weight:700;")
            .arg(pill_fg.name(), pill_bg.name()));
    top->addWidget(pill, 0, Qt::AlignTop);
    cl->addLayout(top);
    // 进度条（原型 .tx-bar：6px；完成＝ok 色）
    int pct = 0;
    if (e.bytes_total > 0)
      pct = static_cast<int>(e.bytes_done * 100 / e.bytes_total);
    if (e.state == 1) pct = 100;
    auto* bar = new QProgressBar(card);
    bar->setTextVisible(false);
    bar->setFixedHeight(6);
    bar->setRange(0, 100);
    bar->setValue(pct);
    bar->setStyleSheet(
        QStringLiteral("QProgressBar { background:%1; border:1px solid %2; "
                       "border-radius:3px; }"
                       "QProgressBar::chunk { background:%3; "
                       "border-radius:3px; }")
            .arg(t.surface_alt.name(), t.border.name(),
                 e.state == 1 ? t.success.name() : t.brand.name()));
    cl->addWidget(bar);
    // 脚行：字节/速度/文件粒度＋说明＋操作（原型 .tx-foot/.ops）
    auto* foot = new QHBoxLayout();
    foot->setSpacing(10);
    QString left;
    if (e.is_folder) {
      left = QStringLiteral("文件夹 · 已传 %1/%2 个文件")
                 .arg(QString::number(e.files_done),
                      QString::number(e.files_total));
    } else if (e.bytes_total > 0) {
      left = QStringLiteral("%1 / %2 · %3%")
                 .arg(human_size(static_cast<qint64>(e.bytes_done)),
                      human_size(static_cast<qint64>(e.bytes_total)),
                      QString::number(pct));
    } else {
      left = QStringLiteral("待对端上报大小");
    }
    auto* left_l = new QLabel(left, card);
    left_l->setStyleSheet(muted_l);
    foot->addWidget(left_l);
    if (e.state == 0 && e.speed_bps > 0 && e.bytes_total > e.bytes_done) {
      const double remain_s =
          double(e.bytes_total - e.bytes_done) / e.speed_bps;
      const QString remain =
          remain_s >= 60
              ? QStringLiteral("%1 分 %2 秒")
                    .arg(int(remain_s / 60))
                    .arg(int(remain_s) % 60)
              : QStringLiteral("%1 秒").arg(int(remain_s) + 1);
      auto* speed_l =
          new QLabel(QStringLiteral("%1/s · 剩余 %2")
                         .arg(human_size(static_cast<qint64>(e.speed_bps)),
                              remain),
                     card);
      speed_l->setStyleSheet(muted_l);
      foot->addWidget(speed_l);
    }
    if (e.state == 1) {
      // 收方整文件 SHA-256 校验通过才落盘/回执——双向都可如实标校验
      auto* ok_l = new QLabel(
          e.outgoing ? QStringLiteral("✓ 接收方已确认 · SHA-256 已校验")
                     : QStringLiteral("✓ 已保存至本机 · SHA-256 已校验"),
          card);
      ok_l->setStyleSheet(
          QStringLiteral("font-size:11.5px; color:%1;").arg(t.success.name()));
      foot->addWidget(ok_l);
    }
    if (e.state == 3) {
      auto* c_l =
          new QLabel(QStringLiteral("对端保留已传分块，重发自动续传"), card);
      c_l->setStyleSheet(muted_l);
      foot->addWidget(c_l);
    }
    foot->addStretch();
    // 操作：描边小钮（品牌色＝打开/打开目录）；取消走引擎中止＋台账改判
    const auto op_btn = [&](const QString& text, bool brand,
                            const std::function<void()>& on_click) {
      auto* b = new QPushButton(text, card);
      b->setCursor(Qt::PointingHandCursor);
      b->setStyleSheet(brand ? brand_a : plain_a);
      connect(b, &QPushButton::clicked, this, on_click);
      foot->addWidget(b);
    };
    if (e.state == 0) {
      op_btn(QStringLiteral("取消"), false, [this, id = e.id] {
        for (TransferEntry& en : transfers_) {
          if (en.id == id && en.state == 0) {
            en.state = 3;
            en.note = QStringLiteral("已取消");
          }
        }
        direct_engine_.cancel_transfer(id.toStdString());
        refresh_transfers_view();
      });
      if (e.outgoing && !e.path.isEmpty()) {
        op_btn(QStringLiteral("打开目录"), true, [path = e.path] {
          QDesktopServices::openUrl(QUrl::fromLocalFile(
              QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath()));
        });
      }
    } else if (e.state == 1) {
      if (!e.path.isEmpty()) {
        const QString label = (e.is_folder || !e.outgoing)
                                  ? QStringLiteral("打开目录")
                                  : QStringLiteral("打开");
        op_btn(label, true, [is_folder = e.is_folder, path = e.path] {
          QDesktopServices::openUrl(QUrl::fromLocalFile(
              is_folder ? path : QFileInfo(path).absolutePath()));
        });
      }
      op_btn(QStringLiteral("所在会话"), false,
             [this, kind = e.kind, peer = e.peer] {
               leave_transfers_view();
               open_chat(kind, peer);
             });
    }
    cl->addLayout(foot);
    // 已可见父下新建的子件不随 addWidget 自动显示——显式 show
    card->show();
    tx_list_lay_->addWidget(card);
  }
  tx_list_lay_->addStretch();
  // 重建后同步结算布局：新增卡在脏布局上等 LayoutRequest 事件，嵌套事件
  // 泵里可能迟消化——曾实录第二张卡滞留默认几何 (0,0 100x30) 不入画
  tx_list_lay_->activate();
}

// 传输台账验收面：逐条 JSON（id/名/形态/对端/状态/方向/文件夹/字节）。
QString MainWindow::transfers_json() const {
  QJsonArray arr;
  for (const TransferEntry& e : transfers_) {
    QJsonObject o;
    o.insert(QStringLiteral("id"), e.id);
    o.insert(QStringLiteral("name"), e.name);
    o.insert(QStringLiteral("kind"), e.kind);
    o.insert(QStringLiteral("peer"), e.peer);
    o.insert(QStringLiteral("state"), e.state);
    o.insert(QStringLiteral("outgoing"), e.outgoing);
    o.insert(QStringLiteral("folder"), e.is_folder);
    o.insert(QStringLiteral("bytes"), QString::number(e.bytes_total));
    arr.append(o);
  }
  QJsonObject root;
  root.insert(QStringLiteral("transfers"), arr);
  return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

// 发文件发送缝（传输面「发送文件」钮与聊天发文件钮同一路径）：守卫→
// 直连通道发送→台账跟踪＋会话内文件卡。
QString MainWindow::transfer_send_file(const QString& path) {
  if (current_peer_.isEmpty()) {
    show_status(QStringLiteral("先选择设备再发送文件"));
    return {};
  }
  if (current_kind_ == QStringLiteral("group") ||
      current_kind_ == QStringLiteral("dgroup")) {
    // 群文件传输不在本期范围：文件仍走点对点直连（单聊）
    show_status(QStringLiteral("群会话暂不支持文件发送（文件走单聊点对点）"));
    return {};
  }
  if (current_kind_ != QStringLiteral("collab") && !direct_send_allowed()) {
    return {}; // T3.4 策略闸门（文案策略面已给）
  }
  const std::string tid =
      direct_engine_.send_file(current_peer_.toStdString(), path);
  if (tid.empty()) {
    show_status(QStringLiteral("文件发送失败"));
    return {};
  }
  const QString id = QString::fromStdString(tid);
  const QString name = QFileInfo(path).fileName();
  file_sent_.insert(name, 0);
  TransferEntry e;
  e.id = id;
  e.name = name;
  e.kind = current_kind_;
  e.peer = current_peer_;
  e.peer_display = tx_peer_display(e.kind, e.peer);
  e.path = path;
  e.bytes_total = static_cast<quint64>(QFileInfo(path).size());
  e.ts_ms = QDateTime::currentMSecsSinceEpoch();
  track_transfer(e);
  // 文件卡气泡（原型 .bubble.file-card）：协作单聊＝传输元数据入归档，
  // 直连/跨态＝仅双方本机保存（文案如实随形态，不虚构归档）
  append_file_card(
      QString(), name,
      QStringLiteral("%1 · 直传 · %2")
          .arg(human_size(QFileInfo(path).size()),
               (current_kind_ == QStringLiteral("collab") &&
                collab_engine_.is_logged_in())
                   ? QStringLiteral("传输元数据已入归档")
                   : QStringLiteral("仅双方本机保存")),
      QDateTime::currentMSecsSinceEpoch(), /*outgoing=*/true);
  show_status(QStringLiteral("文件发送中：%1").arg(name));
  return id;
}

// 发起单聊（原型 side-head + 钮）：候选＝组织架构成员（未拉到回落
// 会话表∪在线表，去重去自己），选中即开该协作会话
void MainWindow::show_new_chat_menu() {
  if (!collab_engine_.is_logged_in()) return;
  QStringList accounts;
  const QJsonDocument od = QJsonDocument::fromJson(last_org_json_.toUtf8());
  if (od.isObject()) {
    for (const auto& m :
         od.object().value(QStringLiteral("members")).toArray()) {
      accounts << m.toObject().value(QStringLiteral("account")).toString();
    }
  }
  if (accounts.isEmpty()) {
    accounts = collab_peers_.values() + online_accounts_.values();
  }
  accounts.removeAll(collab_engine_.account());
  accounts.removeDuplicates();
  accounts.sort();
  if (accounts.isEmpty()) {
    show_status(QStringLiteral("暂无可发起会话的联系人（组织架构未下发）"));
    return;
  }
  QMenu menu(this);
  for (const QString& a : accounts) {
    QAction* act = menu.addAction(a);
    connect(act, &QAction::triggered, this, [this, a] {
      open_chat(QStringLiteral("collab"), a);
    });
  }
  menu.exec(new_chat_btn_->mapToGlobal(
      QPoint(0, new_chat_btn_->height())));
}

// 群条目右键菜单：服务端群（拉人／公告／退群）与临时群（解散）
void MainWindow::show_group_menu(const QPoint& pos) {
  auto* item = device_list_->itemAt(pos);
  if (!item) return;
  const QString kind = item->data(Qt::UserRole + 1).toString();
  const QString id = item->data(Qt::UserRole).toString();
  QMenu menu(this);
  if (kind == QStringLiteral("collab") || kind == QStringLiteral("group")) {
    // T4.5：常用联系人星标／取消（group kind 的 id 形如 "group:N"，直接用）
    const bool starred = fav_is_starred_(id);
    auto* act_star = menu.addAction(
        starred ? QStringLiteral("取消星标") : QStringLiteral("星标（置顶）"));
    connect(act_star, &QAction::triggered, this, [this, id, starred] {
      if (!collab_engine_.is_logged_in()) {
        show_status(QStringLiteral("未登录协作态，常用联系人不可用"));
        return;
      }
      collab_engine_.fav_cmd(starred ? QStringLiteral("unstar")
                                     : QStringLiteral("star"),
                             id);
    });
    // 需求批⑦：会话级已读回执开关（显式 true/false 覆盖全局缺省）
    auto* act_receipt =
        menu.addAction(QStringLiteral("发送已读回执（本会话）"));
    act_receipt->setCheckable(true);
    act_receipt->setChecked(read_receipts_enabled(id));
    connect(act_receipt, &QAction::toggled, this,
            [this, id](bool on) { set_read_receipts_enabled(id, on); });
    menu.addSeparator();
  }
  if (kind == QStringLiteral("group")) {
    const quint64 gid = id.mid(QStringLiteral("group:").size()).toULongLong();
    auto* act_invite = menu.addAction(QStringLiteral("拉人进群…"));
    auto* act_ann = menu.addAction(QStringLiteral("设置群公告…"));
    auto* act_profile = menu.addAction(QStringLiteral("设置群资料…"));
    auto* act_hist = menu.addAction(QStringLiteral("公告编辑历史…"));
    auto* act_memo = menu.addAction(QStringLiteral("群备忘录…"));
    auto* act_vault = menu.addAction(QStringLiteral("群密码箱…"));
    auto* act_ci = menu.addAction(QStringLiteral("CI/CD 流水线…"));
    auto* act_pack = menu.addAction(QStringLiteral("打包与导出…"));
    auto* act_record = menu.addAction(QStringLiteral("导出群记录…"));
    auto* act_server = menu.addAction(QStringLiteral("服务器…"));
    menu.addSeparator();
    auto* act_leave = menu.addAction(QStringLiteral("退出群聊"));
    connect(act_invite, &QAction::triggered, this,
            [this, gid] { group_invite_dialog(gid); });
    connect(act_ann, &QAction::triggered, this,
            [this, gid] { group_announce_dialog(gid); });
    connect(act_profile, &QAction::triggered, this,
            [this, gid] { group_profile_dialog(gid); });
    connect(act_hist, &QAction::triggered, this,
            [this, gid] { group_announce_history_dialog(gid); });
    connect(act_memo, &QAction::triggered, this, [this, gid] {
      // R24-2 群备忘录：独立文件面会话窗口（懒建复用，换群重拉）
      const auto it = groups_.find(gid);
      if (!group_memo_) {
        group_memo_ = new GroupMemoDialog(this);
      }
      group_memo_->set_group(gid, it != groups_.end() ? it->name
                                                     : QStringLiteral("群 %1").arg(gid));
      group_memo_->show();
      group_memo_->raise();
      group_memo_->activateWindow();
    });
    connect(act_vault, &QAction::triggered, this, [this, gid] {
      // R24-3 群密码箱：独立文件面会话窗口（懒建复用，换群重拉）
      const auto it = groups_.find(gid);
      if (!group_vault_) {
        group_vault_ = new GroupVaultDialog(this);
      }
      group_vault_->set_group(gid, it != groups_.end() ? it->name
                                                       : QStringLiteral("群 %1").arg(gid));
      group_vault_->show();
      group_vault_->raise();
      group_vault_->activateWindow();
    });
    connect(act_ci, &QAction::triggered, this, [this, gid] {
      // R25-2 群 CI/CD：独立文件面会话窗口（懒建复用，换群重拉）
      const auto it = groups_.find(gid);
      if (!group_ci_) {
        group_ci_ = new GroupCiDialog(this);
      }
      group_ci_->set_group(gid, it != groups_.end() ? it->name
                                                    : QStringLiteral("群 %1").arg(gid));
      group_ci_->show();
      group_ci_->raise();
      group_ci_->activateWindow();
    });
    connect(act_pack, &QAction::triggered, this, [this, gid] {
      // R25-3 打包/导出：独立文件面会话窗口（懒建复用，换群重拉）
      const auto it = groups_.find(gid);
      if (!group_pack_) {
        group_pack_ = new GroupPackDialog(this);
      }
      group_pack_->set_group(gid, it != groups_.end() ? it->name
                                                      : QStringLiteral("群 %1").arg(gid));
      group_pack_->show();
      group_pack_->raise();
      group_pack_->activateWindow();
    });
    connect(act_record, &QAction::triggered, this,
            [this, gid] { group_record_export_dialog(gid); });
    connect(act_server, &QAction::triggered, this, [this, gid] {
      // R26-2 群服务器面板：独立文件面会话窗口（懒建复用，换群重拉）
      const auto it = groups_.find(gid);
      if (!group_server_) {
        group_server_ = new GroupServerDialog(this);
      }
      group_server_->set_group(gid, it != groups_.end() ? it->name
                                                        : QStringLiteral("群 %1").arg(gid));
      group_server_->show();
      group_server_->raise();
      group_server_->activateWindow();
    });
    connect(act_leave, &QAction::triggered, this, [this, gid] {
      if (QMessageBox::question(
              this, QStringLiteral("退出群聊"),
              QStringLiteral("确认退出该群？（群主退群＝解散该群）")) !=
          QMessageBox::Yes) {
        return;
      }
      collab_engine_.leave_group(gid);
    });
    menu.exec(device_list_->mapToGlobal(pos));
  } else if (kind == QStringLiteral("dgroup")) {
    auto* act_del = menu.addAction(QStringLiteral("解散临时群"));
    connect(act_del, &QAction::triggered, this, [this, id] {
      dgroup_members_.remove(id);
      if (current_peer_ == id) {
        current_kind_ = QStringLiteral("direct");
        current_peer_.clear();
      }
      refresh_devices();
      show_status(QStringLiteral("临时群已解散（本就不进归档，无服务端动作）"));
    });
    menu.exec(device_list_->mapToGlobal(pos));
  }
}

// 拉人进群（成员数据源同建群；服务端校验账号存在且不在群里）
void MainWindow::group_invite_dialog(quint64 group_id) {
  if (!groups_.contains(group_id)) return;
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("拉人进群（群 %1）").arg(group_id));
  dlg.resize(380, 480);
  auto* layout = new QVBoxLayout(&dlg);
  auto* members = new QListWidget(&dlg);
  const QString me = collab_engine_.account();
  const QStringList in_group = groups_.value(group_id).members;
  for (const QString& a : org_accounts()) {
    if (a == me || in_group.contains(a)) continue;
    auto* item = new QListWidgetItem(a, members);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
  }
  auto* extra = new QLineEdit(&dlg);
  extra->setPlaceholderText(QStringLiteral("其他账号，逗号分隔（可空）"));
  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  layout->addWidget(members);
  layout->addWidget(extra);
  layout->addWidget(buttons);
  if (dlg.exec() != QDialog::Accepted) return;
  QStringList picked;
  for (int i = 0; i < members->count(); ++i) {
    if (members->item(i)->checkState() == Qt::Checked) {
      picked << members->item(i)->text();
    }
  }
  for (const QString& part :
       extra->text().split(QChar(','), Qt::SkipEmptyParts)) {
    const QString a = part.trimmed();
    if (!a.isEmpty() && !picked.contains(a)) picked << a;
  }
  if (picked.isEmpty()) {
    show_status(QStringLiteral("拉人取消：未选择账号"));
    return;
  }
  collab_engine_.invite_group(group_id, picked);
  show_status(QStringLiteral("拉人请求已发送（%1 人）").arg(picked.size()));
}

// 群公告（群主/管理员可设，R24-1 权限放宽；服务端校验并回执结果）
void MainWindow::group_announce_dialog(quint64 group_id) {
  const auto it = groups_.find(group_id);
  if (it == groups_.end()) return;
  bool ok = false;
  const QString text = QInputDialog::getMultiLineText(
      this, QStringLiteral("设置群公告（群 %1）").arg(group_id),
      QStringLiteral("群主/管理员可设；留空＝清除公告"), it->announcement, &ok);
  if (!ok) return;
  collab_engine_.announce_group(group_id, text.trimmed());
  show_status(QStringLiteral("群公告设置请求已发送（全员将收到重要级通知）"));
}

// 群资料（群组-1 主题/目的）编辑：权沿群公告先例（群主/管理员，服务端
// 裁决回执）；两字段整组覆盖＝留空即清除。回执落定后重拉群列表对齐。
void MainWindow::group_profile_dialog(quint64 group_id) {
  const auto it = groups_.find(group_id);
  if (it == groups_.end()) return;
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("群资料（群 %1）").arg(group_id));
  dlg.resize(420, 300);
  auto* form = new QFormLayout(&dlg);
  auto* topic = new QLineEdit(it->topic, &dlg);
  topic->setPlaceholderText(QStringLiteral("一句话主题（可空）"));
  form->addRow(QStringLiteral("主题"), topic);
  auto* purpose = new QTextEdit(it->purpose, &dlg);
  purpose->setPlaceholderText(QStringLiteral("群目的说明（可空）"));
  form->addRow(QStringLiteral("目的"), purpose);
  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  form->addRow(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (dlg.exec() != QDialog::Accepted) return;
  collab_engine_.set_group_profile(group_id, topic->text().trimmed(),
                                   purpose->toPlainText().trimmed());
  show_status(QStringLiteral("群资料设置请求已发送（群主/管理员可设）"));
}

// 群组-2 群记录导出（定案②全员：本群成员即可导，服务端判权；定案③留痕：
// 服务端一次导出落 audit_reads 一行；定案④ Markdown .md）：区间＋AI 档两
// 选项→逐页拉取（keyset 游标续传、可中断、触顶 truncated 不静默截尾）→
// 拼接后存 .md（含「AI 未启用」降级注明——模型未接线）。
void MainWindow::group_record_export_dialog(quint64 group_id) {
  const auto it = groups_.find(group_id);
  if (it == groups_.end()) return;
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("导出群记录（群 %1）").arg(group_id));
  dlg.resize(480, 240);
  auto* form = new QFormLayout(&dlg);
  auto* from_edit =
      new QDateTimeEdit(QDateTime::currentDateTime().addDays(-7), &dlg);
  from_edit->setCalendarPopup(true);
  from_edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
  form->addRow(QStringLiteral("起始时间（含）"), from_edit);
  auto* to_edit = new QDateTimeEdit(QDateTime::currentDateTime(), &dlg);
  to_edit->setCalendarPopup(true);
  to_edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
  form->addRow(QStringLiteral("截止时间（含）"), to_edit);
  auto* cb_ai = new QCheckBox(
      QStringLiteral("生成 AI 总结（未接线则自动降级为普通文档）"), &dlg);
  form->addRow(QStringLiteral("总结档"), cb_ai);
  auto* progress = new QLabel(QStringLiteral("尚未导出"), &dlg);
  form->addRow(QStringLiteral("进度"), progress);
  auto* buttons = new QDialogButtonBox(&dlg);
  auto* btn_start = buttons->addButton(QStringLiteral("开始导出"),
                                       QDialogButtonBox::ActionRole);
  auto* btn_save = buttons->addButton(QStringLiteral("保存为 Markdown…"),
                                      QDialogButtonBox::ActionRole);
  auto* btn_close =
      buttons->addButton(QStringLiteral("关闭"), QDialogButtonBox::RejectRole);
  btn_save->setEnabled(false);
  form->addRow(buttons);

  // 导出会话状态（逐页累积；随对话框销毁自动作废）
  struct ExportState {
    quint64 gid = 0;
    qint64 from_ms = 0;
    qint64 to_ms = 0;
    bool with_ai = false;
    qint64 fetched = 0;
    qint64 after_id = 0;
    int pages = 0;
    QString md;
  };
  QSharedPointer<ExportState> st(new ExportState());

  auto finish = [&](const QString& msg, bool can_save) {
    progress->setText(msg);
    btn_start->setEnabled(true);
    btn_save->setEnabled(can_save);
  };
  auto begin_export = [&]() {
    progress->setText(QStringLiteral("导出中…（第 1 页）"));
    files_client()->group_record_export(st->gid, st->from_ms, st->to_ms,
                                       st->with_ai, 0, 0);
  };
  auto start = [&]() {
    btn_start->setEnabled(false);
    btn_save->setEnabled(false);
    st->gid = group_id;
    st->from_ms = from_edit->dateTime().toMSecsSinceEpoch();
    st->to_ms = to_edit->dateTime().toMSecsSinceEpoch();
    st->with_ai = cb_ai->isChecked();
    st->fetched = 0;
    st->after_id = 0;
    st->pages = 0;
    st->md.clear();
    if (!files_ensure_login()) {
      finish(QStringLiteral("文件面未连接（files_port 未配置或协作未登录）"),
             false);
      return;
    }
    if (!files_client()->is_logged_in()) {
      progress->setText(QStringLiteral("文件面连接中…"));
      // 异步登录：logged_in 到后开拉（SingleShot＝一次导出只触发一次）
      QObject::connect(files_client(), &FilesClient::logged_in, &dlg,
                       [&]() { begin_export(); }, Qt::SingleShotConnection);
      QObject::connect(files_client(), &FilesClient::login_failed, &dlg,
                       [this, &dlg, finish](const QString& err) {
                         finish(QStringLiteral("文件面登录失败：%1").arg(err),
                                false);
                       });
      return;
    }
    begin_export();
  };
  auto save = [&]() {
    const QString suggested = QStringLiteral("group-%1-record.md").arg(st->gid);
    const QString path = QFileDialog::getSaveFileName(
        &dlg, QStringLiteral("保存群记录"), suggested,
        QStringLiteral("Markdown（.md）;;所有文件"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
      show_status(QStringLiteral("保存失败：%1").arg(path));
      return;
    }
    f.write(st->md.toUtf8());
    f.close();
    show_status(QStringLiteral("群记录已保存（%1）").arg(path));
  };

  QObject::connect(btn_start, &QPushButton::clicked, &dlg,
                   [&]() { start(); });
  QObject::connect(btn_save, &QPushButton::clicked, &dlg, [&]() { save(); });
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg,
                   &QDialog::reject);
  // 单页回包：拼 md＋续页（游标续传；触顶 truncated 即收尾，不静默截尾）
  QObject::connect(files_client(), &FilesClient::group_record_export_page, &dlg,
                   [this, &dlg, st, progress, &finish, &save](
                       const QJsonObject& page) {
                     if (page.value(QStringLiteral("gid")).toVariant().toULongLong() !=
                         st->gid) {
                       return; // 其他群的回执不接
                     }
                     const QString md =
                         page.value(QStringLiteral("md")).toString();
                     st->md += md;
                     st->pages += 1;
                     st->fetched += page.value(QStringLiteral("count")).toInt();
                     const bool has_more =
                         page.value(QStringLiteral("has_more")).toBool();
                     const bool truncated =
                         page.value(QStringLiteral("truncated")).toBool();
                     const qint64 next_id =
                         page.value(QStringLiteral("next_after_id"))
                             .toVariant()
                             .toLongLong();
                     if (has_more && !truncated && next_id > 0) {
                       progress->setText(
                           QStringLiteral("导出中…（第 %1 页，已 %2 条）")
                               .arg(st->pages + 1)
                               .arg(st->fetched));
                       files_client()->group_record_export(
                           st->gid, st->from_ms, st->to_ms, st->with_ai,
                           next_id, st->fetched);
                       return;
                     }
                     QString msg = QStringLiteral("导出完成（%1 页 %2 条）")
                                       .arg(st->pages)
                                       .arg(st->fetched);
                     if (truncated) {
                       msg += QStringLiteral("（已达单次上限，尾部 truncated 标"
                                            "注；续传用 after_id=%1）")
                                    .arg(next_id);
                     }
                     finish(msg, true);
                     if (st->with_ai &&
                         !page.value(QStringLiteral("ai_enabled")).toBool()) {
                       // 降级口径：AI 未接线＝普通文档＋「AI 未启用」注明
                       show_status(QStringLiteral(
                           "AI 未启用：已导出为普通文档（模型未接线/网关不可达"
                           "自动降级）"));
                     }
                     save();
                   });
  QObject::connect(files_client(), &FilesClient::group_record_export_failed,
                   &dlg, [&dlg, st, finish](const QString& err) {
                     finish(QStringLiteral("导出失败：%1").arg(err), false);
                   });
  dlg.exec();
}

// 公告编辑历史（R24-1）：请求后等专用回执（超时兜底），倒序列出
// 谁/何时/改成了什么（content 空串＝该次为清除）。
void MainWindow::group_announce_history_dialog(quint64 group_id) {
  if (!groups_.contains(group_id)) return;
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("公告编辑历史（群 %1）").arg(group_id));
  dlg.resize(520, 400);
  auto* layout = new QVBoxLayout(&dlg);
  auto* list = new QListWidget(&dlg);
  list->setWordWrap(true);
  layout->addWidget(list);
  auto* close_btn =
      new QPushButton(QStringLiteral("关闭"), &dlg);
  layout->addWidget(close_btn);
  QObject::connect(close_btn, &QPushButton::clicked, &dlg, &QDialog::accept);

  QEventLoop loop;
  auto conn = connect(
      &collab_engine_, &CollabEngine::announcement_history_received, &dlg,
      [&loop, &list, group_id](quint64 gid, const QString& history_json) {
        if (gid != group_id) return; // 其他群的回执继续等
        const auto arr = nlohmann::json::parse(
            history_json.toStdString(), nullptr, false);
        if (arr.is_discarded() || !arr.is_array()) {
          auto* item = new QListWidgetItem(QStringLiteral("历史解析失败"), list);
          item->setForeground(Qt::gray);
        } else if (arr.empty()) {
          new QListWidgetItem(QStringLiteral("暂无编辑记录"), list);
        } else {
          for (const auto& h : arr) {
            const QDateTime ts =
                QDateTime::fromMSecsSinceEpoch(h.value("ts_ms", 0));
            const QString content =
                QString::fromStdString(h.value("content", std::string{}));
            new QListWidgetItem(
                QStringLiteral("%1　%2　设为：%3")
                    .arg(ts.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                         QString::fromStdString(h.value("editor", std::string{})),
                         content.isEmpty() ? QStringLiteral("（清除公告）")
                                           : content),
                list);
          }
        }
        loop.quit();
      });
  collab_engine_.announce_history(group_id);
  // 超时兜底：服务端 3 秒无回执提示后关（连接异常等极端情形不吊死模态）
  QTimer::singleShot(3000, &loop, &QEventLoop::quit);
  loop.exec();
  QObject::disconnect(conn);
  dlg.exec();
}

// 免服务端临时群（直连态）：从已发现设备多选，发送＝逐设备点对点扇出。
// 不经服务端——不进归档（界面分组与提示条明示）。
void MainWindow::dgroup_dialog() {
  const auto peers = direct_engine_.peers();
  if (peers.isEmpty()) {
    show_status(QStringLiteral("未发现局域网设备，无法建临时群"));
    return;
  }
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("新建临时群（点对点扇出，不进归档）"));
  dlg.resize(400, 440);
  auto* layout = new QVBoxLayout(&dlg);
  auto* list = new QListWidget(&dlg);
  for (const Peer& p : peers) {
    const QString id = QString::fromStdString(p.device_id);
    const QString name =
        p.name.empty() ? id.left(8) : QString::fromStdString(p.name);
    auto* item = new QListWidgetItem(
        QStringLiteral("%1\n%2 · TCP %3")
            .arg(name, p.address.toString(), QString::number(p.tcp_port)),
        list);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
    item->setData(Qt::UserRole, id);
  }
  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  layout->addWidget(list);
  layout->addWidget(buttons);
  if (dlg.exec() != QDialog::Accepted) return;
  QStringList picked;
  for (int i = 0; i < list->count(); ++i) {
    if (list->item(i)->checkState() == Qt::Checked) {
      picked << list->item(i)->data(Qt::UserRole).toString();
    }
  }
  if (picked.isEmpty()) {
    show_status(QStringLiteral("建临时群取消：未选择设备"));
    return;
  }
  const QString id = QStringLiteral("dgroup:%1").arg(next_dgroup_++);
  dgroup_members_.insert(id, picked);
  refresh_devices();
  open_chat(QStringLiteral("dgroup"), id);
  show_status(QStringLiteral("临时群已建立（%1 台设备）· 不进归档").arg(picked.size()));
}

QString MainWindow::dgroup_title(const QString& dgroup_id) const {
  return QStringLiteral("临时群 %1")
      .arg(dgroup_id.mid(QStringLiteral("dgroup:").size()));
}

// —— T3.1 组织架构：登录后向服务端查询，管理端维护即生效到客户端 ——

void MainWindow::request_org() {
  if (collab_engine_.is_logged_in()) collab_engine_.query_org();
}

QString MainWindow::org_json() const { return last_org_json_; }

QString MainWindow::banner_text() const { return banner_->text(); }

QString MainWindow::status_text() const { return statusBar()->currentMessage(); }

QString MainWindow::chat_html() const { return chat_view_->toHtml(); }

QString MainWindow::input_hint() const { return input_box_->placeholderText(); }

QString MainWindow::chat_meta() const { return chat_meta_->text(); }

// —— T4.3 消息状态与多端（验收面）——
QString MainWindow::delivery_text() const { return delivery_text_; }
QString MainWindow::kick_text() const { return kick_text_; }
QStringList MainWindow::online_accounts() const {
  QStringList out = online_accounts_.values(); // values() 直取，避免区间构造歧义
  out.sort();
  return out;
}
void MainWindow::set_delivery_state(const QString& text) {
  delivery_text_ = text;
  show_status(text); // 状态栏同步（最近一条发出消息的状态常驻可查）
}

// —— 需求批⑦ 消息回执：开关与消息旁状态 ——

bool MainWindow::read_receipts_enabled(const QString& peer) const {
  // 全局默认开；会话级显式设置（true/false）覆盖全局（QSettings 持久化）
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  if (!peer.isEmpty() &&
      settings.contains(QStringLiteral("receipts/peer/%1").arg(peer))) {
    return settings.value(QStringLiteral("receipts/peer/%1").arg(peer))
        .toBool();
  }
  return settings
      .value(QStringLiteral("receipts/send_read"), true)
      .toBool();
}

void MainWindow::set_read_receipts_enabled(const QString& peer, bool enabled) {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  if (peer.isEmpty()) {
    settings.setValue(QStringLiteral("receipts/send_read"), enabled);
    show_status(enabled ? QStringLiteral("已读回执：全局开启（新收消息将上报已读）")
                        : QStringLiteral("已读回执：全局关闭（不再上报已读）"));
    return;
  }
  settings.setValue(QStringLiteral("receipts/peer/%1").arg(peer), enabled);
  show_status(enabled ? QStringLiteral("已读回执（本会话 %1）：开启").arg(peer)
                      : QStringLiteral("已读回执（本会话 %1）：关闭").arg(peer));
}

void MainWindow::apply_message_receipt(const QString& msg_id,
                                       const QString& state) {
  if (msg_id.isEmpty() || state.isEmpty()) return;
  bool changed = false;
  for (ChatRow& row : chat_rows_) {
    if (!row.system && !row.msg_id.isEmpty() && row.msg_id == msg_id &&
        row.receipt != state) {
      // 只升不降（与本地库同口径）：已读不被乱序送达通知倒退
      if (row.receipt == QStringLiteral("read") &&
          state == QStringLiteral("delivered"))
        continue;
      row.receipt = state;
      changed = true;
    }
  }
  if (changed) rerender_chat();
}

void MainWindow::apply_direct_receipt(quint64 seq, const QString& state) {
  // 直连域回执落行（需求批⑦·已读灰勾单）：直连行无 msg_id，按我发出行
  // 的发送序号命中；只升不降与 apply_message_receipt 同口径
  if (seq == 0 || state.isEmpty()) return;
  bool changed = false;
  for (ChatRow& row : chat_rows_) {
    if (!row.system && row.outgoing && row.seq == seq &&
        row.receipt != state) {
      if (row.receipt == QStringLiteral("read") &&
          state == QStringLiteral("delivered"))
        continue;
      row.receipt = state;
      changed = true;
    }
  }
  if (changed) rerender_chat();
}

// —— 「盯」提醒（todo「盯」单·实现单修订 2026-10-14）——

bool MainWindow::watch_current_row(const QString& key) {
  // 右键菜单与测试同一路径。已读拒绝创建（盯与已读同生命周期）；重复
  // 盯同键＝刷新（UPSERT）而非报错
  if (key.isEmpty()) return false;
  const ChatRow* row = nullptr;
  for (const ChatRow& r : chat_rows_) {
    if (!r.system && r.watch_key == key) {
      row = &r;
      break;
    }
  }
  if (row == nullptr) {
    show_status(QStringLiteral("盯失败：消息不在当前会话"));
    return false;
  }
  if (row->receipt == QStringLiteral("read")) {
    show_status(QStringLiteral("该消息已被读，无需盯"));
    return false;
  }
  LocalStore* store = direct_engine_.store();
  if (store == nullptr) {
    show_status(QStringLiteral("盯失败：本地库未就绪"));
    return false;
  }
  WatchedRow w;
  w.key = key.toStdString();
  w.peer = current_peer_.toStdString();
  w.source = current_kind_.toStdString();
  w.text = row->text.left(24).toStdString(); // 提醒预览（超长截断）
  w.created_ms = QDateTime::currentMSecsSinceEpoch();
  if (!store->add_watch(w)) {
    show_status(QStringLiteral("盯失败：本地库写入失败"));
    return false;
  }
  watched_.insert(key, w);
  rerender_chat(); // 气泡 meta 行长出灰 ◉
  show_status(QStringLiteral(
                  "已盯住本消息：对方已读或未读超 %1 分钟时提醒")
                  .arg(watch_remind_ms() / 60000));
  return true;
}

void MainWindow::unwatch_key(const QString& key) {
  // 手动解除（菜单「取消盯」同路径）；未盯的键静默幂等
  if (watched_.remove(key) == 0) return;
  if (LocalStore* store = direct_engine_.store()) store->remove_watch(key.toStdString());
  rerender_chat(); // 摘 ◉
  show_status(QStringLiteral("已取消盯住"));
}

void MainWindow::watch_read_fired(const QString& key, const QString& reader) {
  // 已读＝静默自动解除（实现单修订 2026-10-14：对方一打开就提醒是「已读
  // 通知」不是「盯」——提醒只走未读超时一路）。生产路径＝message_read／
  // receipts_received 信号驱动；盯住集合无此键＝普通已读，静默穿过
  Q_UNUSED(reader);
  const auto it = watched_.constFind(key);
  if (it == watched_.constEnd()) return;
  const WatchedRow w = it.value();
  watched_.erase(it);
  if (LocalStore* store = direct_engine_.store()) {
    store->remove_watch(w.key);
  }
  ++watch_released_;
  rerender_chat(); // 摘 ◉（无任何用户可见输出）
}

void MainWindow::sweep_watches() {
  LocalStore* store = direct_engine_.store();
  if (store == nullptr) return;
  // 以库为源巡检（内存 watched_ 只是界面侧快速集）：双窗口同库、外部
  // 改写 created_ms（测试回拨）、重启后遗留行都按库面现值裁决
  const QList<WatchedRow> rows = store->watched_all();
  if (rows.isEmpty()) return;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  QStringList read_fire;      // 离线期间已变已读的（补解除）
  QStringList remind_unread;  // 未读超阈值提醒（只一次；阈值可配，默认 10 分钟）
  QStringList expired;        // 超时静默过期（默认 24h，可配）
  for (const WatchedRow& w : rows) {
    // 已读对账：READ 到达前盯行已存在（离线期/补拉场景）——receipt 落库
    // 而 watch 未解除即在此补触发（与信号路径同一出口）
    std::string receipt;
    if (w.source == "collab") {
      receipt = store->receipt_of_msg_id(w.key.substr(2));
    } else {
      // 直连键 d:<收件人>:<序号>（对端不含冒号，取最后一段）
      const QString qkey = QString::fromStdString(w.key);
      const qsizetype cut = qkey.lastIndexOf(QLatin1Char(':'));
      const QString peer = cut > 1 ? qkey.mid(2, cut - 2) : QString();
      const quint64 seq = cut > 1 ? qkey.mid(cut + 1).toULongLong() : 0;
      receipt = (!peer.isEmpty() && seq != 0)
                    ? store->receipt_of_seq(peer.toStdString(), seq)
                    : std::string();
    }
    const QString key = QString::fromStdString(w.key);
    if (receipt == "read") {
      read_fire.append(key);
    } else if (!w.reminded && now - w.created_ms >= watch_remind_ms()) {
      remind_unread.append(key);
    } else if (now - w.created_ms >= watch_expire_ms()) {
      expired.append(key);
    }
  }
  for (const QString& key : read_fire) {
    watch_read_fired(key); // 离线补解除不标读者名（静默）
  }
  for (const QString& key : remind_unread) {
    WatchedRow w;
    for (const WatchedRow& r : rows) {
      if (r.key == key.toStdString()) {
        w = r;
        break;
      }
    }
    store->mark_watch_reminded(w.key); // 只一次打点（先打点防重入轰炸）
    ++watch_timeout_;
    last_watch_notify_ =
        QStringLiteral("盯住的消息仍未读：%1")
            .arg(QString::fromStdString(w.text));
    tray_notify(QStringLiteral("盯住的消息未读提醒"), last_watch_notify_);
    show_status(last_watch_notify_);
    watch_reminded_peers_.insert(QString::fromStdString(w.peer));
    refresh_devices(); // 列表高亮（◉ 前缀，复用未读角标位；打开该会话即清）
  }
  for (const QString& key : expired) {
    watched_.remove(key);
    store->remove_watch(key.toStdString());
    ++watch_expired_; // 超时解除不通知（防僵尸盯）
  }
  if (!expired.isEmpty()) rerender_chat();
}

void MainWindow::load_watches() {
  watched_.clear();
  if (LocalStore* store = direct_engine_.store()) {
    for (const WatchedRow& w : store->watched_all()) {
      watched_.insert(QString::fromStdString(w.key), w);
    }
  }
}

namespace {

// 盯行按创建时刻排序（快照与列表展示的稳定序）
QList<WatchedRow> watched_ordered(const QHash<QString, WatchedRow>& watched) {
  QList<WatchedRow> rows = watched.values();
  std::sort(rows.begin(), rows.end(),
            [](const WatchedRow& a, const WatchedRow& b) {
              return a.created_ms < b.created_ms;
            });
  return rows;
}

} // namespace

QString MainWindow::watch_row_label(const WatchedRow& w) const {
  // 「我在盯哪些」一行：域 · 对端名（解析不出用短 id）· 预览 · 已盯时长
  const QString key = QString::fromStdString(w.key);
  const QString domain = w.source == "collab" ? QStringLiteral("协作")
                                              : QStringLiteral("直连");
  QString peer_name = QString::fromStdString(w.peer);
  if (w.source != "collab") {
    const Peer p = direct_engine_.peer(w.peer);
    if (!p.name.empty()) peer_name = QString::fromStdString(p.name);
  } else {
    peer_name = key.mid(2); // 协作键 c:<会话 id> 即对端账号
  }
  const qint64 held_min = (QDateTime::currentMSecsSinceEpoch() - w.created_ms +
                           60000 - 1) / 60000;
  return QStringLiteral("%1 · %2 · %3 · 已盯 %4 分钟")
      .arg(domain, peer_name, QString::fromStdString(w.text),
           QString::number(held_min));
}

QStringList MainWindow::watch_list_snapshot() const {
  QStringList out;
  for (const WatchedRow& w : watched_ordered(watched_)) {
    out.append(watch_row_label(w));
  }
  return out;
}

bool MainWindow::cancel_watch_by_index(int index) {
  const QList<WatchedRow> rows = watched_ordered(watched_);
  if (index < 0 || index >= rows.size()) return false;
  unwatch_key(QString::fromStdString(rows[index].key));
  return true;
}

void MainWindow::open_watch_list() {
  // 盯列表（实现单修订 2026-10-14：要有入口看到「我在盯哪些」）：非模态
  // 对话框，逐条可取消；空态给一行说明
  if (watch_list_dialog_ == nullptr) {
    watch_list_dialog_ = new QDialog(this);
    watch_list_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    watch_list_dialog_->setWindowTitle(QStringLiteral("盯列表"));
    auto* lay = new QVBoxLayout(watch_list_dialog_);
    watch_list_widget_ = new QListWidget(watch_list_dialog_);
    watch_list_widget_->setMinimumSize(420, 220);
    lay->addWidget(watch_list_widget_);
    auto* row = new QHBoxLayout();
    auto* cancel_btn = new QPushButton(QStringLiteral("取消盯"), watch_list_dialog_);
    auto* close_btn = new QPushButton(QStringLiteral("关闭"), watch_list_dialog_);
    row->addStretch();
    row->addWidget(cancel_btn);
    row->addWidget(close_btn);
    lay->addLayout(row);
    connect(cancel_btn, &QPushButton::clicked, this, [this] {
      const int cur = watch_list_widget_->currentRow();
      if (cancel_watch_by_index(cur)) refresh_watch_list();
    });
    connect(close_btn, &QPushButton::clicked, watch_list_dialog_, &QDialog::close);
  }
  refresh_watch_list();
  watch_list_dialog_->show();
  watch_list_dialog_->raise();
  watch_list_dialog_->activateWindow();
}

void MainWindow::refresh_watch_list() {
  if (watch_list_widget_ == nullptr) return;
  watch_list_widget_->clear();
  const QStringList entries = watch_list_snapshot();
  if (entries.isEmpty()) {
    watch_list_widget_->addItem(QStringLiteral(
        "暂无盯住的消息——在气泡上右键「盯住本消息」可跟进一条"));
    return;
  }
  watch_list_widget_->addItems(entries);
}

// —— T4.7 系统集成 ——

QString MainWindow::last_notify() const { return last_notify_; }

bool MainWindow::tray_available() const {
  return tray_ != nullptr && QSystemTrayIcon::isSystemTrayAvailable();
}

// A23 图标面：关于页——logo（qrc 多尺寸，与窗口/任务栏/安装器同源）＋版本
void MainWindow::show_about() {
  QDialog dlg(this);
  dlg.setObjectName(QStringLiteral("about_dialog"));
  dlg.setWindowTitle(QStringLiteral("关于 Memex"));
  auto* lay = new QVBoxLayout(&dlg);
  auto* logo = new QLabel(&dlg);
  logo->setObjectName(QStringLiteral("about_logo"));
  logo->setPixmap(brand_icon().pixmap(128, 128));
  logo->setAlignment(Qt::AlignCenter);
  lay->addWidget(logo);
  auto* name =
      new QLabel(QStringLiteral("<b>Memex</b> 内网办公即时通讯"), &dlg);
  name->setAlignment(Qt::AlignCenter);
  lay->addWidget(name);
  auto* ver = new QLabel(
      QStringLiteral("版本 %1").arg(QStringLiteral(MEMEX_VERSION)), &dlg);
  ver->setObjectName(QStringLiteral("about_version"));
  ver->setAlignment(Qt::AlignCenter);
  lay->addWidget(ver);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::close);
  lay->addWidget(buttons);
  dlg.setMinimumWidth(320);
  dlg.exec();
}

// BUG-004 托盘激活：隐藏→弹出、最小化→还原（保留最大化态），随后置顶聚焦。
// 三平台同一路径（Qt 抽象层），无平台分支。
void MainWindow::activate_from_tray() {
  setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
  show();
  raise();
  activateWindow();
}

// BUG-004 主流托盘交互：单击/双击都激活主界面（Qt 惯例，微信/企业微信同类）；
// 右键（Context）留给上下文菜单，中键/未知不响应。
// 「最小化到托盘」气泡（closeEvent 里 tray_notify）在托盘图标上方挡 5 秒，
// 第一击常落在气泡上而非图标——气泡点击（messageClicked）同样激活主界面，
// 保证隐藏后第一击必弹（实测：不接线时第一击只把气泡点没，窗口不出）。
void MainWindow::wire_tray_activation(QSystemTrayIcon* tray) {
  connect(tray, &QSystemTrayIcon::activated, this,
          [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger ||
                reason == QSystemTrayIcon::DoubleClick) {
              activate_from_tray();
            }
          });
  connect(tray, &QSystemTrayIcon::messageClicked, this,
          [this] { activate_from_tray(); });
}

void MainWindow::setup_tray() {
  if (!QSystemTrayIcon::isSystemTrayAvailable()) return; // 无托盘环境跳过
  tray_ = new QSystemTrayIcon(brand_icon(), this);
  tray_->setToolTip(QStringLiteral("Memex（直连态：消息不进归档）"));
  auto* menu = new QMenu(this);
  auto* act_show = menu->addAction(QStringLiteral("显示主窗口"));
  connect(act_show, &QAction::triggered, this, [this] { activate_from_tray(); });
  auto* act_tray_auto = menu->addAction(QStringLiteral("开机启动"));
  act_tray_auto->setCheckable(true);
  act_tray_auto->setChecked(autostart_enabled());
  connect(act_tray_auto, &QAction::triggered, this, [this, act_tray_auto](bool on) {
    set_autostart(on);
    act_tray_auto->setChecked(autostart_enabled());
    if (act_autostart_) act_autostart_->setChecked(autostart_enabled());
  });
  auto* act_notify_prefs = menu->addAction(QStringLiteral("通知偏好"));
  connect(act_notify_prefs, &QAction::triggered, this, [] {
    NotificationCenter::instance().show_settings();
  });
  auto* act_quit = menu->addAction(QStringLiteral("退出"));
  connect(act_quit, &QAction::triggered, this, &MainWindow::quit_app);
  tray_->setContextMenu(menu);
  tray_->show();
  wire_tray_activation(tray_);
}

// 托盘菜单「退出」真身（2026-10-11 用户实录：窗口最小化到托盘的隐藏态
// 点退出，进程不退——close() 不可见窗口不触发 lastWindowClosed（Qt 的
// 「最后一个可见窗口关闭」语义），仅摘窗不退进程＝无窗运行态，托盘图标
// 还在（QSystemTrayIcon 对象未析构），用户得把窗口拉起来再退一次才死。
// 退出即显式 quit：不依赖窗口可见性。先摘 tray_ 让 closeEvent 不再按
// 「最小化到托盘」分支拦截，close() 走完关闭语义（隐藏＋收尾）再退。
void MainWindow::quit_app() {
  tray_ = nullptr;
  close();
  QApplication::quit();
}

// 新消息闪烁提醒（用户令 2026-10-05）：QWindow::alert 走平台原生注意 API
// ——Windows FlashWindowEx／macOS requestUserAttention／X11 urgency 任务栏
// 闪烁；Wayland 无该协议（平台降级边界，BUGS/走查注明）。开关＝通知偏好
// 的 flash_alert（默认开），关了完全不闪；合并窗内只触发一次不叠加。
void MainWindow::alert_attention() {
  if (!NotifyPrefs::load().flash_alert) return;
  if (isActiveWindow()) return;   // 激活中不闪（触发点已判，双保险）
  if (alert_active_) return;      // 合并窗内不叠加
  alert_active_ = true;
  ++alert_count_;
  if (QWindow* handle = windowHandle()) handle->alert(kFlashAlertMs);
  QTimer::singleShot(kFlashAlertMs, this, [this] { alert_active_ = false; });
}

int MainWindow::alert_count() const { return alert_count_; }

void MainWindow::tray_notify(const QString& title, const QString& text) {
  last_notify_ = title + QStringLiteral("：") + text; // 无托盘也记录（断言面）
  if (tray_ && QSystemTrayIcon::supportsMessages()) {
    tray_->showMessage(title, text, QSystemTrayIcon::Information, 5000);
  }
}

void MainWindow::event_notify(bool enabled, SoundEvent ev,
                              const QString& title, const QString& text) {
  // 锁屏期间（用户令 2026-10-08 ④）：内容不出锁屏面——弹窗与提示音全静默，
  // 只累计未读条数（上线不是消息不计）
  if (lock_) {
    if (ev != SoundEvent::Online) lock_->bump_unread();
    return;
  }
  const NotifyPrefs p = NotifyPrefs::load();
  if (enabled) tray_notify(title, text); // 弹窗受事件开关裁决
  // 提示音三档独立裁决（弹窗关了声音仍可按档播）；beep 兜底＝无
  // Multimedia 依赖（真音频文件走 Qt Multimedia 另批，文档注明）
  if (sound_should_play(p.sound_mode, p.sound_msg, p.sound_file,
                        p.sound_online, ev)) {
    QApplication::beep();
  }
}

// —— 离开锁屏（用户令 2026-10-08 ④）——
// 全局输入事件喂计时器：鼠标移动/点击、键盘、滚轮任一即重计。
// 锁屏遮罩在屏时不再重置（锁已锁上，输密码动作不算「回来了」）。
bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
  // @ 成员菜单（Qt::Popup）抢键盘：键入转发回输入框（连续输入继续按词头
  // 过滤），菜单重建延到事件循环空转帧——分发中清 QMenu actions 会悬垂
  // （textChanged 在转发栈内同步触发，见 rebuild_at_menu 调用方）
  if (watched == at_menu_ && event->type() == QEvent::KeyPress) {
    auto* ke = static_cast<QKeyEvent*>(event);
    if (ke->key() == Qt::Key_Escape) return false; // 菜单自己收（不吞 Esc）
    if (input_box_) {
      in_at_key_forward_ = true;
      input_box_->event(event); // 键入回输入框（textChanged 被标记拦下）
      in_at_key_forward_ = false;
      QTimer::singleShot(0, this, [this] {
        if (at_menu_) rebuild_at_menu(input_box_->text());
      });
    }
    return true;
  }
  switch (event->type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::KeyPress:
    case QEvent::Wheel:
      if (idle_timer_ && !lock_) idle_timer_->start();
      break;
    default:
      break;
  }
  return QMainWindow::eventFilter(watched, event);
}

// 重读 away_lock 设置（设置对话框关闭/测试接线即调＝即时生效）：
// 计时器间隔换算毫秒重启；未启用或没密码则停表（没密码锁不住人，不装锁）。
void MainWindow::apply_away_lock_settings() {
  if (!idle_timer_) return;
  if (away_lock::enabled() && away_lock::has_password()) {
    idle_timer_->start(away_lock::timeout_seconds() * 1000);
  } else {
    idle_timer_->stop();
  }
}

// 重读 net_blacklist：开关关＝不装过滤器（全放行）；开＝段表转谓词注入
// 直连引擎（发现包 sender 与 TCP 入站 peerAddress 两处同一谓词）。
// CIDR 匹配只在 net_guard 内实现，这里只拼装不重复实现。
void MainWindow::apply_net_settings() {
  direct_engine_.set_ipmsg_enabled(ipmsg_compat::enabled()); // 飞秋兼容即时生效
  if (!net_blacklist::enabled()) {
    direct_engine_.set_address_filter({});
    return;
  }
  const QStringList cidrs = net_blacklist::entries();
  direct_engine_.set_address_filter(
      [cidrs](const QHostAddress& addr) {
        return !net_guard::blocked(cidrs, addr); // 谓词=放行；黑名单命中=false 拒
      });
}

void MainWindow::maybe_lock_screen() {
  if (lock_) return;
  if (!away_lock::enabled() || !away_lock::has_password()) return;
  lock_ = new LockScreenDialog(
      [](const QString& pwd) { return away_lock::verify_password(pwd); },
      this);
  connect(lock_, &QDialog::finished, this, [this](int) {
    lock_->deleteLater();
    lock_ = nullptr;
    apply_away_lock_settings(); // 解锁后重新计时（下次离开再锁）
  });
  lock_->showFullScreen(); // 无边框置顶全屏；Esc/close 拒，唯一出路＝密码
}

bool MainWindow::lock_screen_visible() const {
  return lock_ && lock_->isVisible();
}

int MainWindow::lock_unread_count() const {
  return lock_ ? lock_->unread_count() : 0;
}

QString MainWindow::lock_screen_text() const {
  return lock_ ? lock_->unread_text() : QString();
}

void MainWindow::lock_try_unlock(const QString& pwd) {
  if (lock_) lock_->try_unlock(pwd);
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (tray_) {
    // 托盘可用：关闭即最小化进托盘（常驻后台不断直连发现；真退出走托盘菜单）
    hide();
    tray_notify(QStringLiteral("已最小化到托盘"),
                QStringLiteral("Memex 仍在后台运行（右键托盘图标可退出）"));
    event->ignore();
    return;
  }
  save_window_geometry(); // 真退出：存窗口几何，下次启动恢复（首次启动用默认）
  QMainWindow::closeEvent(event);
}

QString MainWindow::autostart_dir() {
  // 测试覆盖：MEMEX_TEST_AUTOSTART_DIR 指向临时目录，避免污染真实家目录
  const QByteArray override_dir = qgetenv("MEMEX_TEST_AUTOSTART_DIR");
  if (!override_dir.isEmpty()) return QString::fromUtf8(override_dir);
  return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
         QStringLiteral("/autostart");
}

bool MainWindow::autostart_enabled() const {
  return QFile::exists(autostart_dir() + QStringLiteral("/memex-client.desktop"));
}

void MainWindow::set_autostart(bool on) {
  const QString path = autostart_dir() + QStringLiteral("/memex-client.desktop");
  if (!on) {
    QFile::remove(path);
    if (act_autostart_) act_autostart_->setChecked(false);
    return;
  }
  QDir().mkpath(autostart_dir());
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
    if (act_autostart_) act_autostart_->setChecked(false); // 落盘失败回滚勾选
    return;
  }
  QTextStream out(&f);
  out << "[Desktop Entry]\n"
      << "Type=Application\n"
      << "Name=Memex\n"
      << "Icon=memex\n" // hicolor 图标（deb/rpm 装到 /usr/share/icons/hicolor）
      << "Comment=Memex 内网办公即时通讯\n"
      << "Exec=" << QCoreApplication::applicationFilePath() << "\n"
      << "Terminal=false\n"
      << "Categories=Network;InstantMessaging;\n"
      << "X-GNOME-Autostart-enabled=true\n";
  f.close();
  if (act_autostart_) act_autostart_->setChecked(true);
}

// 归档提示条（常驻不可关）：协作态显示归档口径；直连／降级态必须明示
// 「消息不进归档」——切换与降级共用这一条提示（A7、A11 界面口径）。
// 平台-7 降级显式化：DEGRADED 压过跨态（服务端不可达对本会话恒真，
// 是更具体的口径——跨态细节由会话标记承载）；输入框提示随态同步。
void MainWindow::update_chat_avatar() {
  if (!chat_avatar_) return;
  if (chat_avatar_src_.isEmpty()) {
    chat_avatar_->clear();
    chat_avatar_->setStyleSheet(QString());
    return;
  }
  // 原型 .avatar.sm：28px 圆角 8、底＝名源哈希色、字＝on_brand 700
  chat_avatar_->setText(avatar_initial(chat_avatar_src_));
  chat_avatar_->setStyleSheet(
      QStringLiteral("background:%1; color:%2; font-weight:700; "
                     "font-size:12px; border-radius:8px;")
          .arg(avatar_hue(chat_avatar_src_).name(), tk().on_brand.name()));
}

void MainWindow::update_banner() {
  // 协作单聊与服务端群聊都走服务端转发归档（T4.1 后者同口径）
  const bool collab_session =
      (current_kind_ == QStringLiteral("collab") ||
       current_kind_ == QStringLiteral("group")) &&
      collab_engine_.is_logged_in();
  const ThemeTokens& t = tk();
  if (collab_session) {
    // 群公告占横幅（原型 desktop-group.html .chat-banner.collab-ok：品牌底
    // 承公告）；无公告走通用归档口径。模型只存公告文本不存更新时刻——署
    // 群主不带「更新于」（台账近似项）
    const GroupEntry* g = current_kind_ == QStringLiteral("group")
                              ? current_group()
                              : nullptr;
    if (g && !g->announcement.isEmpty()) {
      QString owner = roster_display(g->owner);
      if (owner.isEmpty()) owner = g->owner;
      banner_->setStyleSheet(
          QStringLiteral("background:%1; color:%2; font-size:12px; "
                         "padding:6px 10px;")
              .arg(t.brand_wash.name(), t.brand_wash_text.name()));
      banner_->setText(QStringLiteral("　📢 群公告（群主：%1）：%2")
                           .arg(owner, g->announcement));
    } else {
      banner_->setStyleSheet(
          QStringLiteral("background:%1; color:%2; font-size:12px; "
                         "padding:6px 10px;")
              .arg(t.success_wash.name(), t.success_text.name()));
      banner_->setText(QStringLiteral(
          "　✔ 协作态会话：消息经服务端转发并全量归档；撤回仅改显示，"
          "服务端保留原文与撤回记录"));
    }
  } else {
    banner_->setStyleSheet(
        QStringLiteral("background:%1; color:%2; font-size:12px; "
                       "padding:6px 10px;")
            .arg(t.brand_wash.name(), t.brand_wash_text.name()));
    // 平台-7：DEGRADED（本应归档但服务端不可达）恒先判——全面标识
    if (collab_degraded_) {
      banner_->setText(QStringLiteral(
          "　⚠ 已降级直连态（未归档通信）：服务端不可达，消息不进归档"
          "（点对点传输，仅保存在双方本机；恢复后协作消息自动补传归档）"));
    } else {
      // T4.2 跨态会话（恰一边登录）：固定「未归档」标识，常驻不可关闭（A7）
      bool cross = false;
      if (current_kind_ == QStringLiteral("direct")) {
        cross = is_cross_state(current_peer_);
      } else if (current_kind_ == QStringLiteral("dgroup")) {
        for (const QString& dev : dgroup_members_.value(current_peer_)) {
          if (is_cross_state(dev)) {
            cross = true;
            break;
          }
        }
      }
      if (cross) {
        banner_->setText(QStringLiteral(
            "　⚠ 跨态会话 · 未归档：恰一边登录协作态，消息点对点传输不进归档"
            "（与未登录终端的会话标记常驻不可关闭）"));
      } else {
        banner_->setText(QStringLiteral(
            "　⚠ 直连态会话：消息点对点传输，不经过服务器——消息不进归档"
            "（服务端无任何记录，仅保存在双方本机）"));
      }
    }
  }
  // 平台-7：输入框提示随态——降级态明示「不进归档」，群聊走原型占位
  //（「输入消息，@ 成员可加提醒…」），其余常规
  if (collab_degraded_) {
    input_box_->setPlaceholderText(QStringLiteral(
        "未归档通信（已降级直连）：此会话消息不进服务端归档"));
  } else if (current_kind_ == QStringLiteral("group")) {
    input_box_->setPlaceholderText(
        QStringLiteral("输入消息，@ 成员可加提醒…"));
  } else {
    input_box_->setPlaceholderText(QStringLiteral("输入消息…"));
  }
  // 「@ 唤起成员选择」提示（原型 .input-toolbar .hint）：群会话专用
  if (at_hint_) at_hint_->setVisible(current_kind_ == QStringLiteral("group"));
  // 输入区固定说明条（原型 .input-note：跨态/直连＝警示底「不可关闭」，
  // 协作＝素底归档说明）；头部徽标（.noarch）只在未归档会话挂「未归档」
  if (input_note_) {
    const QString warn_note =
        QStringLiteral("background:%1; color:%2; font-size:11px; "
                       "padding:6px 18px;")
            .arg(t.warning_wash.name(), t.warning.name());
    const QString plain_note =
        QStringLiteral("background:transparent; color:%1; font-size:11px; "
                       "padding:6px 18px;")
            .arg(t.text_muted.name());
    if (collab_session) {
      input_note_->setStyleSheet(plain_note);
      input_note_->setText(
          current_kind_ == QStringLiteral("group")
              ? QStringLiteral("群聊 · 消息经服务端按成员扇出并全量归档")
              : QStringLiteral("协作态 · 本会话消息将进入服务端归档"));
    } else if (collab_degraded_) {
      input_note_->setStyleSheet(warn_note);
      input_note_->setText(QStringLiteral(
          "已降级直连：服务端不可达，消息暂不归档；恢复后协作消息自动补传归档。"
          "此说明固定显示，不可关闭。"));
    } else if (current_kind_ == QStringLiteral("direct") &&
               is_cross_state(current_peer_)) {
      input_note_->setStyleSheet(warn_note);
      input_note_->setText(QStringLiteral(
          "本会话当前为跨态直达：你发出的消息不进入归档。"
          "此说明固定显示，不可关闭。"));
    } else if (!current_peer_.isEmpty()) {
      input_note_->setStyleSheet(warn_note);
      input_note_->setText(QStringLiteral(
          "直连态：消息点对点传输、不经服务器、不进入归档（仅保存在双方本机）。"));
    } else {
      input_note_->setStyleSheet(plain_note);
      input_note_->setText(QString());
    }
  }
  if (local_badge_) {
    if (collab_session) {
      local_badge_->hide(); // 归档会话无「未归档」徽标（原型仅未归档会话挂）
    } else {
      local_badge_->setText(QStringLiteral("未归档"));
      local_badge_->setStyleSheet(
          QStringLiteral("background:%1; color:%2; border:1px dashed %3; "
                         "border-radius:999px; padding:1px 8px; font-size:11px; "
                         "font-weight:700;")
              .arg(t.warning_wash.name(), t.warning.name(), t.warning.name()));
      local_badge_->show();
    }
  }
  // 输入区未归档胶囊（原型 .input-foot .pill-outline）：文案随形态，
  // 归档会话不挂
  if (noarch_pill_) {
    if (collab_session &&
        current_kind_ == QStringLiteral("group")) {
      // 群会话归档口径胶囊（原型 desktop-group.html .input-foot
      // .pill-outline「群消息全量归档」）：描边素胶囊
      noarch_pill_->setText(QStringLiteral("群消息全量归档"));
      noarch_pill_->setStyleSheet(
          QStringLiteral("color:%1; border:1px solid %1; border-radius:999px; "
                         "padding:2px 8px; font-size:11px;")
              .arg(t.text_muted.name()));
    } else if (collab_session || current_peer_.isEmpty()) {
      noarch_pill_->clear();
      noarch_pill_->setStyleSheet(QString());
    } else {
      noarch_pill_->setText(
          collab_degraded_
              ? QStringLiteral("降级 · 未归档")
              : (current_kind_ == QStringLiteral("direct") &&
                         is_cross_state(current_peer_)
                     ? QStringLiteral("跨态 · 未归档")
                     : QStringLiteral("直连 · 无归档")));
      noarch_pill_->setStyleSheet(
          QStringLiteral("color:%1; border:1px solid %1; border-radius:999px; "
                         "padding:2px 8px; font-size:11px;")
              .arg(t.text_muted.name()));
    }
  }
}

// 登录后把本地库里的历史协作会话补进列表（换机／重启后会话入口不丢）。
// 群会话（"group:N"）不入单聊列表——群列表由 GROUP_DATA 单独维护（T4.1）。
void MainWindow::seed_collab_peers() {
  if (LocalStore* store = direct_engine_.store()) {
    const QStringList hist = store->peers(QStringLiteral("collab"));
    for (const QString& p : hist) {
      if (!p.startsWith(QStringLiteral("group:"))) collab_peers_.insert(p);
    }
  }
}

void MainWindow::show_collab_login_dialog() {
  QDialog dlg(this);
  const BrandKit& kit = BrandKit::instance();
  // 品牌随换：标题与顶部品牌区（公司名/logo/登录页文案；无牌=默认标）
  dlg.setWindowTitle(kit.company_name().isEmpty()
                         ? QStringLiteral("登录协作态")
                         : QStringLiteral("%1 · 登录").arg(
                               kit.company_name()));
  auto* lay = new QVBoxLayout(&dlg);
  if (kit.branded()) {
    auto* brand_area = new QWidget(&dlg);
    auto* brand_lay = new QHBoxLayout(brand_area);
    brand_lay->setContentsMargins(0, 0, 0, 0);
    auto* logo = new QLabel(brand_area);
    if (!kit.logo().isNull()) {
      logo->setPixmap(kit.logo().scaled(
          40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
      logo->setPixmap(kit.window_icon().pixmap(40, 40));
    }
    auto* name = new QLabel(kit.company_name(), brand_area);
    QFont bf = name->font();
    bf.setBold(true);
    bf.setPointSize(bf.pointSize() + 2);
    name->setFont(bf);
    brand_lay->addWidget(logo);
    brand_lay->addWidget(name);
    brand_lay->addStretch();
    lay->addWidget(brand_area);
    if (!kit.slogan().isEmpty()) {
      auto* slogan = new QLabel(kit.slogan(), &dlg);
      lay->addWidget(slogan);
    }
  }
  auto* form = new QFormLayout;
  lay->addLayout(form);
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  auto* host = new QLineEdit(
      settings.value(QStringLiteral("host"), default_collab_host())
          .toString(),
      &dlg);
  auto* port = new QLineEdit(
      settings.value(QStringLiteral("port"), default_collab_port()).toString(),
      &dlg);
  auto* files_port = new QLineEdit(
      settings.value(QStringLiteral("files_port"), default_files_port())
          .toString(),
      &dlg);
  auto* account = new QLineEdit(
      settings.value(QStringLiteral("account")).toString(), &dlg);
  auto* password = new QLineEdit(&dlg);
  password->setEchoMode(QLineEdit::Password);
  form->addRow(QStringLiteral("服务器地址"), host);
  form->addRow(QStringLiteral("端口"), port);
  form->addRow(QStringLiteral("文件口"), files_port);
  form->addRow(QStringLiteral("账号"), account);
  form->addRow(QStringLiteral("口令"), password);
  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  form->addRow(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (dlg.exec() != QDialog::Accepted) return;
  if (account->text().trimmed().isEmpty()) return;
  settings.setValue(QStringLiteral("host"), host->text().trimmed());
  settings.setValue(QStringLiteral("port"), port->text().trimmed());
  settings.setValue(QStringLiteral("files_port"),
                    files_port->text().trimmed());
  settings.setValue(QStringLiteral("account"), account->text().trimmed());
  // 品牌拉取（免鉴权 GET 不依赖登录成败；accept 后 host/文件口已知，
  // 比登录成功后拉取更早见效——设计稿 §4 差异如实注明）；host 先过
  // 解析覆盖（部署批 424），品牌与登录同源
  const QString resolved_host = resolve_conn_host(host->text().trimmed());
  BrandKit::instance().fetch(
      resolved_host, static_cast<quint16>(files_port->text().toUInt()));
  login_collab(resolved_host,
               static_cast<quint16>(port->text().toUInt()),
               account->text().trimmed(), password->text());
}

// 连接设置（部署批 424）：域名→IP 解析覆盖编辑。规则串 host_overrides
// （空=不覆盖，公网解析不受影响）；hosts/内网 DNS 等标准做法见 README
// 「公网接入：Cloudflare tunnel」节——此处只管本机客户端覆盖项。
void MainWindow::show_conn_settings_dialog() {
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("连接设置"));
  auto* lay = new QVBoxLayout(&dlg);
  auto* form = new QFormLayout;
  lay->addLayout(form);
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  auto* overrides = new QLineEdit(
      settings.value(QStringLiteral("host_overrides")).toString(), &dlg);
  overrides->setPlaceholderText(
      QStringLiteral("server.memex.cuihairu.site=192.168.5.5"));
  overrides->setToolTip(QStringLiteral(
      "域名=IP，多条用分号分隔；留空=不覆盖（公网走正常解析）。"
      "命中后该域名的连接直连内网 IP，不过 tunnel。"));
  form->addRow(QStringLiteral("域名解析覆盖"), overrides);
  auto* hint = new QLabel(
      QStringLiteral("仅影响本机连接目标（优先于系统 DNS）；局域网也可用 "
                     "hosts 或内网 DNS 强制解析——见 README 接入文档。"),
      &dlg);
  hint->setWordWrap(true);
  hint->setStyleSheet(QStringLiteral("color: #7d7169;"));
  lay->addWidget(hint);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                       QDialogButtonBox::Cancel, &dlg);
  lay->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (dlg.exec() != QDialog::Accepted) return;
  const QString rules = overrides->text().trimmed();
  if (rules.isEmpty()) {
    settings.remove(QStringLiteral("host_overrides")); // 默认不覆盖：清项
  } else {
    settings.setValue(QStringLiteral("host_overrides"), rules);
  }
}

// —— 原型 rail 面（docs/design/prototypes 桌面屏 .rail，用户令 2026-10-10
//    原型走查 P0＝主导航缺失）：图标导航两套项严格按原型——
//    直连：消息／局域网设备／文件传输／搜索 ┊ 设置／头像
//    协作：消息／组织架构／文件／共享空间（只读）┊ 个人／头像(在线点)
//    「共享空间」全工程（客户端/服务端）均无功能体，导航位照原型落位但
//    置灰，不冒充可用入口；文件传输（desktop-files）与组织架构
//    （desktop-org）均已切整屏视图；好友视图占协作 rail 第二位
//    （desktop-friends）。消息工作区常亮＝本版不换屏（残差如实报）。——
void MainWindow::rebuild_rail() {
  if (!rail_) return;
  const bool collab = collab_engine_.is_logged_in();
  QVector<RailNav::Item> items;
  RailNav::Item messages;
  messages.id = QStringLiteral("messages");
  messages.tip = QStringLiteral("消息");
  messages.icon = RailNav::Icon::Chat;
  items.append(messages);
  if (collab) {
    // 好友视图入口（原型 desktop-friends.html：侧栏好友分组 IA）
    RailNav::Item friends;
    friends.id = QStringLiteral("friends");
    friends.tip = QStringLiteral("好友");
    friends.icon = RailNav::Icon::Person;
    items.append(friends);
    RailNav::Item org;
    org.id = QStringLiteral("org");
    org.tip = QStringLiteral("组织架构");
    org.icon = RailNav::Icon::Org;
    items.append(org);
    RailNav::Item files;
    files.id = QStringLiteral("files");
    files.tip = QStringLiteral("文件");
    files.icon = RailNav::Icon::Folder;
    items.append(files);
    RailNav::Item share;
    share.id = QStringLiteral("share");
    share.tip = QStringLiteral("共享空间（只读）");
    share.icon = RailNav::Icon::Share;
    share.disabled = true;
    items.append(share);
  } else {
    RailNav::Item devices;
    devices.id = QStringLiteral("devices");
    devices.tip = QStringLiteral("局域网设备");
    devices.icon = RailNav::Icon::Devices;
    items.append(devices);
    RailNav::Item files;
    files.id = QStringLiteral("files");
    files.tip = QStringLiteral("文件传输");
    files.icon = RailNav::Icon::Folder;
    items.append(files);
    RailNav::Item search;
    search.id = QStringLiteral("search");
    search.tip = QStringLiteral("搜索");
    search.icon = RailNav::Icon::Search;
    items.append(search);
  }
  RailNav::Item spacer;
  spacer.spacer = true; // 原型 .rail .spacer：上下两组分隔
  items.append(spacer);
  if (collab) {
    RailNav::Item profile;
    profile.id = QStringLiteral("profile");
    profile.tip = QStringLiteral("个人");
    profile.icon = RailNav::Icon::Person;
    items.append(profile);
  } else {
    RailNav::Item settings;
    settings.id = QStringLiteral("settings");
    settings.tip = QStringLiteral("设置");
    settings.icon = RailNav::Icon::Gear;
    items.append(settings);
  }
  RailNav::Item avatar;
  avatar.id = QStringLiteral("avatar");
  avatar.tip = QStringLiteral("个人资料");
  avatar.avatar = true;
  items.append(avatar);
  rail_->set_items(items);
  rail_->set_active(QStringLiteral("messages"));
  rail_->apply_tokens(tk());
}

void MainWindow::rail_clicked(const QString& id) {
  // 本版消息工作区＝设备/会话同一列表（原型直连分「消息/局域网设备」两屏，
  // 合并口径残差如实报）；两入口都落焦点到列表。
  if (id == QStringLiteral("messages") || id == QStringLiteral("devices")) {
    leave_transfers_view(); // 传输面 → 会话/列表态（几何随态收口）
    leave_org_view();       // 组织架构面 → 会话/列表态
    if (sidebar_mode_ != QStringLiteral("sessions")) {
      sidebar_mode_ = QStringLiteral("sessions");
      update_side_head();
      refresh_devices();
    }
    rail_->set_active(id);
    device_list_->setFocus();
    return;
  }
  if (id == QStringLiteral("friends")) {
    leave_transfers_view();
    leave_org_view();
    if (sidebar_mode_ != QStringLiteral("friends")) {
      sidebar_mode_ = QStringLiteral("friends");
      update_side_head();
      refresh_devices();
    }
    rail_->set_active(id);
    device_list_->setFocus();
    return;
  }
  if (id == QStringLiteral("files")) {
    // 文件传输整屏（原型 desktop-files.html）；文件助手对话框仍在
    // 「协作 → 文件助手…」菜单
    enter_transfers_view();
    return;
  }
  if (id == QStringLiteral("org")) {
    // 组织架构整屏三栏（原型 desktop-org.html）
    enter_org_view();
    return;
  }
  if (id == QStringLiteral("search")) {
    search_box_->setFocus();
    search_box_->selectAll();
    return;
  }
  if (id == QStringLiteral("settings")) {
    if (opt_menu_) opt_menu_->popup(QCursor::pos());
    return;
  }
  if (id == QStringLiteral("profile") || id == QStringLiteral("avatar")) {
    show_profile_dialog();
    return;
  }
}

void MainWindow::show_file_assistant() {
  if (!file_assistant_) {
    file_assistant_ = new FileAssistantDialog(this);
    file_assistant_->setAttribute(Qt::WA_DeleteOnClose);
  }
  file_assistant_->show();
  file_assistant_->raise();
  file_assistant_->activateWindow();
}

// 侧栏头随形态切换（原型：直连「局域网设备 · N 台在线」／协作「消息」／
// 好友视图「好友」）；搜索占位同换（协作「搜索消息 / 联系人 / 群聊」、好友
// 「搜索好友 / 工号」）；推广卡仅直连可见。
// rail 两套项只在模式翻转时重建（refresh_devices 高频调用）。
void MainWindow::update_side_head() {
  const bool collab = collab_engine_.is_logged_in();
  const bool friends = collab && sidebar_mode_ == QStringLiteral("friends");
  if (side_title_) {
    side_title_->setText(friends ? QStringLiteral("好友")
                                 : (collab ? QStringLiteral("消息")
                                           : QStringLiteral("局域网设备")));
  }
  if (search_box_) {
    search_box_->setPlaceholderText(
        friends ? QStringLiteral("搜索好友 / 工号")
                : (collab ? QStringLiteral("搜索消息 / 联系人 / 群聊")
                          : QStringLiteral("搜索设备 / IP")));
  }
  if (disc_hint_) {
    // 注脚随视图：直连＝发现口径；好友＝通讯录来源（应用无加友审批流，
    // 不照抄原型「须经对方同意」）
    disc_hint_->setText(friends ? QStringLiteral("来自组织架构通讯录")
                                : disc_direct_text_);
    disc_hint_->setVisible(!collab || friends);
  }
  if (device_count_) device_count_->setVisible(!collab || friends);
  if (collab_promo_) collab_promo_->setVisible(!collab);
  if (new_chat_btn_) new_chat_btn_->setVisible(collab);
  const QString mode =
      collab ? QStringLiteral("collab") : QStringLiteral("direct");
  if (!collab && sidebar_mode_ != QStringLiteral("sessions") &&
      sidebar_mode_ != QStringLiteral("transfers")) {
    sidebar_mode_ = QStringLiteral("sessions"); // 登出即回会话视图（传输面
                                                // 两形态都有，保留）
  }
  if (rail_ && rail_mode_ != mode) {
    rail_mode_ = mode;
    rebuild_rail();
    rail_->set_avatar_text(collab ? collab_engine_.account()
                                  : CollabEngine::device_name());
  }
  if (rail_) {
    rail_->set_presence(collab, collab && online_accounts_.contains(
                                                   collab_engine_.account()));
  }
  // 视图互斥收口（传输面时盖回上方对会话侧栏控件的显隐写）
  apply_view_mode();
}

void MainWindow::refresh_devices() {
  update_side_head();
  const QString selected = current_peer_;
  device_list_->blockSignals(true);
  device_list_->clear();
  // 好友视图（原型 desktop-friends.html）：侧栏只摆组织架构通讯录
  //（星标好友＋部门分组）；设备行/会话行/临时群都在消息视图
  const bool friends_view =
      collab_engine_.is_logged_in() &&
      sidebar_mode_ == QStringLiteral("friends");
  if (friends_view) {
    struct FEntry {
      QString account, display, dept;
    };
    QList<FEntry> roster;
    const QJsonDocument od =
        QJsonDocument::fromJson(last_org_json_.toUtf8());
    if (od.isObject()) {
      for (const auto& m :
           od.object().value(QStringLiteral("members")).toArray()) {
        const QJsonObject mo = m.toObject();
        FEntry e;
        e.account = mo.value(QStringLiteral("account")).toString();
        e.display = mo.value(QStringLiteral("display_name")).toString();
        e.dept = mo.value(QStringLiteral("department_path")).toString();
        if (!e.account.isEmpty() && e.account != collab_engine_.account())
          roster.push_back(e);
      }
    }
    int online = 0;
    for (const FEntry& e : roster)
      if (online_accounts_.contains(e.account)) ++online;
    device_count_->setText(
        roster.isEmpty()
            ? QStringLiteral("组织架构未下发")
            : QStringLiteral("<span style=\"color:%1;\">●</span> %2 / %3 在线")
                  .arg(tk().success.name())
                  .arg(online)
                  .arg(roster.size()));
    auto add_header = [this](const QString& title) {
      auto* header = new QListWidgetItem(title);
      header->setFlags(Qt::NoItemFlags);
      device_list_->addItem(header);
    };
    auto add_friend_row = [this](const FEntry& e) {
      const QString shown =
          e.display.isEmpty() ? e.account : e.display;
      // 原型行名「人名 · 部门」（部门取路径末级；未分配不拼）
      QString leaf;
      if (!e.dept.isEmpty()) leaf = e.dept.section(QChar('/'), -1);
      const QString rowname =
          leaf.isEmpty()
              ? shown
              : QStringLiteral("%1 · %2").arg(shown, leaf);
      // 末条预览：与协作会话行同口径（撤回如实标；无历史「暂无消息」）
      QString preview;
      const auto hist = direct_engine_.history(e.account, 1);
      if (!hist.isEmpty()) {
        const QString text =
            hist.last().recalled
                ? QStringLiteral("撤回了一条消息")
                : QString::fromStdString(hist.last().text);
        preview = hist.last().from == collab_engine_.account().toStdString()
                      ? QStringLiteral("我：%1").arg(text)
                      : text;
      } else {
        preview = QStringLiteral("暂无消息");
      }
      auto* item = new QListWidgetItem(device_list_);
      QWidget* row = make_session_row(
          tk(), shown, online_accounts_.contains(e.account) ? 1 : 0,
          rowname, fmt_last_contact(fav_last_ms_(e.account)), preview,
          unread_counts_.value(e.account));
      item->setSizeHint(row->sizeHint());
      device_list_->setItemWidget(item, row);
      item->setData(Qt::UserRole, e.account);
      item->setData(Qt::UserRole + 1, QStringLiteral("collab"));
      item->setData(Qt::UserRole + 6,
                    QStringList{e.account, e.display, e.dept}.join(
                        QLatin1Char('\n')));
      item->setToolTip(QStringLiteral("账号：%1\n部门：%2")
                           .arg(e.account,
                                e.dept.isEmpty()
                                    ? QStringLiteral("（未分配）")
                                    : e.dept));
    };
    if (roster.isEmpty()) {
      auto* hint = new QListWidgetItem(
          QStringLiteral("组织架构未下发（登录后自动拉取）"));
      hint->setFlags(Qt::NoItemFlags);
      device_list_->addItem(hint);
    } else {
      QList<FEntry> starred, plain;
      for (const FEntry& e : roster)
        (fav_is_starred_(e.account) ? starred : plain).push_back(e);
      if (!starred.isEmpty()) {
        add_header(QStringLiteral("星标好友"));
        for (const FEntry& e : starred) add_friend_row(e);
      }
      // 部门分组：全路径为键（末级同名不撞），组内按显示名
      std::sort(plain.begin(), plain.end(),
                [](const FEntry& a, const FEntry& b) {
                  return a.display < b.display;
                });
      QMap<QString, QList<FEntry>> by_dept;
      for (const FEntry& e : plain)
        by_dept[e.dept.isEmpty() ? QStringLiteral("（未分配部门）")
                                 : e.dept].push_back(e);
      for (auto it = by_dept.constBegin(); it != by_dept.constEnd(); ++it) {
        add_header(QStringLiteral("%1 · %2 人").arg(it.key()).arg(it->size()));
        for (const FEntry& e : it.value()) add_friend_row(e);
      }
    }
    device_list_->blockSignals(false);
    if (!selected.isEmpty()) {
      for (int i = 0; i < device_list_->count(); ++i) {
        if (device_list_->item(i)->data(Qt::UserRole).toString() ==
            selected) {
          device_list_->setCurrentRow(i, QItemSelectionModel::NoUpdate);
          break;
        }
      }
    }
    return;
  }
  const auto peers = direct_engine_.peers();
  // 原型直连列表（desktop-direct.html）：「在线设备」一组会话行（头像/在
  // 线点/姓名·设备/时间/末条预览/未读胶囊）。离线对端由发现服务超时即摘，
  // 无离线留存表——「离线设备」分区不渲染（台账近似项）。
  if (!peers.isEmpty()) {
    auto* header = new QListWidgetItem(QStringLiteral("在线设备"));
    header->setFlags(Qt::NoItemFlags);
    device_list_->addItem(header);
  }
  QList<QPair<qint64, const Peer*>> order;
  for (const Peer& p : peers) order.push_back({fav_last_ms_(QString::fromStdString(p.device_id)), &p});
  std::sort(order.begin(), order.end(),
            [](const QPair<qint64, const Peer*>& a,
               const QPair<qint64, const Peer*>& b) {
              if (a.first != b.first) return a.first > b.first;
              return a.second->name < b.second->name;
            });
  for (const auto& kv : order) {
    const Peer& p = *kv.second;
    auto* item = new QListWidgetItem(device_list_);
    const QString id = QString::fromStdString(p.device_id);
    const QString name =
        p.name.empty() ? id.left(8) : QString::fromStdString(p.name);
    // 飞秋对端（"ipmsg:" 前缀）无 TCP 直连口：元信息行走 UDP 标注
    const bool is_ipmsg = id.startsWith(QStringLiteral("ipmsg:"));
    const QString meta =
        is_ipmsg
            ? QStringLiteral("飞秋 · UDP %1").arg(p.address.toString())
            : QStringLiteral("%1 · TCP %2")
                  .arg(p.address.toString(), QString::number(p.tcp_port));
    // T4.2：对端协作账号（仅作显示；空=未登录）——原型行「人名 · 设备名」，
    // 应用侧账号 id 即人名位
    const QString account = QString::fromStdString(p.account);
    const QString display =
        account.isEmpty() ? name
                          : QStringLiteral("%1 · %2").arg(account, name);
    // 末条预览（原型 .r-sub）：本地库最近一条（撤回如实标）；文件传输
    // 不落库，无历史走元信息行兜底（IP · 口）
    QString preview;
    const auto hist = direct_engine_.history(id, 1);
    if (!hist.isEmpty()) {
      preview = hist.last().recalled
                    ? QStringLiteral("撤回了一条消息")
                    : QString::fromStdString(hist.last().text);
    } else {
      preview = meta;
    }
    // 「盯」已读提醒高亮（原文本行 ◉ 前缀位；打开该会话即清）
    const QString shown =
        (watch_reminded_peers_.contains(id) ? QStringLiteral("◉ ")
                                            : QString()) +
        display;
    // 列表内对端都在发现超时窗内＝在线（presence=1）；头像首字取人名位
    QWidget* row = make_session_row(
        tk(), account.isEmpty() ? name : account, 1, shown,
        fmt_last_contact(fav_last_ms_(id)), preview,
        unread_counts_.value(id));
    item->setSizeHint(row->sizeHint());
    device_list_->setItemWidget(item, row);
    // 元信息（IP/端口/协作账号）退悬浮——行面让位原型会话行
    item->setToolTip(QStringLiteral("%1\n%2%3").arg(
        display, meta,
        account.isEmpty()
            ? QString()
            : QStringLiteral("\n协作账号 %1").arg(account)));
    item->setData(Qt::UserRole, id);
    item->setData(Qt::UserRole + 1, QStringLiteral("direct"));
    // 需求批⑨查找：可检索字段＝昵称/设备 id/IP/协作账号（\n 分行，整行比对
    // ＝精确、子串＝模糊，两态都能命中）
    item->setData(Qt::UserRole + 6,
                  QStringList{name, id, p.address.toString(), account}
                      .join(QLatin1Char('\n')));
  }

  // 协作会话（本地历史 + 本会话窗口期的对端）与服务端群合并成原型两组
  //（desktop-collab.html 侧栏）：置顶（星标）/最近联系（其余）——群行与
  // 人行同列混排（原型产品研发部/MES 群均在两组内），行控件化（头像/在
  // 离线点/末条预览/未读胶囊）替原两行纯文本。
  if (collab_engine_.is_logged_in() &&
      (!collab_peers_.isEmpty() || !groups_.isEmpty())) {
    struct Entry {
      QString key, kind, name;
      quint64 gid{0};
      int members{0};
    };
    QList<Entry> entries;
    for (const QString& a : collab_peers_)
      entries.push_back({a, QStringLiteral("collab"), a, 0, 0});
    for (auto it = groups_.constBegin(); it != groups_.constEnd(); ++it) {
      entries.push_back({QStringLiteral("group:%1").arg(it.key()),
                         QStringLiteral("group"), it->name, it.key(),
                         int(it->members.size())});
    }
    // T4.5：星标置顶（最近优先）、其余按最近联系、再按显示名
    std::sort(entries.begin(), entries.end(),
              [this](const Entry& a, const Entry& b) {
                const bool sa = fav_is_starred_(a.key),
                           sb = fav_is_starred_(b.key);
                if (sa != sb) return sa;
                const qint64 ta = fav_last_ms_(a.key), tb = fav_last_ms_(b.key);
                if (ta != tb) return ta > tb;
                return a.name < b.name;
              });
    QList<Entry> starred, recent;
    for (const Entry& e : entries)
      (fav_is_starred_(e.key) ? starred : recent).push_back(e);
    auto add_header = [this](const QString& title) {
      auto* header = new QListWidgetItem(title);
      header->setFlags(Qt::NoItemFlags); // 分组标题：不可选（空 id 不进会话）
      device_list_->addItem(header);
    };
    auto add_session_row = [this](const Entry& e) {
      const bool group = e.kind == QStringLiteral("group");
      auto* item = new QListWidgetItem(device_list_);
      // 末条预览（原型 .r-sub）：本地库最近一条（撤回行如实标）；文件
      // 不落消息库（会话内存态），预览只反映文本消息。无历史走归档口径
      // 兜底行（群行保留人数/群号信息位）。
      QString preview;
      const auto hist = direct_engine_.history(e.key, 1);
      if (!hist.isEmpty()) {
        const auto& m = hist.last();
        const QString text =
            m.recalled ? QStringLiteral("撤回了一条消息")
                       : QString::fromStdString(m.text);
        if (m.from == collab_engine_.account().toStdString()) {
          preview = QStringLiteral("我：%1").arg(text);
        } else if (group) {
          preview = QStringLiteral("%1：%2")
                        .arg(QString::fromStdString(m.from), text);
        } else {
          preview = text;
        }
      } else if (group) {
        // 原型 .r-sub「N 人 · 群公告已更新」：有公告走公告口径，无公告保
        // 留群号＋归档信息位
        const GroupEntry& ge = groups_.value(e.gid);
        preview = !ge.announcement.isEmpty()
                      ? QStringLiteral("%1 人 · 群公告已更新")
                            .arg(QString::number(e.members))
                      : QStringLiteral("%1 人 · 群 %2 · 已归档")
                            .arg(QString::number(e.members),
                                 QString::number(e.gid));
      } else {
        preview = QStringLiteral("协作态 · 已归档");
      }
      // 「盯」已读提醒高亮（原文本行 ◉ 前缀位；打开该会话即清）
      const QString name =
          (watch_reminded_peers_.contains(e.key) ? QStringLiteral("◉ ")
                                                 : QString()) +
          e.name;
      // T4.3：在离线点随服务端推送刷新（在线表含自己；群行无点）
      QWidget* row =
          make_session_row(tk(), e.name,
                           group ? -1
                                 : (online_accounts_.contains(e.key) ? 1 : 0),
                           name, fmt_last_contact(fav_last_ms_(e.key)),
                           preview, unread_counts_.value(e.key));
      item->setSizeHint(row->sizeHint());
      device_list_->addItem(item);
      device_list_->setItemWidget(item, row);
      item->setData(Qt::UserRole, e.key);
      item->setData(Qt::UserRole + 1, e.kind);
      if (group) {
        // ⑨：群名/群号/成员账号（按成员找群＝找联系人的群面）
        item->setData(Qt::UserRole + 6,
                      e.name + QLatin1Char('\n') + QString::number(e.gid) +
                          QLatin1Char('\n') +
                          groups_.value(e.gid).members.join(
                              QLatin1Char('\n')));
      } else {
        item->setData(Qt::UserRole + 6, e.key); // ⑨：账号精确/模糊皆命中
        // 需求批⑪：签名悬浮可见（空=未设不显）
        QStringList tips;
        const QString sig = member_signatures_.value(e.key);
        if (!sig.isEmpty()) {
          tips << QStringLiteral("个性签名：%1").arg(sig);
        }
        // 需求批⑩：在线时长悬浮可见（滚动 24h/7d/30d 并集时长）
        const QString dur = member_online_.value(e.key);
        if (!dur.isEmpty()) {
          tips << dur;
        }
        // 需求批⑫：头像悬浮可见（富文本首行；已设走缓存文件，未设走默认组）
        QString tip_html =
            QStringLiteral("<img src='%1' width='32' height='32'>")
                .arg(avatar_tooltip_src(
                    e.key, member_avatar_ver_.value(e.key), 32));
        if (!tips.isEmpty()) {
          tip_html += QStringLiteral("<br>") + tips.join(QLatin1String("<br>"));
        }
        item->setToolTip(tip_html);
      }
    };
    if (!starred.isEmpty()) {
      add_header(QStringLiteral("置顶"));
      for (const Entry& e : starred) add_session_row(e);
    }
    if (!recent.isEmpty()) {
      add_header(QStringLiteral("最近联系"));
      for (const Entry& e : recent) add_session_row(e);
    }
  }

  // 免服务端临时群（直连态，T4.1）：点对点扇出，不进归档——行控件化对齐
  // 原型（desktop-group.html 列表临群行：opacity .7 弱化＋「免服务端 · 仅
  // 本网段 · 不归档」副行＋.noarch「未归档」徽标）
  if (!dgroup_members_.isEmpty()) {
    auto* header = new QListWidgetItem(QStringLiteral("临时群 · 不进归档"));
    header->setFlags(Qt::NoItemFlags);
    device_list_->addItem(header);
    for (auto it = dgroup_members_.constBegin(); it != dgroup_members_.constEnd();
         ++it) {
      auto* item = new QListWidgetItem(device_list_);
      const QString title = dgroup_title(it.key());
      QWidget* row = make_session_row(tk(), title, -1, title, QString(),
                                      QStringLiteral("免服务端 · 仅本网段 · 不归档"),
                                      unread_counts_.value(it.key()),
                                      QStringLiteral("未归档"));
      auto* dim = new QGraphicsOpacityEffect(row);
      dim->setOpacity(0.7); // 原型临时群行 opacity:.7（不归档群视觉弱化）
      row->setGraphicsEffect(dim);
      item->setSizeHint(row->sizeHint());
      device_list_->addItem(item);
      device_list_->setItemWidget(item, row);
      item->setData(Qt::UserRole, it.key());
      item->setData(Qt::UserRole + 1, QStringLiteral("dgroup"));
      item->setToolTip(QStringLiteral("%1 台设备 · 点对点扇出")
                           .arg(it->size()));
      // ⑨：临群名/成员设备 id
      item->setData(Qt::UserRole + 6,
                    QStringList{title}.join(QLatin1Char('\n')) +
                        QLatin1Char('\n') + it->join(QLatin1Char('\n')));
    }
  }
  device_list_->blockSignals(false);
  // 原型 device-count：success 绿点＋muted 计数；空表如实「未发现设备」
  device_count_->setText(
      peers.isEmpty()
          ? QStringLiteral("未发现设备")
          : QStringLiteral("<span style=\"color:%1;\">●</span> %2 台在线")
                .arg(tk().success.name())
                .arg(peers.size()));

  // 当前会话设备离线 → 保留视图但提示；列表选中态恢复
  if (!selected.isEmpty()) {
    for (int i = 0; i < device_list_->count(); ++i) {
      if (device_list_->item(i)->data(Qt::UserRole).toString() == selected) {
        device_list_->setCurrentRow(i, QItemSelectionModel::NoUpdate);
        break;
      }
    }
  }

  // 未选中会话时的空态：无设备显示发现引导；有设备提示先选择
  if (selected.isEmpty()) {
    if (peers.isEmpty()) {
      show_guidance();
    } else if (chat_showing_guidance_) {
      chat_showing_guidance_ = false;
      chat_view_->clear();
      chat_rows_.clear();
      append_system_line(QStringLiteral("从左侧选择设备，开始点对点会话"));
    }
  }
}

void MainWindow::open_peer(const QString& device_id) {
  open_chat(QStringLiteral("direct"), device_id);
}

// 打开会话（kind=direct 设备 / collab 账号）：历史取自共享本地库——
// 直连与协作消息同库合并，按 source 字段标注来源（T2.4 合并展示）。
// 关会话回列表态（布局令）：清当前会话（历史在库里，重开即重渲），
// 面板收起、窗口缩回「左菜单+好友列表」长方形。启动也走此路＝无会话
// 必然坍缩到列表态尺寸（2026-10-15 缺陷修复：此前启动保持 1280 横窗）
void MainWindow::close_chat() {
  current_kind_.clear();
  current_peer_.clear();
  chat_rows_.clear();
  chat_view_->clear();
  chat_panel_->hide();
  chat_open_ = false; // 布局态收回列表态（存档键/最小尺寸判据）
  // 群会话附属面随会话收起（右栏/头部群钮/@提示——下次开非群会话不带出）
  if (group_panel_) group_panel_->hide();
  if (head_announce_btn_) head_announce_btn_->hide();
  if (head_gfiles_btn_) head_gfiles_btn_->hide();
  if (head_members_btn_) head_members_btn_->hide();
  if (at_hint_) at_hint_->hide();
  if (at_menu_) at_menu_->hide();
  chat_title_->setToolTip(QString());
  // 列表态：列表列占满 rail 之外的全部窗宽（微信/QQ 启动即列表；无论
  // 启动还是关会话，右缘都不再留空白——2026-10-14 用户报）
  side_->setMaximumWidth(QWIDGETSIZE_MAX);
  side_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  apply_state_minimum();
  if (!isMaximized() && width() > 400) resize(320, height());
  // 列表态收紧窗口右缘上限＝左侧列内容宽（rail 60＋会话列 260）：拖窗口右缘
  // 不再把列表与右框之间撑出死空间（2026-10-11 用户报——列表态拉宽窗口，
  // 对话面板已收起，右侧空档与列表视觉分离）。拉长只应发生在会话态（右窗
  // 可见时由 chat 面吸收）。最大化不受本上限约束（受屏幕几何支配）。
  if (!isMaximized()) setMaximumWidth(rail_->minimumWidth() + side_->minimumWidth());
}

// 最小尺寸随布局态（Telegram TGSplitView 每态各有一档最小宽度）：列表态
// 下限＝左侧两列内容宽（rail 60＋列表列 260）；会话态＝三列列宽之和
//（Telegram columnMinimalWidthLeft/Main 口径），与内容 sizeHint 解耦。
void MainWindow::apply_state_minimum() {
  if (chat_open_) {
    // 会话态＝rail＋列表列＋会话列三列全显：60＋260＋380（Telegram
    // columnMinimalWidthLeft/Main 口径＝列最小宽之和，与内容 sizeHint
    // 解耦）。不能用布局 minimumSizeHint：聊天输入行一串文字按钮把自然
    // 下限顶到 892——那是内容的下限不是窗口的下限，钳住反而把用户此前
    // 可自由缩窄的会话窗锁死；380 只保证会话列最小可读，工具条挤窄是
    // 既有表现（修复前窗口可缩到 320，内容同样挤压）
    setMinimumSize(rail_->minimumWidth() + side_->minimumWidth() + 380, 420);
  } else {
    setMinimumSize(320, 420);
  }
}

// —— 窗口几何持久化（同类 IM 通行做法：Telegram savePosition／Slack
// mainWindowSettings.bounds／Discord WINDOW_BOUNDS 均记住上次尺寸与位置）——
// 存档按布局态分键（Telegram TGWindowArchiver 按视图态归档同构）：列表态
// 与会话态各自存档，恢复时只取「当前态」的存档——会话态的宽窗不盖回无会话
// 态（2026-10-15 缺陷：启动无会话却按上次会话宽显示）。默认几何只在无存档
// 用。QSettings 与本文件其他键同口径（组织/应用名）。
void MainWindow::save_window_geometry() {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  if (isMaximized()) return; // 最大化态不存（恢复时按普通态处理）
  settings.setValue(chat_open_
                        ? QStringLiteral("window_geometry_chat")
                        : QStringLiteral("window_geometry_list"),
                    saveGeometry());
}

// 启动恢复：启动必然是列表态（布局令＝构造即 close_chat），只取列表态存档；
// 无存档＝保持默认列表态尺寸（close_chat 已钳 320 宽）。
void MainWindow::restore_window_geometry() {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  const QByteArray geom =
      settings.value(QStringLiteral("window_geometry_list")).toByteArray();
  // 首次启动＝无存档：保持默认几何（close_chat 已钳 320 宽）
  if (!geom.isEmpty() && restoreGeometry(geom) && !isMaximized() &&
      width() > 400)
    // 列表态必为 320 宽：旧版（单键时代）留下的宽窗存档兜底
    resize(320, height());
  // 列表态右缘上限无条件复归：空档新装（close_chat 已设）与有存档路径同
  // 一出口——只在 restoreGeometry 成功分支里设上限＝新装环境上限恒放开
  // （布局腿实录假红）
  if (!isMaximized())
    setMaximumWidth(rail_->minimumWidth() + side_->minimumWidth());
}

// 会话态恢复（open_chat 展开用）：优先会话态存档，无则退旧单键存档（旧版
// 只存过 ≥480 宽的窗——列表窄条从不存档，宽窗必然属会话态），再失败返回
// false 由调用方落会话态默认尺寸。返回值＝是否已恢复。
bool MainWindow::restore_chat_geometry() {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  QByteArray geom =
      settings.value(QStringLiteral("window_geometry_chat")).toByteArray();
  if (geom.isEmpty())
    geom = settings.value(QStringLiteral("window_geometry")).toByteArray();
  if (geom.isEmpty()) return false;
  if (!restoreGeometry(geom)) return false;
  if (isMaximized()) return true;
  if (width() < 520) resize(1280, height()); // 窄/损坏存档兜底：会话态默认宽
  return true;
}

void MainWindow::open_chat(const QString& kind, const QString& id) {
  // 传输面/组织架构面下打开会话（通知点入/传输卡「所在会话」/档案「发消息」）：
  // 先回会话视图
  if (sidebar_mode_ == QStringLiteral("transfers")) leave_transfers_view();
  if (sidebar_mode_ == QStringLiteral("org")) leave_org_view();
  // 布局令：对话面板只在打开会话后展开（列表态点好友／菜单／通知点入）；
  // 从列表态展开时若窗口还停在窄长方形，恢复会话态几何（Telegram 按态归档
  // 优先，无存档＝默认宽 1280×当前高）
  setMaximumWidth(QWIDGETSIZE_MAX); // 释放列表态的右缘上限（见 close_chat）
  if (!chat_open_ && !isMaximized() && width() < 520) {
    if (!restore_chat_geometry()) resize(1280, height());
  }
  // 会话态：列表列收回固定宽，余量交给 chat 面板（stretch 100）
  side_->setFixedWidth(260);
  chat_panel_->show();
  chat_open_ = true; // 布局态切到会话态（存档键/最小尺寸判据）
  apply_state_minimum();
  current_kind_ = kind;
  current_peer_ = id;
  // 打开即读：清该会话未读角标并重渲行（原型行 n-count 只在列表态显）
  if (unread_counts_.remove(id) > 0) refresh_devices();
  // 「盯」已读提醒高亮同口径：打开该会话即清（已到场，不再占角标位）
  if (watch_reminded_peers_.remove(id) > 0) refresh_devices();
  chat_showing_guidance_ = false;
  chat_view_->clear();
  chat_rows_.clear(); // 换会话即清空重渲记录（否则旧行会在切换主题时冒出来）

  const bool collab = kind == QStringLiteral("collab");
  const bool group = kind == QStringLiteral("group");
  // 群专用头部钮（原型 .ch-actions）与非群会话一律隐藏
  if (head_announce_btn_) head_announce_btn_->setVisible(group);
  if (head_gfiles_btn_) head_gfiles_btn_->setVisible(group);
  if (head_members_btn_) head_members_btn_->setVisible(group);
  if (at_menu_) at_menu_->hide(); // 换会话即收 @ 成员菜单
  chat_title_->setToolTip(QString()); // 群号悬浮提示（群分支再挂，其余清空）
  QString avatar_src; // 头像取字源（原型 .chat-head .avatar.sm 首字）
  if (collab) {
    chat_title_->setText(id);
    chat_meta_->setText(QStringLiteral("%1 · 协作态 · 消息进入服务端归档")
                            .arg(id));
    avatar_src = id;
  } else if (group) {
    // 服务端群会话（T4.1）：标题群名；meta 行走原型口径（群成员 · 群主 ·
    // 全量归档，见 group_meta_text），群号挪到标题悬浮提示
    const quint64 gid = id.mid(QStringLiteral("group:").size()).toULongLong();
    const GroupEntry g = groups_.value(gid);
    const QString gname = g.name.isEmpty()
                              ? QStringLiteral("群 %1").arg(gid)
                              : g.name;
    chat_title_->setText(gname);
    chat_title_->setToolTip(QStringLiteral("群 %1").arg(gid));
    avatar_src = gname;
    chat_meta_->setText(group_meta_text(g));
  } else if (kind == QStringLiteral("dgroup")) {
    chat_title_->setText(dgroup_title(id));
    chat_meta_->setText(
        QStringLiteral("%1 台设备 · 点对点扇出 · 消息不进归档；对端回复落在"
                       "与其的单聊会话")
            .arg(dgroup_members_.value(id).size()));
    avatar_src = dgroup_title(id);
  } else {
    const Peer p = direct_engine_.peer(id.toStdString());
    const QString name = p.name.empty() ? id.left(8) : QString::fromStdString(p.name);
    // 原型行名「人名 · 设备名」：对端登录账号即人名位（未登录＝纯设备名）
    const QString account = QString::fromStdString(p.account);
    chat_title_->setText(account.isEmpty()
                             ? name
                             : QStringLiteral("%1 · %2").arg(account, name));
    avatar_src = account.isEmpty() ? name : account;
    // 平台-7：降级态 meta 固定「已降级 · 未归档」前缀（服务端不可达对本
    // 会话恒真，压过跨态——更具体的口径）；T4.2 跨态次之（A7）
    if (collab_degraded_) {
      chat_meta_->setText(
          QStringLiteral("已降级 · 未归档 · %1 · TCP %2")
              .arg(p.address.toString(), QString::number(p.tcp_port)));
    } else if (is_cross_state(id)) {
      chat_meta_->setText(
          QStringLiteral("跨态 · 未归档 · %1 · TCP %2")
              .arg(p.address.toString(), QString::number(p.tcp_port)));
    } else {
      chat_meta_->setText(
          QStringLiteral("%1 · 点对点直连 · TCP %2")
              .arg(p.address.toString(), QString::number(p.tcp_port)));
    }
  }
  chat_avatar_src_ = avatar_src;
  update_chat_avatar();
  update_banner();
  update_group_panel(); // 群右栏随会话换内容/显隐（原型 .infopanel）

  // 需求批⑧：筛选态与挂起跳转只属于当前会话——换会话即复位
  filter_from_ms_ = 0;
  filter_until_ms_ = 0;
  pending_jump_ = QDate();
  render_open_history();
}

// 会话历史渲染（open_chat 尾段抽取，按日期跳转/清筛选复用）：跨态标记→
// 空历史引导→双态合并行→已读上报（⑦开关裁决）→回执态补查（⑦）。
void MainWindow::render_open_history() {
  const QString kind = current_kind_;
  const QString id = current_peer_;
  const bool collab = kind == QStringLiteral("collab");
  const bool group = kind == QStringLiteral("group");
  const QString my_id = (collab || group)
                            ? collab_engine_.account()
                            : QString::fromStdString(direct_engine_.device_id());
  chat_view_->clear();
  chat_rows_.clear();

  // T4.2 跨态会话固定标识（A7）＋归档起点提示（A8 客户端面）：
  // 标识常驻不可关闭，无论历史空否都打。
  if (kind == QStringLiteral("direct") && is_cross_state(id)) {
    if (collab_engine_.is_logged_in()) {
      append_chip_line(QStringLiteral(
          "跨态会话 · 未归档：与未登录终端点对点传输（标记常驻不可关闭）；"
          "归档自 %1（本次协作态登录时刻，此前跨态直连消息不进归档）")
                           .arg(QDateTime::fromMSecsSinceEpoch(collab_login_ms_)
                                    .toString(QStringLiteral(
                                        "yyyy-MM-dd HH:mm:ss"))));
    } else {
      append_chip_line(QStringLiteral(
          "跨态会话 · 未归档：对端已登录协作态，本机未登录——本机消息不进归档"
          "（标记常驻不可关闭）"));
    }
  }

  const auto hist = direct_engine_.history(id);
  if (hist.isEmpty()) {
    if (collab) {
      append_system_line(
          QStringLiteral("已与 %1 建立协作会话 · 消息经服务端转发并全量归档")
              .arg(esc(id)));
    } else if (group) {
      append_system_line(
          QStringLiteral("已打开群「%1」· 消息经服务端按成员扇出并全量归档")
              .arg(esc(chat_title_->text())));
    } else if (kind == QStringLiteral("dgroup")) {
      append_system_line(
          QStringLiteral("已建立临时群「%1」（%2 台设备）· 点对点扇出，"
                         "本会话不进归档")
              .arg(esc(chat_title_->text()))
              .arg(dgroup_members_.value(id).size()));
    } else {
      // 原型 .sys-hint 胶囊口径：发现方式＋不归档（2026-10-30 对齐文案）
      append_system_line(
          QStringLiteral("已通过 UDP 广播发现对方 · 本对话不归档"));
    }
  }
  for (const StoredMessage& m : hist) {
    append_message(QString::fromStdString(m.from),
                   QString::fromStdString(m.text), m.ts_ms,
                   m.from == my_id.toStdString(),
                   QString::fromStdString(m.source),
                   QString::fromStdString(m.msg_id),
                   QString::fromStdString(m.receipt), m.seq);
  }
  // T4.3：打开协作单聊=已读历史——对最近一条收到的协作消息上报已读
  //（打开前到达的消息此前未上报；逐条全报是噪音，只报最新一条）。
  // 需求批⑦：是否上报受会话/全局已读回执开关裁决。
  if (collab && read_receipts_enabled(id)) {
    for (auto it = hist.crbegin(); it != hist.crend(); ++it) {
      if (it->from == id.toStdString() && !it->msg_id.empty()) {
        collab_engine_.mark_read(QString::fromStdString(it->msg_id));
        break;
      }
    }
  }
  // 直连已读上报（需求批⑦·已读灰勾单 2026-10-14）：打开直连会话＝本轮
  // 消息已览，对来自对端的消息逐条上报已读（引擎按序号去重——重开聊天
  // 不重发，重启后重报一次幂等）。与协作态「只报最新一条」不同：直连无
  // 服务端台账，漏报无补拉机会，逐条才闭环；开关裁决同款。
  if (kind == QStringLiteral("direct") && read_receipts_enabled(id)) {
    for (const StoredMessage& m : hist) {
      if (m.from == id.toStdString() && m.seq != 0)
        direct_engine_.mark_read(id.toStdString(), m.seq);
    }
  }
  // 需求批⑦ 归档扩：打开会话即补查自己消息的回执态——离线期错过的
  // DELIVER_NOTICE/READ_NOTICE 经服务端台账对齐（最近 50 条封顶防滥用）。
  if ((collab || group) && collab_engine_.is_logged_in()) {
    QStringList mine;
    for (auto it = hist.crbegin(); it != hist.crend() && mine.size() < 50;
         ++it) {
      if (it->from == my_id.toStdString() && !it->msg_id.empty())
        mine.push_back(QString::fromStdString(it->msg_id));
    }
    collab_engine_.query_receipts(mine);
  }
}

// —— 需求批⑧ 聊天记录按日期 ——
// 筛选态渲染：本地索引（双态合并，idx_messages_peer_ts）按时间窗重建当前
// 会话行；头部系统行注明范围与条数；自己发出行的回执态照常补查（⑦）。
void MainWindow::render_filtered_history() {
  const bool collab = current_kind_ == QStringLiteral("collab");
  const bool group = current_kind_ == QStringLiteral("group");
  const QString my_id = (collab || group)
                            ? collab_engine_.account()
                            : QString::fromStdString(direct_engine_.device_id());
  chat_view_->clear();
  chat_rows_.clear();
  const auto hist = direct_engine_.history_between(
      current_peer_, filter_from_ms_, filter_until_ms_, 2000);
  append_system_line(QStringLiteral("已筛选：%1 ~ %2 · 共 %3 条 · "
                                   "「按日期」对话框可清除筛选")
                         .arg(day_label(filter_from_ms_),
                              day_label(filter_until_ms_))
                         .arg(hist.size()));
  for (const StoredMessage& m : hist) {
    append_message(QString::fromStdString(m.from),
                   QString::fromStdString(m.text), m.ts_ms,
                   m.from == my_id.toStdString(),
                   QString::fromStdString(m.source),
                   QString::fromStdString(m.msg_id),
                   QString::fromStdString(m.receipt), m.seq);
  }
  if ((collab || group) && collab_engine_.is_logged_in()) {
    QStringList mine;
    for (auto it = hist.crbegin(); it != hist.crend() && mine.size() < 50;
         ++it) {
      if (it->from == my_id.toStdString() && !it->msg_id.empty())
        mine.push_back(QString::fromStdString(it->msg_id));
    }
    collab_engine_.query_receipts(mine);
  }
  // 直连已读上报（需求批⑦·已读灰勾单）：筛选窗同开窗口径——窗内对端
  // 消息视为已览，逐条上报（引擎按序号去重）
  if (!collab && !group && read_receipts_enabled(current_peer_)) {
    for (const StoredMessage& m : hist) {
      if (m.from == current_peer_.toStdString() && m.seq != 0)
        direct_engine_.mark_read(current_peer_.toStdString(), m.seq);
    }
  }
}

// 范围筛选（毫秒含端点）：本地即时重建；协作/群登录态同时向服务端补拉
// 该窗（服务端归档比本地多的行——离线期/换机漏收——回包落本地索引后
// history_received 触发再次渲染）。
void MainWindow::apply_date_filter(qint64 from_ms, qint64 until_ms) {
  if (current_peer_.isEmpty() || chat_panel_->isHidden()) {
    show_status(QStringLiteral("先选择会话再按日期查看"));
    return;
  }
  // 两侧全开的窗＝未筛选（与清除同义，避免「筛选态字段全 0」歧义）
  if (from_ms <= 0 && until_ms <= 0) {
    clear_date_filter();
    return;
  }
  filter_from_ms_ = from_ms;
  filter_until_ms_ = until_ms;
  render_filtered_history();
  if ((current_kind_ == QStringLiteral("collab") ||
       current_kind_ == QStringLiteral("group")) &&
      collab_engine_.is_logged_in()) {
    collab_engine_.query_history(current_peer_, from_ms, until_ms);
  }
}

// 清除筛选恢复全量（幂等：无筛选即无动作；重渲含跨态标记与引导，与
// open_chat 同构）。
void MainWindow::clear_date_filter() {
  if (filter_from_ms_ == 0 && filter_until_ms_ == 0) return;
  filter_from_ms_ = 0;
  filter_until_ms_ = 0;
  render_open_history();
  show_status(QStringLiteral("已清除日期筛选"));
}

// 跳转到目标日首条：先按当日窗向服务端补拉（协作/群登录态；本地缺的行
// 到达后重渲再定位），本地全量重渲（保持上下文完整）后锚点滚动。
void MainWindow::jump_to_date(const QDate& date) {
  if (current_peer_.isEmpty() || chat_panel_->isHidden()) {
    show_status(QStringLiteral("先选择会话再按日期查看"));
    return;
  }
  if (!date.isValid()) return;
  pending_jump_ = date;
  if ((current_kind_ == QStringLiteral("collab") ||
       current_kind_ == QStringLiteral("group")) &&
      collab_engine_.is_logged_in()) {
    collab_engine_.query_history(current_peer_, day_start_ms(date),
                                 day_end_ms(date));
  }
  render_open_history();
  scroll_to_day(date);
  show_status(QStringLiteral("已跳转到 %1")
                  .arg(date.toString(QStringLiteral("yyyy-MM-dd"))));
}

void MainWindow::scroll_to_day(const QDate& date) {
  if (!date.isValid()) return;
  chat_view_->scrollToAnchor(QStringLiteral("d") +
                              date.toString(QStringLiteral("yyyyMMdd")));
}

// 「按日期」对话框：日历选择起始/结束日；跳转（滚到起始日首条）、
// 范围筛选（只看窗内记录）、清除筛选三动作。
void MainWindow::show_date_dialog() {
  if (current_peer_.isEmpty() || chat_panel_->isHidden()) {
    show_status(QStringLiteral("先选择会话再按日期查看"));
    return;
  }
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("按日期查看聊天记录"));
  auto* layout = new QVBoxLayout(&dlg);
  auto* form = new QFormLayout;
  auto* from_edit = new QDateEdit(QDate::currentDate(), &dlg);
  from_edit->setCalendarPopup(true);
  from_edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
  auto* until_edit = new QDateEdit(QDate::currentDate(), &dlg);
  until_edit->setCalendarPopup(true);
  until_edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
  form->addRow(QStringLiteral("起始"), from_edit);
  form->addRow(QStringLiteral("结束"), until_edit);
  layout->addLayout(form);
  layout->addWidget(new QLabel(
      QStringLiteral("跳转＝滚动到起始日第一条；筛选＝只看日期范围内的记录"
                     "（协作/群会话自动向服务端补拉）"),
      &dlg));
  auto* actions = new QHBoxLayout;
  auto* jump_btn = new QPushButton(QStringLiteral("跳转到起始日"), &dlg);
  auto* filter_btn = new QPushButton(QStringLiteral("按范围筛选"), &dlg);
  auto* clear_btn = new QPushButton(QStringLiteral("清除筛选"), &dlg);
  auto* close_btn = new QPushButton(QStringLiteral("关闭"), &dlg);
  // 清除只在筛选态可用（无筛选可清）
  clear_btn->setEnabled(filter_from_ms_ > 0 || filter_until_ms_ > 0);
  connect(jump_btn, &QPushButton::clicked, this, [&] {
    dlg.accept();
    jump_to_date(from_edit->date());
  });
  connect(filter_btn, &QPushButton::clicked, this, [&] {
    dlg.accept();
    apply_date_filter(day_start_ms(from_edit->date()),
                      day_end_ms(until_edit->date()));
  });
  connect(clear_btn, &QPushButton::clicked, this, [&] {
    dlg.accept();
    clear_date_filter();
  });
  connect(close_btn, &QPushButton::clicked, &dlg, &QDialog::reject);
  actions->addWidget(jump_btn);
  actions->addWidget(filter_btn);
  actions->addWidget(clear_btn);
  actions->addStretch();
  actions->addWidget(close_btn);
  layout->addLayout(actions);
  dlg.exec();
}

void MainWindow::append_message(const QString& from_id, const QString& text,
                                qint64 ts_ms, bool outgoing,
                                const QString& source, const QString& msg_id,
                                const QString& receipt, quint64 seq) {
  QString name = from_id;
  if (!outgoing) {
    if (current_kind_ == QStringLiteral("collab") ||
        current_kind_ == QStringLiteral("group")) {
      name = from_id; // 协作单聊／群聊对端即账号（群内显账号便于 @）
    } else {
      const Peer p = direct_engine_.peer(from_id.toStdString());
      if (!p.name.empty()) name = QString::fromStdString(p.name);
    }
  } else {
    name = QStringLiteral("我");
  }
  // 振屏标记（需求批⑥）：不进气泡，渲染居中系统行（实时与历史重载同构）。
  // 受控标记口径（同文字颜色〔#RRGGBB〕）：普通文本恰为 "[振屏]" 亦按
  // 振屏渲染——两侧落库一致，副作用可接受。
  if (text == QStringLiteral("[振屏]")) {
    const QString line =
        outgoing ? QStringLiteral("你发送了窗口抖动")
                 : QStringLiteral("%1 给你发来窗口抖动").arg(esc(name));
    append_system_line(line);
    return;
  }
  // 来源字段（合并展示）：直连＝仅本机；协作＝服务端归档
  const QString tag =
      source == QStringLiteral("collab")
          ? QStringLiteral(" · 协作·已归档")
          : QStringLiteral(" · 直连·仅本机");
  // 群聊启用 @成员 高亮（T4.1）
  const bool at_mode = current_kind_ == QStringLiteral("group");
  ChatRow row;
  row.system = false;
  row.name = name + tag;
  row.text = text;
  row.ts_ms = ts_ms;
  row.outgoing = outgoing;
  row.at_mode = at_mode;
  // 群主标签（原型 .msg-meta .role）：群内收向行且发送者＝群主
  row.owner_tag =
      at_mode && !outgoing &&
      from_id == groups_.value(current_group_id()).owner;
  row.msg_id = msg_id;
  row.seq = seq;
  row.receipt = receipt;
  // 「盯」行键（「盯」单·实现单修订）：仅我发出的 1:1 文本行可盯——
  // 直连按 (对端, 序号)、协作按 msg_id（回执同口径键）；群/临时群/图片/
  // 收向行恒空＝右键不弹盯菜单
  if (outgoing) {
    if (current_kind_ == QStringLiteral("collab") && !msg_id.isEmpty()) {
      row.watch_key = collab_watch_key(msg_id);
    } else if (current_kind_ == QStringLiteral("direct") && seq != 0) {
      row.watch_key = direct_watch_key(current_peer_, seq);
    }
  }
  if (needs_day_divider(chat_rows_, row.ts_ms))
    append_system_line(day_divider_html(
        QDateTime::fromMSecsSinceEpoch(row.ts_ms).date()));
  chat_rows_.append(row);
  chat_view_->append(day_anchor(row.ts_ms) +
                     bubble_html(row.name, row.text, row.ts_ms, row.outgoing,
                                 row.at_mode, QString(), row.receipt,
                                 row.watch_key,
                                 watched_.contains(row.watch_key),
                                 row.owner_tag));
  auto* bar = chat_view_->verticalScrollBar();
  bar->setValue(bar->maximum());
}

void MainWindow::append_image_message(const QString& from_id,
                                      const QString& image_path, qint64 ts_ms,
                                      bool outgoing, const QString& source) {
  QString name = from_id;
  if (outgoing) {
    name = QStringLiteral("我");
  } else if (current_kind_ != QStringLiteral("collab") &&
             current_kind_ != QStringLiteral("group")) {
    const Peer p = direct_engine_.peer(from_id.toStdString());
    if (!p.name.empty()) name = QString::fromStdString(p.name);
  }
  const QString tag =
      source == QStringLiteral("collab")
          ? QStringLiteral(" · 协作·已归档")
          : QStringLiteral(" · 直连·仅本机");
  ChatRow row;
  row.system = false;
  row.name = name + tag;
  row.text = image_path; // 文本位存路径（图片渲染在 image 分支）
  row.image = image_path;
  row.ts_ms = ts_ms;
  row.outgoing = outgoing;
  row.at_mode = false;
  if (needs_day_divider(chat_rows_, row.ts_ms))
    append_system_line(day_divider_html(
        QDateTime::fromMSecsSinceEpoch(row.ts_ms).date()));
  chat_rows_.append(row);
  chat_view_->append(day_anchor(row.ts_ms) +
                     bubble_html(row.name, row.text, row.ts_ms, row.outgoing,
                                 row.at_mode, row.image));
  auto* bar = chat_view_->verticalScrollBar();
  bar->setValue(bar->maximum());
}

void MainWindow::append_file_card(const QString& from_id,
                                  const QString& file_name,
                                  const QString& file_meta, qint64 ts_ms,
                                  bool outgoing) {
  QString name = from_id;
  if (outgoing) {
    name = QStringLiteral("我");
  } else if (current_kind_ != QStringLiteral("collab") &&
             current_kind_ != QStringLiteral("group")) {
    const Peer p = direct_engine_.peer(from_id.toStdString());
    if (!p.name.empty()) name = QString::fromStdString(p.name);
  }
  ChatRow row;
  row.system = false;
  row.name = name;
  row.text = file_name;
  row.file_card = true;
  row.file_meta = file_meta;
  row.ts_ms = ts_ms;
  row.outgoing = outgoing;
  if (needs_day_divider(chat_rows_, row.ts_ms))
    append_system_line(day_divider_html(
        QDateTime::fromMSecsSinceEpoch(row.ts_ms).date()));
  chat_rows_.append(row);
  chat_view_->append(day_anchor(row.ts_ms) +
                     file_bubble_html(row.name, file_name, file_meta, ts_ms,
                                      outgoing));
  auto* bar = chat_view_->verticalScrollBar();
  bar->setValue(bar->maximum());
  update_group_panel(); // 群右栏「群文件」随会话内文件卡增长
}

void MainWindow::append_system_line(const QString& text) {
  ChatRow row;
  row.system = true;
  row.text = text;
  chat_rows_.append(row);
  chat_view_->append(system_line_html(text));
  auto* bar = chat_view_->verticalScrollBar();
  bar->setValue(bar->maximum());
}

// 居中胶囊系统行（原型 .sys-hint .pill）：跨态/归档提示用——语义仍是系统行
//（不参与日序判定），但重渲按 chip 样式走
void MainWindow::append_chip_line(const QString& text) {
  ChatRow row;
  row.system = true;
  row.chip = true;
  row.text = text;
  chat_rows_.append(row);
  chat_view_->append(chip_line_html(text));
  auto* bar = chat_view_->verticalScrollBar();
  bar->setValue(bar->maximum());
}

bool MainWindow::needs_day_divider(const QVector<ChatRow>& rows,
                                   qint64 ts_ms) {
  if (ts_ms <= 0) return false;
  const QDate d = QDateTime::fromMSecsSinceEpoch(ts_ms).date();
  for (auto it = rows.crbegin(); it != rows.crend(); ++it) {
    if (it->system || it->ts_ms <= 0) continue;
    return QDateTime::fromMSecsSinceEpoch(it->ts_ms).date() != d;
  }
  return true;
}

void MainWindow::show_guidance() {
  chat_showing_guidance_ = true;
  chat_rows_.clear();
  chat_view_->clear();
  render_guidance();
}

void MainWindow::render_guidance() {
  const ThemeTokens& t = tk();
  chat_view_->append(QStringLiteral(
      "<div align=\"center\" style=\"margin-top:48px;\">"
      "<span style=\"color:%1;\">尚未发现同网段设备。</span><br><br>"
      "<span style=\"color:%2; font-size:small;\">"
      "请确认对方已安装 Memex 且与本机同一局域网；<br>"
      "直连发现使用 UDP 2425、点对点传输使用 TCP 2426–2437，"
      "请检查终端防火墙放行。<br><br>"
      "如需组织架构、云端历史与归档检索，请经左上角「协作」菜单登录协作态。"
      "</span></div>")
                         .arg(t.text.name(), t.text_muted.name()));
}

// 控件样式统一出口：所有颜色只从令牌取（R19 · T4.9）。主题切换后
// ThemeManager::theme_changed 触发重刷，无需重启界面。
void MainWindow::apply_theme_styles() {
  const ThemeTokens& t = tk();
  if (side_) {
    side_->setStyleSheet(QStringLiteral("background:%1; border-right:1px solid %2;")
                             .arg(t.surface_alt.name(), t.border.name()));
  }
  if (side_title_) {
    side_title_->setStyleSheet(
        QStringLiteral("font-weight:600; font-size:14px; color:%1;")
            .arg(t.text.name()));
  }
  if (device_count_) {
    // 原型 device-count：绿点 + text-3 计数（点走富文本 success 跨度）
    device_count_->setStyleSheet(
        QStringLiteral("color:%1; font-size:12px;").arg(t.text_muted.name()));
  }
  if (disc_hint_) {
    disc_hint_->setStyleSheet(
        QStringLiteral("color:%1; font-size:11px;").arg(t.text_muted.name()));
  }
  if (new_chat_btn_) {
    // 原型 .icon-btn.brand（brand-soft 底 brand-deep 字）＋悬停品牌底
    new_chat_btn_->setStyleSheet(
        QStringLiteral("QPushButton { background:%1; color:%2; border:none;"
                       " border-radius:8px; font-weight:700; font-size:15px; }"
                       "QPushButton:hover { background:%3; color:%4; }")
            .arg(t.brand_wash.name(), t.brand_text.name(), t.brand.name(),
                 t.on_brand.name()));
  }
  if (device_list_) {
    device_list_->setStyleSheet(QStringLiteral(
        "QListWidget { background:%1; border:1px solid %2; border-radius:8px; }"
        "QListWidget::item { padding:8px; border-bottom:1px solid %3; }"
        "QListWidget::item:selected { background:%4; color:%5; }")
        .arg(t.surface_alt.name(), t.border.name(), t.divider.name(),
             t.selection.name(), t.text.name()));
  }
  if (head_) {
    // 原型 .ch-actions .icon-btn：30px 圆角 8 透明描边、悬停浮面底
    head_->setStyleSheet(
        QStringLiteral(
            "background:%1;"
            "QPushButton { border:none; border-radius:8px; color:%2; }"
            "QPushButton:hover { background:%3; }")
            .arg(t.surface_raised.name(), t.text_muted.name(),
                 t.surface_alt.name()));
  }
  if (chat_title_) {
    chat_title_->setStyleSheet(
        QStringLiteral("font-weight:600; font-size:15px; color:%1;")
            .arg(t.text.name()));
  }
  if (chat_meta_) {
    chat_meta_->setStyleSheet(
        QStringLiteral("color:%1; font-size:12px;").arg(t.text_muted.name()));
  }
  update_chat_avatar(); // on_brand 随主题，头像字色须重取
  if (local_badge_) {
    local_badge_->setStyleSheet(
        QStringLiteral("background:%1; color:%2; border-radius:8px; "
                       "padding:2px 8px; font-size:12px;")
            .arg(t.brand_tint.name(), t.brand_text.name()));
  }
  if (chat_view_) {
    chat_view_->setStyleSheet(QStringLiteral("QTextBrowser { background:%1; border:none; }")
                                  .arg(t.chat_bg.name()));
  }
  if (input_row_) {
    input_row_->setStyleSheet(
        QStringLiteral("background:%1;").arg(t.surface_raised.name()));
  }
  // 描边按钮（发文件／截图／表情／颜色）同款：品牌色描边＋品牌色文字
  const QString outline = QStringLiteral(
      "QPushButton { background:%1; color:%2; border:1px solid %3; "
      "border-radius:8px; padding:6px 12px; }"
      "QPushButton:hover { background:%4; }")
      .arg(t.surface_raised.name(), t.brand_text.name(), t.brand.name(),
           t.brand_wash.name());
  for (QPushButton* btn :
       {file_btn_, folder_btn_, shot_btn_, emoji_btn_, color_btn_, nudge_btn_}) {
    if (btn) btn->setStyleSheet(outline);
  }
  if (send_btn_) {
    send_btn_->setStyleSheet(QStringLiteral(
        "QPushButton { background:%1; color:%2; border:none; "
        "border-radius:8px; padding:6px 18px; font-weight:600; }"
        "QPushButton:hover { background:%3; }"
        "QPushButton:disabled { background:%4; }")
        .arg(t.brand.name(), t.on_brand.name(), t.brand_hover.name(),
             t.disabled_bg.name()));
  }
  // rail 导航栏：令牌即状态（图标色随主题重画）；推广卡同刷
  if (rail_) rail_->apply_tokens(t);
  if (collab_promo_) {
    collab_promo_->setStyleSheet(
        QStringLiteral(
            "QWidget#collab_promo { background:%1; border:1px solid %2; "
            "border-radius:8px; }"
            "QLabel#collab_promo_title { font-weight:600; color:%3; }"
            "QLabel#collab_promo_sub { color:%4; font-size:11px; }"
            "QPushButton#collab_promo_btn { background:%5; color:%6; "
            "border:none; border-radius:6px; padding:3px 12px; "
            "font-weight:600; }"
            "QPushButton#collab_promo_btn:hover { background:%7; }")
            .arg(t.surface_raised.name(), t.border.name(), t.text.name(),
                 t.text_muted.name(), t.brand.name(), t.on_brand.name(),
                 t.brand_hover.name()));
  }
  if (banner_) {
    update_banner(); // 横幅底/文随形态与主题两变
  }
  if (at_hint_) {
    at_hint_->setStyleSheet(
        QStringLiteral("color:%1; font-size:11px;")
            .arg(t.text_muted.name()));
  }
  if (head_members_btn_) {
    // 原型 .icon-btn.brand：brand_wash 底＋brand_text 字（仅群会话可见）
    head_members_btn_->setStyleSheet(
        QStringLiteral("background:%1; color:%2; border:none; "
                       "border-radius:8px;")
            .arg(t.brand_wash.name(), t.brand_wash_text.name()));
  }
  if (group_panel_) {
    // 原型 .infopanel：panel 底＋左缘 1px 分隔线；内容（成员/文件/卡）随
    // 主题重建
    group_panel_->setStyleSheet(
        QStringLiteral("background:%1; border-left:1px solid %2;")
            .arg(t.surface.name(), t.surface_alt.name()));
    update_group_panel();
  }
  // —— 文件传输面（原型 desktop-files.html）：骨架样式在此刷；侧栏行与
  //     传输卡内嵌令牌色，随 rebuild_transfer_side/refresh_transfers_view
  //     全量重建同刷 ——
  if (transfer_side_) {
    transfer_side_->setStyleSheet(
        QStringLiteral("background:%1; border-right:1px solid %2;")
            .arg(t.surface_alt.name(), t.border.name()));
    rebuild_transfer_side();
  }
  if (transfer_page_) {
    transfer_page_->setStyleSheet(
        QStringLiteral("background:%1;"
                       "QLabel#tx_title { font-weight:600; font-size:15px; "
                       "color:%2; }"
                       "QLabel#tx_note { background:%3; border:1px solid %4; "
                       "border-radius:10px; padding:10px 14px; "
                       "font-size:11.5px; color:%5; }")
            .arg(t.surface.name(), t.text.name(), t.surface_alt.name(),
                 t.border.name(), t.text_muted.name()));
    refresh_transfers_view();
  }
  if (tx_send_btn_) {
    // 原型 .ch-actions .icon-btn.brand：品牌底＋品牌字
    tx_send_btn_->setStyleSheet(
        QStringLiteral("QPushButton { background:%1; color:%2; border:none; "
                       "border-radius:8px; padding:6px 12px; font-weight:700; "
                       "font-size:11.5px; }"
                       "QPushButton:hover { background:%3; }")
            .arg(t.brand.name(), t.on_brand.name(), t.brand_hover.name()));
  }
  // —— 组织架构面（原型 desktop-org.html）：骨架样式在此刷；树/网格/档案
  //     内嵌令牌色，随 rebuild_org_view 全量重建同刷 ——
  if (org_side_) {
    org_side_->setStyleSheet(
        QStringLiteral("background:%1; border-right:1px solid %2;")
            .arg(t.surface_alt.name(), t.border.name()));
    if (org_search_) {
      org_search_->setStyleSheet(
          QStringLiteral("QLineEdit { background:%1; border:1px solid %2; "
                         "border-radius:8px; padding:6px 10px; "
                         "font-size:12px; color:%3; }")
              .arg(t.surface_raised.name(), t.border.name(), t.text.name()));
    }
    if (sidebar_mode_ == QStringLiteral("org")) rebuild_org_view();
  }
  if (org_page_) {
    org_page_->setStyleSheet(
        QStringLiteral("background:%1;")
            .arg(t.surface.name()));
  }
  if (org_profile_) {
    org_profile_->setStyleSheet(
        QStringLiteral("background:%1; border-left:1px solid %2;")
            .arg(t.surface_alt.name(), t.border.name()));
  }
  {
    // 发起群聊 brand 钮（findChild 兜底：钮在骨架里建、无成员指针）
    if (auto* b = org_page_
                      ? org_page_->findChild<QPushButton*>(
                            QStringLiteral("org_group_btn"))
                      : nullptr) {
      b->setStyleSheet(
          QStringLiteral("QPushButton { background:%1; color:%2; border:none; "
                         "border-radius:8px; padding:7px 18px; "
                         "font-weight:700; font-size:12px; }"
                         "QPushButton:hover { background:%3; }")
              .arg(t.brand.name(), t.on_brand.name(), t.brand_hover.name()));
    }
  }
  // 聊天区富文本里的颜色也是内联的（气泡/@高亮/系统行）——按记录重渲一遍
  rerender_chat();
}

// 聊天区重渲：主题切换后富文本里的内联色不会自动跟随，故按记录重放。
// 只重渲当前会话已有的行（记录在 chat_rows_），不重新查库。
void MainWindow::rerender_chat() {
  if (!chat_view_) return;
  if (chat_showing_guidance_) {
    chat_view_->clear();
    render_guidance();
    return;
  }
  const bool at_bottom =
      chat_view_->verticalScrollBar()->value() >=
      chat_view_->verticalScrollBar()->maximum() - 4;
  chat_view_->clear();
  for (const ChatRow& row : chat_rows_) {
    if (row.system) {
      chat_view_->append(row.chip ? chip_line_html(row.text)
                                  : system_line_html(row.text));
    } else if (row.file_card) {
      chat_view_->append(day_anchor(row.ts_ms) +
                         file_bubble_html(row.name, row.text, row.file_meta,
                                          row.ts_ms, row.outgoing));
    } else {
      chat_view_->append(day_anchor(row.ts_ms) +
                         bubble_html(row.name, row.text, row.ts_ms,
                                     row.outgoing, row.at_mode, row.image,
                                     row.receipt, row.watch_key,
                                     watched_.contains(row.watch_key),
                                     row.owner_tag));
    }
  }
  if (at_bottom) {
    auto* bar = chat_view_->verticalScrollBar();
    bar->setValue(bar->maximum());
  }
}

void MainWindow::show_theme_settings() {
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("主题"));
  auto* layout = new QVBoxLayout(&dlg);
  auto* page = new ThemeSettingsPage(&ThemeManager::instance(), &dlg);
  layout->addWidget(page);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  layout->addWidget(buttons);
  dlg.exec();
}

void MainWindow::show_status(const QString& text) {
  status_hint_ = text;
  statusBar()->showMessage(
      QString::fromStdString(direct_engine_.status_text()) +
          QStringLiteral("　|　") +
          QString::fromStdString(collab_engine_.status_text()) +
          QStringLiteral("　|　") + status_hint_,
      0);
}

// —— 需求批⑪ 个性签名：设置对话框（设置菜单入口；登录门＝签名存服务端档案）——
void MainWindow::show_profile_dialog() {
  if (!collab_engine_.is_logged_in()) {
    show_status(QStringLiteral("个人资料需登录协作态（签名存服务端档案）"));
    return;
  }
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("个人资料 · 个性签名"));
  auto* layout = new QVBoxLayout(&dlg);
  auto* hint = new QLabel(
      QStringLiteral("签名在会话列表悬浮可见；留空保存＝清除。"), &dlg);
  layout->addWidget(hint);
  // 需求批⑫：我的头像（已设走缩放缓存，未设走默认头像组）＋更换入口
  {
    auto* avatar_row = new QHBoxLayout;
    auto* avatar_label = new QLabel(&dlg);
    avatar_label->setAlignment(Qt::AlignCenter);
    avatar_label->setPixmap(avatar_for(collab_engine_.account(),
                                       member_avatar_ver_.value(
                                           collab_engine_.account()), 64));
    avatar_row->addWidget(avatar_label);
    auto* btn_avatar = new QPushButton(QStringLiteral("更换头像…"), &dlg);
    avatar_row->addWidget(btn_avatar);
    avatar_row->addStretch(1);
    layout->addLayout(avatar_row);
    // 状态行红字明示（既有 request_failed 通道的对话框内显影；窗口销毁
    // 即断连，不影响主窗状态栏通道）
    auto* avatar_status = new QLabel(&dlg);
    avatar_status->setStyleSheet(
        QStringLiteral("color:%1;")
            .arg(ThemeManager::instance().tokens().danger.name()));
    avatar_status->setWordWrap(true);
    layout->addWidget(avatar_status);
    const QPointer<QLabel> status_guard(avatar_status);
    const QPointer<QLabel> pixmap_guard(avatar_label);
    connect(files_client(), &FilesClient::request_failed, &dlg,
            [status_guard](const QString& op, int status,
                           const QString& error) {
              if (!status_guard) return;
              status_guard->setText(
                  QStringLiteral("头像操作失败[%1]（%2）：%3")
                      .arg(op,
                           status > 0 ? QString::number(status)
                                      : QStringLiteral("网络"),
                           error));
            });
    connect(files_client(), &FilesClient::avatar_uploaded, &dlg,
            [this, status_guard, pixmap_guard](int, qint64) {
              if (!status_guard) return;
              // 收尾档完成时 ver 已入缓存（主窗先连），本机先见即刷
              if (avatar_upload_queue_.isEmpty() && avatar_new_ver_ > 0 &&
                  pixmap_guard) {
                pixmap_guard->setPixmap(
                    avatar_for(collab_engine_.account(), avatar_new_ver_,
                               64));
                status_guard->setText(QStringLiteral("头像已更新"));
              }
            });
    connect(btn_avatar, &QPushButton::clicked, &dlg,
            [this] { change_avatar(); });
  }
  // 需求批⑩：我的在线时长（滚动 24h/7d/30d 并集；org 数据到达即有值）
  const QString my_online = member_online_.value(collab_engine_.account());
  if (!my_online.isEmpty()) {
    layout->addWidget(new QLabel(my_online, &dlg));
  }
  auto* edit = new QLineEdit(&dlg);
  edit->setMaxLength(120); // 服务端同口径长度门
  edit->setText(member_signatures_.value(collab_engine_.account()));
  edit->setPlaceholderText(QStringLiteral("写一句个性签名（上限 120 字）"));
  layout->addWidget(edit);
  auto* buttons =
      new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                           &dlg);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, [this, &dlg, edit] {
    apply_signature(edit->text().trimmed());
    dlg.accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  layout->addWidget(buttons);
  dlg.exec();
}

// 发送签名设置（带参＝测试直调不弹框；回执经 profile_result 异步到）
void MainWindow::apply_signature(const QString& signature) {
  if (!collab_engine_.is_logged_in()) {
    show_status(QStringLiteral("设置签名需登录协作态"));
    return;
  }
  collab_engine_.set_signature(signature);
  show_status(QStringLiteral("签名设置请求已发送"));
}

QString MainWindow::own_signature() const {
  return member_signatures_.value(collab_engine_.account());
}

// —— 需求批⑫ 头像：文件面接线（懒建 FilesClient；登录复用协作口令，
//     端口走 QSettings files_port，缺省 24561 与各文件面窗口同款）——

FilesClient* MainWindow::files_client() {
  if (files_client_) return files_client_;
  files_client_ = new FilesClient(this);
  connect(files_client_, &FilesClient::logged_in, this, [this] {
    // 冲账：登录前挂起的头像下载
    for (const QString& a : std::as_const(avatar_pending_accounts_)) {
      queue_avatar_download(a, member_avatar_ver_.value(a));
    }
    avatar_pending_accounts_.clear();
    // 登录前点「更换头像…」挂起的选图续流程
    if (avatar_pick_pending_) {
      avatar_pick_pending_ = false;
      pick_and_upload_avatar();
    }
  });
  connect(files_client_, &FilesClient::avatar_fetched, this,
          [this](const QString& account, int size, const QByteArray& bytes) {
            on_avatar_fetched(account, size, bytes);
          });
  connect(files_client_, &FilesClient::avatar_uploaded, this,
          [this](int size, qint64 ver) { on_avatar_uploaded(size, ver); });
  connect(files_client_, &FilesClient::avatar_deleted, this, [this] {
    // 回落默认头像：本地缓存与在途对账全清，org 重拉带新 ver
    avatar_fetched_ver_.remove(collab_engine_.account());
    avatar_fetching_ver_.remove(collab_engine_.account());
    member_avatar_ver_.insert(collab_engine_.account(), 0);
    refresh_devices();
    collab_engine_.query_org();
    show_status(QStringLiteral("头像已清除（回落默认头像）"));
  });
  connect(files_client_, &FilesClient::request_failed, this,
          [this](const QString& op, int status, const QString& error) {
            if (op == QStringLiteral("avatar.download") && status == 404) {
              // 未设置头像（或刚删除）：记为已取免反复拉，展示走默认头像组
              for (auto it = avatar_fetching_ver_.constBegin();
                   it != avatar_fetching_ver_.constEnd(); ++it) {
                avatar_fetched_ver_.insert(it.key(), it.value());
              }
              avatar_fetching_ver_.clear();
              return;
            }
            if (op == QStringLiteral("avatar.upload")) {
              avatar_upload_queue_.clear();
              avatar_upload_pixmaps_.clear();
            }
            show_status(QStringLiteral("头像操作失败（%1 %2：%3）")
                            .arg(op,
                                 status > 0 ? QString::number(status)
                                            : QStringLiteral("网络"),
                                 error));
          });
  return files_client_;
}

bool MainWindow::files_ensure_login() {
  if (files_client_ && files_client_->is_logged_in()) return true;
  if (!collab_engine_.is_logged_in()) return false;
  const QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  const quint16 files_port = static_cast<quint16>(
      settings.value(QStringLiteral("files_port"), default_files_port())
          .toUInt());
  if (files_port == 0) return false;
  const QString host = collab_host_.isEmpty()
                           ? resolve_conn_host(
                                 settings.value(QStringLiteral("host"),
                                                default_collab_host())
                                     .toString())
                           : collab_host_;
  files_client()->login(host, files_port, collab_engine_.account(),
                        collab_engine_.password());
  return true; // 异步：logged_in 信号到后自动续（挂起下载/选图）
}

void MainWindow::change_avatar() {
  if (!collab_engine_.is_logged_in()) {
    show_status(QStringLiteral("更换头像需登录协作态"));
    return;
  }
  if (files_client_ == nullptr || !files_client_->is_logged_in()) {
    avatar_pick_pending_ = true;
    if (!files_ensure_login()) {
      avatar_pick_pending_ = false;
      show_status(
          QStringLiteral("文件面未连接（files_port 未配置或协作未登录）"));
    }
    return; // logged_in 到后自动续选图
  }
  pick_and_upload_avatar();
}

void MainWindow::pick_and_upload_avatar() {
  const QString path = QFileDialog::getOpenFileName(
      this, QStringLiteral("选择头像图片"), QString(),
      QStringLiteral("图片 (*.png *.jpg *.jpeg *.gif)"));
  if (path.isEmpty()) return;
  // 发网前本地门：QPixmap 打不开即拒不发网（服务端只兜 magic/上限）
  QPixmap pm(path);
  if (pm.isNull()) {
    show_status(QStringLiteral("图片无法读取（未发网）"));
    return;
  }
  CropDialog dlg(pm.toImage(), this);
  if (dlg.exec() != QDialog::Accepted) return;
  const QImage square = dlg.cropped();
  if (square.isNull()) {
    show_status(QStringLiteral("裁剪结果无效（未发网）"));
    return;
  }
  // 逐档平滑缩放并编码 PNG（256→32；上传串行走队列，失败即停）
  avatar_upload_queue_.clear();
  avatar_upload_pixmaps_.clear();
  for (const int size : {256, 128, 64, 32}) {
    const QPixmap scaled = QPixmap::fromImage(
        square.scaled(size, size, Qt::IgnoreAspectRatio,
                      Qt::SmoothTransformation));
    avatar_upload_pixmaps_.push_back(scaled);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    scaled.save(&buf, "PNG");
    avatar_upload_queue_.push_back({size, bytes});
  }
  avatar_uploaded_count_ = 0;
  avatar_new_ver_ = 0;
  show_status(QStringLiteral("头像上传中（1/4）"));
  start_next_avatar_upload();
}

void MainWindow::start_next_avatar_upload() {
  if (avatar_upload_queue_.isEmpty()) {
    if (avatar_new_ver_ <= 0) return; // 全程无成功回包（理论不达；防呆）
    // 本机先见：四档按服务端最新版本戳落缓存，不等 org 重拉
    const QString me = collab_engine_.account();
    for (int i = 0; i < avatar_upload_pixmaps_.size(); ++i) {
      store_avatar_cache(me, avatar_new_ver_,
                         avatar_upload_pixmaps_[i].width(),
                         avatar_upload_pixmaps_[i]);
    }
    avatar_fetched_ver_.insert(me, avatar_new_ver_);
    member_avatar_ver_.insert(me, avatar_new_ver_);
    avatar_upload_pixmaps_.clear();
    refresh_devices();
    collab_engine_.query_org(); // 成功即重拉本人资料（org 随后到）
    show_status(QStringLiteral("头像已更新"));
    return;
  }
  const auto [size, bytes] = avatar_upload_queue_.front();
  files_client_->avatar_upload(size, bytes);
}

void MainWindow::on_avatar_uploaded(int size, qint64 ver) {
  Q_UNUSED(size);
  if (ver > avatar_new_ver_) avatar_new_ver_ = ver; // 服务端 MAX(ts_ms) 演进
  avatar_upload_queue_.pop_front();
  ++avatar_uploaded_count_;
  if (!avatar_upload_queue_.isEmpty()) {
    show_status(QStringLiteral("头像上传中（%1/4）")
                    .arg(avatar_uploaded_count_ + 1));
  }
  start_next_avatar_upload(); // 队列尽＝收尾（落本机缓存＋重拉 org）
}

void MainWindow::queue_avatar_download(const QString& account, qint64 ver) {
  if (ver <= 0) return;
  if (avatar_fetched_ver_.value(account) == ver) return; // 已缓存该版
  if (files_client_ == nullptr || !files_client_->is_logged_in()) {
    avatar_pending_accounts_.insert(account);
    return;
  }
  avatar_fetching_ver_.insert(account, ver);
  files_client_->avatar_download(account, 256); // 只取 256 档，本地派生四档
}

void MainWindow::on_avatar_fetched(const QString& account, int size,
                                   const QByteArray& bytes) {
  const qint64 ver = avatar_fetching_ver_.take(account);
  if (ver <= 0 || size != 256) return;
  QPixmap pm;
  if (!pm.loadFromData(bytes) || pm.isNull()) {
    avatar_fetched_ver_.insert(account, ver); // 坏包不再拉同版
    return;
  }
  // 256 原档＋平滑缩放三档一并落缓存（tooltip/组织树/资料框同源取用）
  for (const int s : {256, 128, 64, 32}) {
    store_avatar_cache(account, ver, s, s == 256
                                            ? pm
                                            : pm.scaled(s, s,
                                                        Qt::IgnoreAspectRatio,
                                                        Qt::SmoothTransformation));
  }
  avatar_fetched_ver_.insert(account, ver);
  refresh_devices(); // 悬浮头像即时刷新
}

qint64 MainWindow::own_avatar_ver() const {
  return member_avatar_ver_.value(collab_engine_.account());
}

// 需求批⑩：本人在线时长展示串（org 数据缓存；tooltip/资料对话框同源）
QString MainWindow::own_online() const {
  return member_online_.value(collab_engine_.account());
}

} // namespace memex::client
