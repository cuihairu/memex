// 发送路由（LAY-002 拆分）：send_in_current_chat 分形态路由、跨态
// 互通、直连守卫与文件/文件夹/截图/振屏各发送缝。

#include "main_window.hpp"
#include "main_window/common.hpp"


#include <QTimer>
#include <QShortcut>
#include <QStandardPaths>
#include "notify_prefs.hpp"
namespace memex::client {

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

} // namespace memex::client
