#include "screenshot_tool.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QDateTime>
#include <QDir>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScreen>
#include <QVBoxLayout>

#include <cmath>

namespace memex::client {

// —— 纯函数：选区（虚拟屏设备像素）→ 单屏逻辑像素 ——

QRect region_to_local(const QRect& device_rect, const QPoint& logical_top_left,
                      const QSize& logical_size, double dpr) {
  if (dpr <= 0) return QRect();
  const QRectF screen_device(logical_top_left * dpr, logical_size * dpr);
  const QRect inter = device_rect.intersected(screen_device.toRect());
  if (inter.isEmpty()) return QRect();
  return QRect(
      QPoint(qRound((inter.x() - screen_device.x()) / dpr),
             qRound((inter.y() - screen_device.y()) / dpr)),
      QSize(qRound(inter.width() / dpr), qRound(inter.height() / dpr)));
}

// —— 标注画布 ——

AnnotationCanvas::AnnotationCanvas(QObject* parent) : QObject(parent) {}

void AnnotationCanvas::setImage(const QPixmap& img) {
  if (img.isNull()) return;
  original_ = img;
  commands_.clear();
}

void AnnotationCanvas::addCommand(const AnnotationCommand& cmd) {
  if (!hasImage()) return;
  commands_.append(cmd);
}

bool AnnotationCanvas::undo() {
  if (commands_.isEmpty()) return false;
  commands_.removeLast();
  return true;
}

void AnnotationCanvas::clear() { commands_.clear(); }

void AnnotationCanvas::applyMosaic(QPixmap& base, const AnnotationCommand& cmd) {
  QRect r = cmd.rect.normalized();
  if (r.width() < 4 || r.height() < 4) return;
  r = r.intersected(QRect(QPoint(0, 0), base.size()));
  if (r.isEmpty()) return;
  // 块大小随区域自适应：缩小到 1/block 再最近邻放大＝像素化
  const int block = qMax(8, qMin(r.width(), r.height()) / 20);
  QPixmap small =
      base.copy(r)
          .scaled(qMax(1, r.width() / block), qMax(1, r.height() / block),
                  Qt::IgnoreAspectRatio, Qt::FastTransformation);
  QPixmap back = small.scaled(r.size(), Qt::IgnoreAspectRatio,
                              Qt::FastTransformation);
  QPainter p(&base);
  p.drawPixmap(r.topLeft(), back);
}

QPixmap AnnotationCanvas::render() const {
  if (!hasImage()) return QPixmap();
  QPixmap base = original_.copy();
  QPixmap overlay(base.size());
  overlay.fill(Qt::transparent);
  {
    QPainter p(&overlay);
    for (const AnnotationCommand& cmd : commands_) {
      switch (cmd.type) {
      case AnnotationCommand::Type::kMosaic:
        applyMosaic(base, cmd);
        break;
      case AnnotationCommand::Type::kPen: {
        if (cmd.points.size() < 2) break;
        p.setPen(QPen(cmd.color, cmd.width, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.drawPolyline(cmd.points.constData(),
                       static_cast<int>(cmd.points.size()));
        break;
      }
      case AnnotationCommand::Type::kArrow: {
        if (cmd.points.size() < 2) break;
        const QPoint a = cmd.points.value(0);
        const QPoint b = cmd.points.value(1);
        p.setPen(QPen(cmd.color, cmd.width, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.drawLine(a, b);
        const double ang = std::atan2(b.y() - a.y(), b.x() - a.x());
        const double hl = cmd.width * 4.0 + 10.0;
        for (const double da : {M_PI / 6, -M_PI / 6}) {
          p.drawLine(b, QPoint(qRound(b.x() - hl * std::cos(ang + da)),
                               qRound(b.y() - hl * std::sin(ang + da))));
        }
        break;
      }
      case AnnotationCommand::Type::kText: {
        QFont f;
        f.setPixelSize(qMax(16, original_.height() / 25));
        f.setBold(true);
        p.setFont(f);
        p.setPen(cmd.color);
        p.drawText(cmd.text_pos.x(), cmd.text_pos.y() + f.pixelSize(),
                   cmd.text);
        break;
      }
      }
    }
  }
  QPixmap out(base.size());
  out.fill(Qt::white);
  QPainter p(&out);
  p.drawPixmap(0, 0, base);
  p.drawPixmap(0, 0, overlay);
  return out;
}

bool AnnotationCanvas::save(const QString& path) const {
  return render().save(path, "PNG");
}

// —— 区域选择覆盖层 ——

RegionSelectOverlay::RegionSelectOverlay(QScreen* screen, SelectionState* state,
                                         QWidget* parent)
    : QWidget(parent), screen_(screen), state_(state) {
  Q_ASSERT(screen);
  setAttribute(Qt::WA_TranslucentBackground);
  setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                 Qt::Tool);
  setGeometry(screen->geometry());
  setCursor(Qt::CrossCursor);
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
}

void RegionSelectOverlay::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), QColor(20, 20, 20, 110));
  if (state_->selecting) {
    const QRect local =
        region_to_local(state_->current, screen_->geometry().topLeft(),
                        screen_->geometry().size(),
                        screen_->devicePixelRatio());
    if (!local.isEmpty()) {
      // 选区「挖空」：Clear 合成擦出透明，桌面透出（经典选区效果）
      p.setCompositionMode(QPainter::CompositionMode_Clear);
      p.fillRect(local, Qt::transparent);
      p.setCompositionMode(QPainter::CompositionMode_SourceOver);
      p.setPen(QPen(QColor(QStringLiteral("#e16531")), 2));
      p.drawRect(local.adjusted(0, 0, -1, -1));
      // 尺寸标签：优先在选区上方，空间不足放内部顶端
      const QString label = QStringLiteral("%1 × %2")
                                .arg(state_->current.width())
                                .arg(state_->current.height());
      QFont f = font();
      f.setBold(true);
      p.setFont(f);
      const QRect chip = p.boundingRect(QRect(), Qt::AlignCenter, label);
      QRect chip_pos = chip;
      const int cy = local.y() - chip.height() - 6;
      chip_pos.moveTo(local.x(), cy >= 0 ? cy : local.y() + 4);
      p.fillRect(chip_pos.adjusted(-6, -3, 6, 3), QColor(20, 20, 0, 200));
      p.setPen(Qt::white);
      p.drawText(chip_pos, Qt::AlignCenter, label);
    }
  }
  // 顶部操作提示
  const QString hint = QStringLiteral("拖拽选择截图区域 · 松开确认 · Esc 取消");
  QFont f = font();
  f.setPixelSize(qMax(13, height() / 40));
  p.setFont(f);
  const QRect hint_chip = p.boundingRect(QRect(), Qt::AlignCenter, hint);
  QRect hint_pos = hint_chip;
  hint_pos.moveTopLeft(QPoint((width() - hint_chip.width()) / 2, 10));
  p.fillRect(hint_pos.adjusted(-10, -4, 10, 4), QColor(20, 20, 20, 170));
  p.setPen(Qt::white);
  p.drawText(hint_pos, Qt::AlignCenter, hint);
}

void RegionSelectOverlay::mousePressEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton) return;
  setFocus();
  emit pressed(e->globalPos());
}

