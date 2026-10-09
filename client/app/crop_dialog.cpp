#include "crop_dialog.hpp"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QSlider>
#include <QVBoxLayout>
#include <QtGlobal>

#include <algorithm>

namespace memex::client {

namespace {
constexpr int kOutputSide = 256; // 输出方形边长（四档中最大档）
constexpr int kViewSide = 320;   // 取景框边长（部件像素）
constexpr int kTopMargin = 24;   // 取景框上边距
constexpr int kSideMargin = 36;  // 取景框左右边距
constexpr int kCtrlArea = 128;   // 取景框下方控制区高度
constexpr double kZoomMax = 4.0; // 放大上限（倍率，1=初始盖满）
} // namespace

CropDialog::CropDialog(const QImage& source, QWidget* parent)
    : QDialog(parent), source_(source) {
  setWindowTitle(QStringLiteral("裁剪头像（拖动取景，滚轮缩放）"));
  setModal(true);
  // 初始缩放：短边恰盖满取景框（此后窗口恒在源图内，拖移不露底）
  const double w = static_cast<double>(source_.width());
  const double h = static_cast<double>(source_.height());
  min_scale_ = static_cast<double>(kViewSide) / std::min(w, h);
  scale_ = min_scale_;

  setFixedSize(kViewSide + 2 * kSideMargin, kViewSide + kTopMargin + kCtrlArea);
  canvas_origin_ = QPointF(kSideMargin, kTopMargin);
  clamp_view();

  auto* root = new QVBoxLayout(this);
  root->addSpacing(kTopMargin + kViewSide + 8);
  auto* slider_row = new QHBoxLayout;
  zoom_slider_ = new QSlider(Qt::Horizontal, this);
  zoom_slider_->setRange(100, static_cast<int>(kZoomMax * 100));
  zoom_slider_->setValue(100);
  connect(zoom_slider_, &QSlider::valueChanged, this, [this](int v) {
    set_zoom(static_cast<double>(v) / 100.0);
  });
  slider_row->addWidget(new QLabel(QStringLiteral("缩放"), this));
  slider_row->addWidget(zoom_slider_, 1);
  root->addLayout(slider_row);

  auto* hint = new QLabel(QStringLiteral("拖动调整取景；确认后裁为方形"), this);
  hint->setAlignment(Qt::AlignCenter);
  root->addWidget(hint);

  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  root->addWidget(buttons);
}

void CropDialog::clamp_view() {
  const double w = static_cast<double>(source_.width());
  const double h = static_cast<double>(source_.height());
  const double win = static_cast<double>(kViewSide) / scale_;
  view_tl_.setX(std::clamp(view_tl_.x(), 0.0, std::max(0.0, w - win)));
  view_tl_.setY(std::clamp(view_tl_.y(), 0.0, std::max(0.0, h - win)));
}

void CropDialog::set_zoom(double zoom) {
  // 以取景框中心为锚：中心源图坐标不动，窗口变大变小
  const double win = static_cast<double>(kViewSide) / scale_;
  const QPointF center = view_tl_ + QPointF(win / 2.0, win / 2.0);
  scale_ = std::clamp(min_scale_ * zoom, min_scale_, min_scale_ * kZoomMax);
  const double new_win = static_cast<double>(kViewSide) / scale_;
  view_tl_ = center - QPointF(new_win / 2.0, new_win / 2.0);
  clamp_view();
  const int slider_v = static_cast<int>(scale_ / min_scale_ * 100.0);
  if (zoom_slider_ && zoom_slider_->value() != slider_v) {
    zoom_slider_->setValue(slider_v); // 回灌（不再触发本函数：值已相同）
  }
  update();
}

void CropDialog::set_view(double zoom, double cx, double cy) {
  scale_ = min_scale_ * std::clamp(zoom, 1.0, kZoomMax);
  const double win = static_cast<double>(kViewSide) / scale_;
  const double w = static_cast<double>(source_.width());
  const double h = static_cast<double>(source_.height());
  view_tl_.setX(std::clamp(cx * w - win / 2.0, 0.0, std::max(0.0, w - win)));
  view_tl_.setY(std::clamp(cy * h - win / 2.0, 0.0, std::max(0.0, h - win)));
  update();
}

QRectF CropDialog::source_window() const {
  const double win = static_cast<double>(kViewSide) / scale_;
  return QRectF(view_tl_, QSizeF(win, win));
}

QImage CropDialog::cropped() const {
  const QRectF win = source_window();
  QRect pixel(qRound(win.x()), qRound(win.y()), qRound(win.width()),
              qRound(win.height()));
  pixel = pixel.intersected(source_.rect());
  return source_.copy(pixel)
      .scaled(kOutputSide, kOutputSide, Qt::IgnoreAspectRatio,
              Qt::SmoothTransformation);
}

void CropDialog::paintEvent(QPaintEvent*) {
  QPainter p(this);
  const QRectF canvas(canvas_origin_, QSizeF(kViewSide, kViewSide));
  p.fillRect(canvas, QColor(24, 24, 24)); // 透明 PNG 不露馅
  p.save();
  p.setClipRect(canvas);
  const QRectF target(canvas_origin_.x() - view_tl_.x() * scale_,
                      canvas_origin_.y() - view_tl_.y() * scale_,
                      source_.width() * scale_, source_.height() * scale_);
  p.drawImage(target, source_);
  p.restore();
  // 取景框外遮罩变暗（全件路径挖去框）
  QPainterPath outside;
  outside.addRect(QRectF(rect()));
  QPainterPath box;
  box.addRect(canvas);
  p.fillPath(outside.subtracted(box), QColor(0, 0, 0, 102));
  QPen pen(QColor(225, 101, 49), 2); // 品牌橙描边
  p.setPen(pen);
  p.drawRect(canvas);
}

void CropDialog::mousePressEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton &&
      QRectF(canvas_origin_, QSizeF(kViewSide, kViewSide))
          .contains(e->position())) {
    dragging_ = true;
    drag_last_ = e->position();
  }
}

void CropDialog::mouseMoveEvent(QMouseEvent* e) {
  if (!dragging_) return;
  const QPointF d = e->position() - drag_last_;
  drag_last_ = e->position();
  view_tl_ -= d / scale_; // 拖图像=取景窗口在源图上反向移动
  clamp_view();
  update();
}

void CropDialog::mouseReleaseEvent(QMouseEvent*) { dragging_ = false; }

void CropDialog::wheelEvent(QWheelEvent* e) {
  const double step = e->angleDelta().y() > 0 ? 1.1 : 1.0 / 1.1;
  set_zoom(std::clamp(scale_ / min_scale_ * step, 1.0, kZoomMax));
}

} // namespace memex::client
