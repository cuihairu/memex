// 头像与文件面（LAY-002 拆分）：FilesClient 懒建接线、头像四档上传
// 与下载缓存、本人在线时长/版本戳。

#include "main_window.hpp"
#include "main_window/common.hpp"


#include "collab_defaults.hpp"

#include <QFileDialog>
#include "file_assistant.hpp"
#include "crop_dialog.hpp"
#include "group_memo_dialog.hpp"
#include "group_ci_dialog.hpp"
#include "group_pack_dialog.hpp"
#include "group_server_dialog.hpp"
#include "task_dialog.hpp"
#include "approval_dialog.hpp"
#include "report_dialog.hpp"
#include "group_tools_dialog.hpp"
#include "brand_settings_page.hpp"
#include "assist_dialog.hpp"
#include "audit_dialog.hpp"
#include "archive_console.hpp"
#include "office_map_dialog.hpp"
#include <QBuffer>
namespace memex::client {

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

// —— 需求批⑫ 头像：文件面接线（懒建 FilesClient；登录复用协作口令，
//     端口走 QSettings files_port，缺省 24561 与各文件面窗口同款）——

FilesClient* MainWindow::files_client() {
  if (files_client_) return files_client_;
  files_client_ = new FilesClient(this);
  if (mailbox_page_) mailbox_page_->set_client(files_client_); // 邮箱面同源
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
    // 邮箱未读徽章首拉（服务端计数为唯一来源）
    refresh_mailbox_badge();
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
