// 聊天记录模糊搜索窗（本机库直查，不触网）。
#include "message_search_dialog.hpp"

#include "core/local_store.hpp"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

namespace memex::client {

namespace {
constexpr int kRolePeer = Qt::UserRole + 1;   // 对端键
constexpr int kRoleMsgId = Qt::UserRole + 2;  // 消息标识（空＝直连态）
constexpr int kRoleText = Qt::UserRole + 3;   // 命中正文
// 单页命中上限（相关度倒序，超出留下一页语义的余量）
constexpr int kPageSize = 200;

QString fmt_time(qint64 ms) {
  return ms > 0
             ? QDateTime::fromMSecsSinceEpoch(ms).toString(
                   QStringLiteral("yyyy-MM-dd HH:mm"))
             : QString();
}
}  // namespace

MessageSearchDialog::MessageSearchDialog(QWidget* parent)
    : QDialog(parent) {
  build_ui();
  connect(search_, &QLineEdit::textChanged, this, [this](const QString& t) {
    // 空关键词＝不搜（不列全表，避免误把会话全量刷进列表）
    if (t.trimmed().isEmpty()) {
      list_->clear();
      set_status(
          QStringLiteral("输入关键词搜索聊天记录：空格或逗号分隔多个词，"
                         "多个词全部命中才出（英文大小写不敏感）"));
      return;
    }
    run_search();
  });
  connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
    open_selected();
  });
}

void MessageSearchDialog::build_ui() {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(12, 12, 12, 12);
  root->setSpacing(8);

  search_ = new QLineEdit(this);
  search_->setPlaceholderText(
      QStringLiteral("搜索聊天记录：只记一半、词在中间、多个词用空格分开"));
  search_->setClearButtonEnabled(true);
  root->addWidget(search_);

  list_ = new QListWidget(this);
  list_->setWordWrap(true);
  root->addWidget(list_, 1);

  status_ = new QLabel(this);
  status_->setWordWrap(true);
  root->addWidget(status_);

  setMinimumSize(560, 420);
  set_status(
      QStringLiteral("输入关键词搜索聊天记录：空格或逗号分隔多个词，"
                     "多个词全部命中才出（英文大小写不敏感）"));
}

void MessageSearchDialog::bind_store(LocalStore* store) {
  store_ = store;
  if (store_ == nullptr) {
    list_->clear();
    set_status(QStringLiteral("本机聊天记录库不可用"), true);
    search_->setEnabled(false);
    return;
  }
  search_->setEnabled(true);
}

void MessageSearchDialog::run_search() {
  const QString kw = search_->text().trimmed();
  if (kw.isEmpty()) {
    list_->clear();
    set_status(QStringLiteral("输入关键词开始搜索"));
    return;
  }
  if (store_ == nullptr || !store_->is_open()) {
    list_->clear();
    set_status(QStringLiteral("本机聊天记录库不可用"), true);
    return;
  }
  const auto rows = store_->search_messages(kw, kPageSize);
  list_->clear();
  for (const StoredMessage& m : rows) {
    const QString peer = QString::fromStdString(m.peer);
    const QString from = QString::fromStdString(m.from);
    const QString text = QString::fromStdString(m.text);
    auto* it = new QListWidgetItem(list_);
    it->setText(QStringLiteral("%1 · %2\n%3")
                    .arg(fmt_time(m.ts_ms), peer + QStringLiteral(" ← ") + from,
                         text));
    it->setData(kRolePeer, peer);
    it->setData(kRoleMsgId, QString::fromStdString(m.msg_id));
    it->setData(kRoleText, text);
  }
  set_status(QStringLiteral("命中 %1 条（相关度优先；双击打开对应会话）")
                 .arg(rows.size()));
}

void MessageSearchDialog::open_selected() {
  QListWidgetItem* it = list_->currentItem();
  if (it == nullptr) {
    if (list_->count() > 0) list_->setCurrentRow(0);
    it = list_->currentItem();
  }
  if (it == nullptr) return;
  const QString peer = it->data(kRolePeer).toString();
  if (peer.isEmpty()) return;
  emit open_message(peer, it->data(kRoleMsgId).toString());
}

int MessageSearchDialog::result_count() const {
  return list_ == nullptr ? 0 : list_->count();
}

QString MessageSearchDialog::status_text() const {
  return status_ == nullptr ? QString() : status_->text();
}

QString MessageSearchDialog::selected_peer() const {
  const QListWidgetItem* it = list_->currentItem();
  return it == nullptr ? QString() : it->data(kRolePeer).toString();
}

QString MessageSearchDialog::selected_msg_id() const {
  const QListWidgetItem* it = list_->currentItem();
  return it == nullptr ? QString() : it->data(kRoleMsgId).toString();
}

void MessageSearchDialog::set_status(const QString& text, bool error) {
  if (status_ == nullptr) return;
  status_->setText(text);
  status_->setStyleSheet(
      error ? QStringLiteral("color: #d4380d;") : QStringLiteral(""));
}

}  // namespace memex::client