void RegionSelectOverlay::mouseMoveEvent(QMouseEvent* e) {
  if (state_->selecting) emit moved(e->globalPos());
}

void RegionSelectOverlay::mouseReleaseEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton) return;
  if (state_->selecting) emit released(e->globalPos());
}

void RegionSelectOverlay::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape) emit escapePressed();
}

// —— 标注窗 ——

namespace {

// 文字输入框：Enter 提交（editingFinished），Esc 取消（直接销毁不提交）。
class TextEdit : public QLineEdit {
public:
  explicit TextEdit(QWidget* parent) : QLineEdit(parent) {}

protected:
  void keyPressEvent(QKeyEvent* e) override {
    if (e->key() == Qt::Key_Escape) {
      deleteLater();
      return;
    }
    QLineEdit::keyPressEvent(e);
  }
};

QPushButton* makeButton(const QString& text, const QString& tooltip,
                        QWidget* parent) {
  auto* b = new QPushButton(text, parent);
  b->setToolTip(tooltip);
  b->setCursor(Qt::PointingHandCursor);
  return b;
}

} // namespace

AnnotationWidget::AnnotationWidget(QWidget* parent) : QWidget(parent) {
  setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                 Qt::Tool);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  // —— 工具条 ——
  bar_ = new QWidget(this);
  bar_->setFixedHeight(40);
  bar_->setStyleSheet(QStringLiteral("background:#ffffff;"));
  auto* bar_layout = new QHBoxLayout(bar_);
  bar_layout->setContentsMargins(8, 4, 8, 4);
  bar_layout->setSpacing(4);

  auto* tools = new QButtonGroup(this);
  tools->setExclusive(true);
  const QStringList tool_names = {QStringLiteral("涂鸦"),
                                 QStringLiteral("箭头"),
                                 QStringLiteral("马赛克"),
                                 QStringLiteral("文字")};
  for (int i = 0; i < tool_names.size(); ++i) {
    auto* b = makeButton(tool_names[i], QStringLiteral("标注工具"), bar_);
    b->setCheckable(true);
    b->setStyleSheet(QStringLiteral(
        "QPushButton { background:#f5f0e8; color:#332b24; border:1px solid "
        "#e8e0d6; border-radius:6px; padding:4px 10px; }"
        "QPushButton:checked { background:#e16531; color:#ffffff; "
        "border-color:#e16531; }"));
    tools->addButton(b, i);
    bar_layout->addWidget(b);
  }
  tools->button(0)->setChecked(true);
  QObject::connect(tools, &QButtonGroup::idClicked, this,
                   [this](int id) { setTool(id); });

  bar_layout->addSpacing(8);

  // 颜色：品牌橙默认＋六色（显式 id 100+，避免自动 id 歧义）
  auto* colors = new QButtonGroup(this);
  colors->setExclusive(true);
  const QList<QColor> palette = {
      QColor(QStringLiteral("#e16531")), QColor(QStringLiteral("#d43d2a")),
      QColor(QStringLiteral("#f2c14e")), QColor(QStringLiteral("#3f9d4e")),
      QColor(QStringLiteral("#2a7fd4")), QColor(QStringLiteral("#ffffff")),
      QColor(QStringLiteral("#222222"))};
  for (int i = 0; i < palette.size(); ++i) {
    const QColor& c = palette[i];
    auto* b = new QPushButton(bar_);
    b->setFixedSize(20, 20);
    b->setCheckable(true);
    b->setStyleSheet(QStringLiteral("border-radius:3px; border:1px solid "
                                    "#9b8f86; background:%1;")
                          .arg(c.name()));
    colors->addButton(b, 100 + i);
    bar_layout->addWidget(b);
    QObject::connect(b, &QPushButton::clicked, this,
                     [this, c]() { setColor(c); });
  }
  colors->button(100)->setChecked(true);

  bar_layout->addSpacing(8);

  // 粗细：细／中／粗（显式 id 即像素宽度）
  auto* widths = new QButtonGroup(this);
  widths->setExclusive(true);
  const QList<int> width_vals = {3, 6, 12};
  const QStringList width_names = {QStringLiteral("细"),
                                   QStringLiteral("中"),
                                   QStringLiteral("粗")};
  for (int i = 0; i < width_vals.size(); ++i) {
    auto* b = makeButton(width_names[i], QStringLiteral("笔宽"), bar_);
    b->setCheckable(true);
    b->setStyleSheet(QStringLiteral(
        "QPushButton { background:#f5f0e8; color:#332b24; border:1px solid "
        "#e8e0d6; border-radius:6px; padding:4px 10px; }"
        "QPushButton:checked { background:#e16531; color:#ffffff; "
        "border-color:#e16531; }"));
    widths->addButton(b, width_vals[i]);
    bar_layout->addWidget(b);
  }
  widths->button(6)->setChecked(true);
  QObject::connect(widths, &QButtonGroup::idClicked, this,
                   [this](int w) { setWidth(w); });

  bar_layout->addSpacing(8);
  auto* undo_btn =
      makeButton(QStringLiteral("撤销"), QStringLiteral("撤销上一步"), bar_);
  auto* clear_btn =
      makeButton(QStringLiteral("清空"), QStringLiteral("清空全部标注"), bar_);
  bar_layout->addWidget(undo_btn);
  bar_layout->addWidget(clear_btn);
  QObject::connect(undo_btn, &QPushButton::clicked, this, [this] {
    canvas_.undo();
    refreshDisplay();
  });
  QObject::connect(clear_btn, &QPushButton::clicked, this, [this] {
    canvas_.clear();
    refreshDisplay();
  });

  bar_layout->addStretch();
  auto* cancel_btn =
      makeButton(QStringLiteral("取消"), QStringLiteral("取消（Esc）"), bar_);
  auto* ok_btn = makeButton(QStringLiteral("确认发送"),
                            QStringLiteral("保存并发送到当前会话（Enter）"),
                            bar_);
  ok_btn->setStyleSheet(QStringLiteral(
      "QPushButton { background:#e16531; color:#ffffff; border:none; "
      "border-radius:6px; padding:4px 14px; font-weight:600; }"));
  cancel_btn->setStyleSheet(QStringLiteral(
      "QPushButton { background:#f5f0e8; color:#332b24; border:1px solid "
      "#e8e0d6; border-radius:6px; padding:4px 14px; }"));
  bar_layout->addWidget(cancel_btn);
  bar_layout->addWidget(ok_btn);
  QObject::connect(cancel_btn, &QPushButton::clicked, this,
                   [this] { emit cancelled(); });
  QObject::connect(ok_btn, &QPushButton::clicked, this,
                   [this] { confirm(); });

  layout->addWidget(bar_);

  canvas_view_ = new CanvasView(this);
  canvas_view_->setMouseTracking(true);
  layout->addWidget(canvas_view_, 1);
}

