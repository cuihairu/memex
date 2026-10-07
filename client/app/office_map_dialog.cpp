// 二期·办公室位置图窗口（实现）。判权全在服务端（编辑归 org-admin，
// can_manage 服务端现裁回带）——客户端画布只提交落位与展示；拖拽松手
// 即保存（高频不高敏，改位不刷服务端日志）。
#include "office_map_dialog.hpp"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/collab/files_client.hpp"

namespace memex::client {

namespace {

struct Seat {
  qint64 id{0};
  QString floor;
  QString label;
  QString account;
  double x{0};
  double y{0};
};

} // namespace

// 抽象平面画布：网格底＋工位圆点（工位号/占用者标签）。搜索过滤=命中
// 亮、其余淡；org-admin 可拖拽落位（松手经 seat_moved 上抛保存）。
// 置名空间作用域（非匿名）——头文件有前置声明与成员指针指到本类。
class OfficeCanvas : public QWidget {
  Q_OBJECT
 public:
  explicit OfficeCanvas(QWidget* parent = nullptr) : QWidget(parent) {
    setMinimumSize(360, 300);
    setMouseTracking(false);
  }
  void set_seats(std::vector<Seat> seats) {
    seats_ = std::move(seats);
    selected_ = -1;
    dragging_ = -1;
    update();
  }
  void set_can_manage(bool on) { can_manage_ = on; }
  void set_filter(const QString& q) {
    filter_ = q.trimmed();
    update();
  }
  qint64 selected_id() const {
    return selected_ >= 0 && selected_ < static_cast<int>(seats_.size())
               ? seats_[static_cast<size_t>(selected_)].id
               : -1;
  }
  // 程序化移位（拖拽模拟）：选中点改坐标并上抛（与鼠标松手同源）
  bool move_selected(double nx, double ny) {
    if (!can_manage_ || selected_ < 0 ||
        selected_ >= static_cast<int>(seats_.size())) {
      return false;
    }
    auto& s = seats_[static_cast<size_t>(selected_)];
    s.x = std::max(0.0, std::min(1.0, nx));
    s.y = std::max(0.0, std::min(1.0, ny));
    update();
    emit seat_moved(s.id, s.floor, s.label, s.x, s.y);
    return true;
  }
  int seat_count() const { return static_cast<int>(seats_.size()); }
  // 当前过滤下的可见工位数（与 paintEvent 同一命中谓词）
  int visible_count() const {
    int n = 0;
    for (const auto& s : seats_) {
      if (matches(s)) ++n;
    }
    return n;
  }
  // 程序化点选（与 mousePressEvent 同源 hit-test；不进入拖拽态）
  void select_at(double nx, double ny) {
    const QPointF pos(nx * width(), ny * height());
    int hit = -1;
    double best = 18.0;
    for (int i = 0; i < static_cast<int>(seats_.size()); ++i) {
      const auto& s = seats_[static_cast<size_t>(i)];
      const QPointF c(s.x * width(), s.y * height());
      const double d = std::hypot(c.x() - pos.x(), c.y() - pos.y());
      if (d < best) {
        best = d;
        hit = i;
      }
    }
    selected_ = hit;
    update();
  }

 signals:
  // floor/label 随点带上——对话框不必回查工位表即可发网保存
  void seat_moved(qint64 id, const QString& floor, const QString& label,
                  double x, double y);

