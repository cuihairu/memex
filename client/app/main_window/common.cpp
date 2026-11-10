// main_window 共享小工具定义（LAY-002 拆分；声明见 main_window/common.hpp）。
#include "main_window/common.hpp"


#include <QFontMetrics>
#include <QIcon>
#include <QPainter>
#include <QRegularExpression>
#include <QUrl>
#include <utility>

#include "theme.hpp"
#include "theme_settings_page.hpp"
namespace memex::client {

// 当前令牌（界面颜色唯一来源，R19 · T4.9）——不再散落十六进制字面量

// 新消息闪烁的合并窗：窗内后续消息不叠加（同会话/跨会话短时间多条
// 合并成一次闪烁）；窗口激活时 Qt 自行取消当前闪烁，窗状态另由触发点
// 的 isActiveWindow 判定管

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

// —— 原型协作列表行（desktop-collab.html .row）控件化 ——
// 头像（40px 圆角 12、八色同源）＋在离线点（.presence 11px 描边侧栏底）
// ＋主列（行首＝名＋右端时间；末条预览）＋右徽标（.n-count 未读数品牌
// 胶囊）。presence：1 在线／0 离线／-1 无（群行）。WA_TransparentForMouse
// Events 让点击/悬停穿透回列表——选中、悬浮与条目悬浮提示仍走
// QListWidget 原生路径（UserRole 数据、itemClicked 不受行控件影响）。
QWidget* make_session_row(const ThemeTokens& t, const QString& avatar_name,
                          int presence, const QString& name,
                          const QString& time, const QString& preview,
                          int unread, const QString& badge) {
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
                    bool outgoing, bool at_mode,
                    const QString& image,
                    const QString& receipt,
                    const QString& watch_key,
                    bool watched,
                    bool owner) {
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

} // namespace memex::client