void AnnotationWidget::setImage(const QPixmap& img, QScreen* screen) {
  canvas_.setImage(img);
  if (!canvas_.hasImage()) return;
  // 目标屏：取屏区域所在屏（缺省主屏）；图像缩到屏可用区 92%，不超过 1:1
  QScreen* target = screen ? screen : QGuiApplication::primaryScreen();
  const QRect avail = target->availableGeometry();
  const double sx = avail.width() * 0.92 / img.width();
  const double sy = avail.height() * 0.92 / img.height();
  scale_ = qMax(0.1, qMin(1.0, qMin(sx, sy)));
  const QSize shown(qRound(img.width() * scale_),
                    qRound(img.height() * scale_));
  resize(shown.width() + 24, shown.height() + 40 + 24);
  move(avail.center() - QPoint(width() / 2, height() / 2));
  image_pos_ = QPoint(12, 40 + 12);
  refreshDisplay();
  show();
  raise();
  setFocus();
}

void AnnotationWidget::refreshDisplay() {
  if (!canvas_.hasImage()) return;
  const QSize full = canvas_.imageSize();
  display_ = canvas_.render().scaled(
      qRound(full.width() * scale_), qRound(full.height() * scale_),
      Qt::KeepAspectRatio, Qt::SmoothTransformation);
  canvas_view_->update();
}