 protected:
  bool matches(const Seat& s) const {
    return filter_.isEmpty() ||
           s.account.contains(filter_, Qt::CaseInsensitive) ||
           s.label.contains(filter_, Qt::CaseInsensitive);
  }

  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.fillRect(rect(), QColor(0xF5, 0xF5, 0xF0));
    // 网格底（抽象平面；十等分参考线）
    p.setPen(QPen(QColor(0xDD, 0xDD, 0xD5), 1));
    for (int i = 1; i < 10; ++i) {
      const int gx = width() * i / 10;
      const int gy = height() * i / 10;
      p.drawLine(gx, 0, gx, height());
      p.drawLine(0, gy, width(), gy);
    }
    for (int i = 0; i < static_cast<int>(seats_.size()); ++i) {
      const auto& s = seats_[static_cast<size_t>(i)];
      const QPointF c(s.x * width(), s.y * height());
      const bool hit = matches(s);
      // 命中亮（橙）/其余淡（灰）；空位白底描边
      QColor fill = hit ? QColor(0xE8, 0x8A, 0x2A) : QColor(0xBB, 0xBB, 0xBB);
      if (s.account.isEmpty()) {
        fill = hit ? QColor(0xFF, 0xFF, 0xFF) : QColor(0xE8, 0xE8, 0xE8);
      }
      if (i == selected_) fill = QColor(0x2A, 0x82, 0xE8); // 选中蓝
      p.setPen(QPen(QColor(0x66, 0x66, 0x66), 1));
      p.setBrush(fill);
      p.drawEllipse(c, 10, 10);
      p.setPen(QPen(hit || i == selected_ ? QColor(0x33, 0x33, 0x33)
                                          : QColor(0x99, 0x99, 0x99)));
      const QString text = s.account.isEmpty()
                               ? s.label
                               : s.label + QStringLiteral("·") + s.account;
      p.drawText(QRectF(c.x() - 70, c.y() + 12, 140, 32), Qt::AlignHCenter,
                 text);
    }
  }

  void mousePressEvent(QMouseEvent* e) override {
    const QPointF pos = e->position();
    // 与 select_at 同源 hit-test（命中半径 18px，最近者胜）
    select_at(pos.x() / width(), pos.y() / height());
    dragging_ = can_manage_ ? selected_ : -1;
  }

  void mouseMoveEvent(QMouseEvent* e) override {
    if (dragging_ < 0 || dragging_ >= static_cast<int>(seats_.size())) return;
    auto& s = seats_[static_cast<size_t>(dragging_)];
    s.x = std::max(0.0, std::min(1.0, e->position().x() / width()));
    s.y = std::max(0.0, std::min(1.0, e->position().y() / height()));
    update();
  }

  void mouseReleaseEvent(QMouseEvent*) override {
    if (dragging_ >= 0 && dragging_ < static_cast<int>(seats_.size())) {
      const auto& s = seats_[static_cast<size_t>(dragging_)];
      emit seat_moved(s.id, s.floor, s.label, s.x, s.y); // 松手即保存
    }
    dragging_ = -1;
  }

 private:
  std::vector<Seat> seats_;
  QString filter_;
  int selected_{-1};
  int dragging_{-1};
  bool can_manage_{false};
};

OfficeMapDialog::OfficeMapDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("办公室位置图"));
  resize(620, 600);
  client_ = new FilesClient(this);
  build_ui();

  connect(client_, &FilesClient::logged_in, this, [this] {
    set_status(QStringLiteral("已连接（") + client_->account() +
               QStringLiteral("）"));
    refresh();
  });
  connect(client_, &FilesClient::login_failed, this,
          [this](const QString& r) {
            set_status(QStringLiteral("连接失败：") + r, true);
            btn_connect_->setEnabled(true);
          });
  connect(client_, &FilesClient::office_map_fetched, this,
          &OfficeMapDialog::apply_map);
  // 增删绑任一回执→重拉平面（单面真源，不自拼本地态）
  connect(client_, &FilesClient::office_seat_saved, this, [this](qint64) {
    set_status(QStringLiteral("工位已保存"));
    refresh();
  });
  connect(client_, &FilesClient::office_seat_deleted, this, [this] {
    set_status(QStringLiteral("工位已删除"));
    refresh();
  });
  connect(client_, &FilesClient::office_seat_bound, this, [this] {
    set_status(QStringLiteral("占用已更新"));
    refresh();
  });
  connect(client_, &FilesClient::request_failed, this,
          [this](const QString& op, int status, const QString& error) {
            set_status(QStringLiteral("操作失败（%1：%2 %3）")
                           .arg(op, QString::number(status), error),
                       true);
          });
}