void AnnotationWidget::setTool(int tool) { tool_ = tool; }

void AnnotationWidget::setColor(const QColor& c) { color_ = c; }

void AnnotationWidget::setWidth(int w) { width_ = w; }

QPoint AnnotationWidget::toImage(const QPoint& view_pos) const {
  return QPoint(qRound((view_pos.x() - image_pos_.x()) / scale_),
                qRound((view_pos.y() - image_pos_.y()) / scale_));
}

void AnnotationWidget::canvasPress(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton || text_edit_) return;
  const QPoint img = toImage(e->pos());
  if (tool_ == 3) { // 文字：落输入框
    text_edit_ = new TextEdit(canvas_view_);
    text_edit_->setStyleSheet(QStringLiteral(
        "QLineEdit { background:#ffffff; border:2px solid #e16531; "
        "border-radius:4px; padding:2px 6px; }"));
    QFont f = font();
    f.setPixelSize(qMax(14, qRound(24 * scale_)));
    text_edit_->setFont(f);
    text_edit_->move(e->pos() - QPoint(2, 2));
    text_edit_->resize(qMax(160, qRound(260 * scale_)),
                       qMax(28, qRound(34 * scale_)));
    text_edit_->setPlaceholderText(QStringLiteral("输入文字"));
    text_edit_->show();
    text_edit_->setFocus();
    QObject::connect(text_edit_, &QLineEdit::editingFinished, this,
                     [this] { commitTextEdit(); });
    return;
  }
  drawing_ = true;
  pending_ = AnnotationCommand();
  pending_.color = color_;
  pending_.width = width_;
  if (tool_ == 0) {
    pending_.type = AnnotationCommand::Type::kPen;
    pending_.points = {img};
  } else if (tool_ == 1) {
    pending_.type = AnnotationCommand::Type::kArrow;
    pending_.points = {img, img};
  } else {
    pending_.type = AnnotationCommand::Type::kMosaic;
    pending_.rect = QRect(img, QSize(0, 0));
  }
}

void AnnotationWidget::canvasMove(QMouseEvent* e) {
  if (!drawing_) return;
  const QPoint img = toImage(e->pos());
  if (pending_.type == AnnotationCommand::Type::kPen) {
    const QPoint last = pending_.points.last();
    if (QPoint(img - last).manhattanLength() >= 2) pending_.points.append(img);
  } else if (pending_.type == AnnotationCommand::Type::kArrow) {
    pending_.points[1] = img;
  } else if (pending_.type == AnnotationCommand::Type::kMosaic) {
    pending_.rect = QRect(pending_.rect.topLeft(), img);
  }
  refreshDisplay();
}

void AnnotationWidget::canvasRelease(QMouseEvent* e) {
  if (!drawing_ || e->button() != Qt::LeftButton) return;
  drawing_ = false;
  bool valid = false;
  if (pending_.type == AnnotationCommand::Type::kPen) {
    valid = pending_.points.size() >= 2;
  } else if (pending_.type == AnnotationCommand::Type::kArrow) {
    valid = (pending_.points.value(0) - pending_.points.value(1))
                .manhattanLength() >= 6;
  } else if (pending_.type == AnnotationCommand::Type::kMosaic) {
    valid = pending_.rect.width() >= 4 && pending_.rect.height() >= 4;
  }
  if (valid) {
    canvas_.addCommand(pending_);
    refreshDisplay();
  }
  pending_ = AnnotationCommand();
}

void AnnotationWidget::cancelTextEdit() {
  if (!text_edit_) return;
  text_edit_->deleteLater();
  text_edit_ = nullptr;
  setFocus();
}

void AnnotationWidget::commitTextEdit() {
  if (!text_edit_) return;
  const QString text = text_edit_->text().trimmed();
  const QPoint pos = text_edit_->pos();
  text_edit_->deleteLater();
  text_edit_ = nullptr;
  setFocus();
  if (text.isEmpty()) return;
  AnnotationCommand cmd;
  cmd.type = AnnotationCommand::Type::kText;
  cmd.color = color_;
  cmd.text = text;
  cmd.text_pos = toImage(pos);
  canvas_.addCommand(cmd);
  refreshDisplay();
}

void AnnotationWidget::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape && !text_edit_) emit cancelled();
  else if (e->key() == Qt::Key_Return) confirm();
}

void AnnotationWidget::confirm() {
  if (!canvas_.hasImage()) return;
  const QString dir = QDir::tempPath() + QStringLiteral("/memex-screenshots");
  QDir().mkpath(dir);
  const QString path =
      dir + QStringLiteral("/memex-shot-%1-%2.png")
                .arg(QDateTime::currentMSecsSinceEpoch())
                .arg(QRandomGenerator::global()->generate() % 100000, 5, 10,
                     QChar('0'));
  if (!canvas_.save(path)) {
    emit failed(QStringLiteral("截图保存失败：%1").arg(path));
    return;
  }
  emit confirmed(path);
}

// —— 截图工具主流程 ——

ScreenshotTool::ScreenshotTool(QObject* parent) : QObject(parent) {
  // 取屏完成 → 标注窗（取屏区域所在屏，缺省主屏）
  connect(this, &ScreenshotTool::captured, this, [this](const QPixmap& img) {
    if (widget_) return;
    widget_ = new AnnotationWidget();
    connect(widget_, &AnnotationWidget::confirmed, this,
            &ScreenshotTool::onWidgetConfirmed);
    connect(widget_, &AnnotationWidget::cancelled, this,
            &ScreenshotTool::onWidgetCancelled);
    connect(widget_, &AnnotationWidget::failed, this,
            &ScreenshotTool::onWidgetFailed);
    QScreen* screen = QGuiApplication::primaryScreen();
    for (QScreen* s : QGuiApplication::screens()) {
      if (s->geometry().contains(last_region_.center())) {
        screen = s;
        break;
      }
    }
    widget_->setImage(img, screen);
  });
}