void OfficeMapDialog::build_ui() {
  auto* layout = new QVBoxLayout(this);

  // 连接区（与文件助手同构：地址/端口/账号/口令）
  auto* conn = new QHBoxLayout;
  auto* host = new QLineEdit(this);
  host->setPlaceholderText(QStringLiteral("服务器地址"));
  auto* port = new QLineEdit(this);
  port->setPlaceholderText(QStringLiteral("文件端口"));
  port->setMaximumWidth(90);
  auto* account_box = new QLineEdit(this);
  account_box->setPlaceholderText(QStringLiteral("账号"));
  account_box->setMaximumWidth(110);
  auto* password = new QLineEdit(this);
  password->setPlaceholderText(QStringLiteral("口令"));
  password->setEchoMode(QLineEdit::Password);
  btn_connect_ = new QPushButton(QStringLiteral("连接"), this);
  conn->addWidget(host);
  conn->addWidget(port);
  conn->addWidget(account_box);
  conn->addWidget(password);
  conn->addWidget(btn_connect_);
  layout->addLayout(conn);
  connect(btn_connect_, &QPushButton::clicked, this,
          [this, host, port, account_box, password] {
            connect_to(host->text(), port->text().toUShort(),
                       account_box->text(), password->text());
          });

  // 平面标题（楼层号由服务端回带）
  floor_label_ = new QLabel(QStringLiteral(" "), this);
  layout->addWidget(floor_label_);

  // 画布（拖拽落位=org-admin；松手上抛保存）
  canvas_ = new OfficeCanvas(this);
  layout->addWidget(canvas_, 1);

  // 搜索定位（账号/工位号过滤高亮）
  auto* srow = new QHBoxLayout;
  srow->addWidget(new QLabel(QStringLiteral("搜索"), this));
  search_ = new QLineEdit(this);
  search_->setPlaceholderText(QStringLiteral("账号或工位号（命中高亮）"));
  srow->addWidget(search_, 1);
  layout->addLayout(srow);
  connect(search_, &QLineEdit::textChanged, this, [this](const QString& t) {
    set_search(t);
  });

  // 编辑区（can_manage 时才可用；客户端灰是门面，判权在服务端）
  auto* erow = new QHBoxLayout;
  new_floor_ = new QLineEdit(this);
  new_floor_->setPlaceholderText(QStringLiteral("楼层"));
  new_floor_->setMaximumWidth(70);
  new_label_ = new QLineEdit(this);
  new_label_->setPlaceholderText(QStringLiteral("工位号"));
  new_label_->setMaximumWidth(90);
  btn_add_ = new QPushButton(QStringLiteral("新增工位"), this);
  btn_delete_ = new QPushButton(QStringLiteral("删除选中"), this);
  bind_account_ = new QLineEdit(this);
  bind_account_->setPlaceholderText(QStringLiteral("占用者账号"));
  btn_bind_ = new QPushButton(QStringLiteral("绑定"), this);
  btn_unbind_ = new QPushButton(QStringLiteral("解绑"), this);
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), this);
  erow->addWidget(new_floor_);
  erow->addWidget(new_label_);
  erow->addWidget(btn_add_);
  erow->addWidget(btn_delete_);
  erow->addWidget(bind_account_, 1);
  erow->addWidget(btn_bind_);
  erow->addWidget(btn_unbind_);
  erow->addWidget(btn_refresh_);
  layout->addLayout(erow);

  status_ = new QLabel(this);
  layout->addWidget(status_);

  connect(btn_add_, &QPushButton::clicked, this, [this] {
    add_seat(new_floor_->text().trimmed(), new_label_->text().trimmed());
  });
  connect(btn_delete_, &QPushButton::clicked, this,
          [this] { delete_selected(); });
  connect(btn_bind_, &QPushButton::clicked, this, [this] {
    bind_selected(bind_account_->text().trimmed());
  });
  connect(btn_unbind_, &QPushButton::clicked, this,
          [this] { bind_selected(QString()); });
  connect(btn_refresh_, &QPushButton::clicked, this, [this] { refresh(); });
  // 松手即保存（画布上抛；can_manage 才真发网）
  connect(canvas_, &OfficeCanvas::seat_moved, this,
          [this](qint64, const QString& floor, const QString& label, double x,
                 double y) {
            if (!can_manage_) return;
            client_->save_office_seat(floor, label, x, y);
          });
}

void OfficeMapDialog::connect_to(const QString& host, quint16 files_port,
                                 const QString& acc, const QString& pass) {
  if (host.isEmpty() || acc.isEmpty()) {
    set_status(QStringLiteral("服务器地址与账号不能为空"), true);
    return;
  }
  if (files_port == 0) {
    set_status(QStringLiteral("文件面端口非法"), true);
    return;
  }
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  settings.setValue(QStringLiteral("files_port"), QString::number(files_port));
  btn_connect_->setEnabled(false);
  client_->login(host, files_port, acc, pass);
}