ScreenshotTool::~ScreenshotTool() {
  closeOverlays();
  delete widget_;
}

void ScreenshotTool::start() {
  if (running_) return;
  running_ = true;
  state_ = SelectionState();
  showOverlays();
}

void ScreenshotTool::startWithImage(const QPixmap& img) {
  if (running_ || img.isNull()) return;
  running_ = true;
  state_ = SelectionState();
  emit captured(img);
}

void ScreenshotTool::showOverlays() {
  for (QScreen* s : QGuiApplication::screens()) {
    auto* ov = new RegionSelectOverlay(s, &state_);
    connect(ov, &RegionSelectOverlay::pressed, this,
            &ScreenshotTool::onOverlayPressed);
    connect(ov, &RegionSelectOverlay::moved, this,
            &ScreenshotTool::onOverlayMoved);
    connect(ov, &RegionSelectOverlay::released, this,
            &ScreenshotTool::onOverlayReleased);
    connect(ov, &RegionSelectOverlay::escapePressed, this,
            &ScreenshotTool::onOverlayEscape);
    overlays_.append(ov);
    ov->show();
    ov->setFocus();
  }
}

void ScreenshotTool::closeOverlays() {
  for (RegionSelectOverlay* ov : overlays_) ov->deleteLater();
  overlays_.clear();
}

void ScreenshotTool::repaintOverlays() {
  for (RegionSelectOverlay* ov : overlays_) ov->update();
}

void ScreenshotTool::onOverlayPressed(const QPoint& device_pt) {
  if (state_.selecting) return;
  state_.selecting = true;
  state_.start = device_pt;
  state_.current = QRect(device_pt, QSize(0, 0));
  repaintOverlays();
}

void ScreenshotTool::onOverlayMoved(const QPoint& device_pt) {
  if (!state_.selecting) return;
  state_.current = QRect(state_.start, device_pt).normalized();
  repaintOverlays();
}

void ScreenshotTool::onOverlayReleased(const QPoint& device_pt) {
  if (!state_.selecting) return;
  state_.selecting = false;
  const QRect rect = QRect(state_.start, device_pt).normalized();
  state_.current = QRect();
  closeOverlays();
  if (rect.width() < 4 || rect.height() < 4) { // 误触（单击）→ 取消
    running_ = false;
    emit cancelled();
    return;
  }
  last_region_ = rect;
  // 覆盖层先隐藏再取屏，避免把选区 UI 截进去
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  const QPixmap shot = QGuiApplication::primaryScreen()->grabWindow(
      0, rect.x(), rect.y(), rect.width(), rect.height());
  if (shot.isNull()) {
    running_ = false;
    emit failed(QStringLiteral(
        "当前桌面环境不支持截屏（Wayland 会话需门户授权）；"
        "可改用「发文件」发送图片"));
    return;
  }
  emit captured(shot);
}

void ScreenshotTool::onOverlayEscape() {
  if (!running_) return;
  state_.selecting = false;
  state_.current = QRect();
  closeOverlays();
  running_ = false;
  emit cancelled();
}

AnnotationCanvas* ScreenshotTool::canvas() {
  return widget_ ? widget_->canvas() : nullptr;
}

void ScreenshotTool::confirmCurrent() {
  if (widget_) widget_->confirm();
}

void ScreenshotTool::onWidgetConfirmed(const QString& path) {
  if (widget_) {
    widget_->deleteLater();
    widget_ = nullptr;
  }
  running_ = false;
  emit confirmed(path);
}

void ScreenshotTool::onWidgetCancelled() {
  if (widget_) {
    widget_->deleteLater();
    widget_ = nullptr;
  }
  running_ = false;
  emit cancelled();
}

void ScreenshotTool::onWidgetFailed(const QString& reason) {
  if (widget_) {
    widget_->deleteLater();
    widget_ = nullptr;
  }
  running_ = false;
  emit failed(reason);
}

} // namespace memex::client