bool OfficeMapDialog::is_connected() const { return client_->is_logged_in(); }

void OfficeMapDialog::refresh(const QString& floor) {
  if (!is_connected()) return;
  if (!floor.isEmpty()) view_floor_ = floor; // org-admin 切层（服务端门）
  client_->fetch_office_map(view_floor_);
}

bool OfficeMapDialog::add_seat(const QString& floor, const QString& label) {
  if (floor.isEmpty() || label.isEmpty()) {
    set_status(QStringLiteral("楼层与工位号不能为空"), true);
    return false;
  }
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  // 新增落画布中心（归一化 0.5,0.5；拖拽再调）。视图随编辑走：
  // 管理者加哪层就停在哪层（回执后的 refresh 落在该层平面）
  view_floor_ = floor;
  client_->save_office_seat(floor, label, 0.5, 0.5);
  return true;
}

bool OfficeMapDialog::delete_selected() {
  const qint64 id = canvas_->selected_id();
  if (id < 0) {
    set_status(QStringLiteral("先选中一个工位（点它）"), true);
    return false;
  }
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  client_->delete_office_seat(id);
  return true;
}

bool OfficeMapDialog::bind_selected(const QString& account) {
  const qint64 id = canvas_->selected_id();
  if (id < 0) {
    set_status(QStringLiteral("先选中一个工位（点它）"), true);
    return false;
  }
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  client_->bind_office_seat(id, account);
  return true;
}

bool OfficeMapDialog::move_selected(double x, double y) {
  return canvas_->move_selected(x, y);
}

bool OfficeMapDialog::select_at(double x, double y) {
  canvas_->select_at(x, y);
  return canvas_->selected_id() >= 0;
}

void OfficeMapDialog::set_search(const QString& q) {
  canvas_->set_filter(q);
}

void OfficeMapDialog::apply_map(const QJsonObject& map) {
  can_manage_ = map.value(QStringLiteral("can_manage")).toBool();
  canvas_->set_can_manage(can_manage_);
  std::vector<Seat> seats;
  for (const auto& v : map.value(QStringLiteral("seats")).toArray()) {
    const auto s = v.toObject();
    Seat t;
    t.id = static_cast<qint64>(s.value(QStringLiteral("id")).toDouble());
    t.floor = s.value(QStringLiteral("floor")).toString();
    t.label = s.value(QStringLiteral("label")).toString();
    t.account = s.value(QStringLiteral("account")).toString();
    t.x = s.value(QStringLiteral("x")).toDouble();
    t.y = s.value(QStringLiteral("y")).toDouble();
    seats.push_back(std::move(t));
  }
  const QString floor = map.value(QStringLiteral("floor")).toString();
  floor_label_->setText(floor.isEmpty()
                            ? QStringLiteral("（未分配工位——无楼层可看）")
                            : QStringLiteral("楼层 %1（%2 个工位）%3")
                                  .arg(floor)
                                  .arg(seats.size())
                                  .arg(can_manage_
                                           ? QStringLiteral("  · 编辑中（拖"
                                                            "拽落位）")
                                           : QString()));
  canvas_->set_seats(std::move(seats));
}

void OfficeMapDialog::set_status(const QString& text, bool error) {
  status_->setText(error ? QStringLiteral("⚠ %1").arg(text) : text);
  // 错误红色：QSS 不好控主题，用调色板直改（恢复用空参刷新路径重设）
  QPalette p = status_->palette();
  p.setColor(QPalette::WindowText,
             error ? QColor(Qt::red)
                   : canvas_->palette().color(QPalette::WindowText));
  status_->setPalette(p);
}

QString OfficeMapDialog::status_text() const { return status_->text(); }

int OfficeMapDialog::seat_count() const { return canvas_->seat_count(); }

int OfficeMapDialog::visible_count() const {
  return canvas_->visible_count();
}

qint64 OfficeMapDialog::selected_id() const { return canvas_->selected_id(); }

QString OfficeMapDialog::floor_text() const { return floor_label_->text(); }

bool OfficeMapDialog::can_manage() const { return can_manage_; }

} // namespace memex::client

// Q_OBJECT 在本 .cpp 内（OfficeCanvas 定义于此），AUTOMOC 需要本文件含
// 自身 moc 产物
#include "office_map_dialog.moc"
