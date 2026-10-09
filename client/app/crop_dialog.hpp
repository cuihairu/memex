// 方形裁剪面（需求批⑫）：头像上传的第二步——选图后拖动取景＋缩放裁出
// 正方。Qt 自绘（paintEvent 画图＋遮罩，鼠标拖移、滚轮/滑杆缩放），不引
// 任何第三方图像库。输出恒 256×256（四档中最大档；其余档由消费面缩放）。
#pragma once

#include <QDialog>
#include <QImage>
#include <QPointF>

class QLabel;
class QSlider;

namespace memex::client {

class CropDialog : public QDialog {
  Q_OBJECT
 public:
  // source 须可读（QImage 非空）；调用方保证（选图后 QPixmap 已过打开门）
  explicit CropDialog(const QImage& source, QWidget* parent = nullptr);

  // 裁剪结果：取景框对应源图方形区域，平滑缩放为 256×256
  QImage cropped() const;

  // —— 测试缝：程序化取景（绕开鼠标交互）——
  // zoom=放大倍率（1=初始盖满取景框）；cx/cy=取景框中心在源图中的
  // 归一化坐标（0~1，自动夹取窗口不越出源图的合法位置）
  void set_view(double zoom, double cx, double cy);
  // 当前取景框对应的源图区域（像素坐标；恒正方形）
  QRectF source_window() const;

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

 private:
  // 以取景框中心为锚缩放（zoom=相对最小缩放的倍率；同步滑杆）
  void set_zoom(double zoom);
  // 窗口夹回源图内（拖移/缩放后调用；源图小于窗口时贴 0）
  void clamp_view();

  QImage source_;
  double scale_{1.0};     // 源图像素 → 部件像素
  double min_scale_{1.0}; // 短边恰盖满取景框的缩放（zoom=1）
  QPointF view_tl_{0, 0}; // 取景框左上角对应的源图坐标（拖移直接改它）
  QPointF canvas_origin_; // 取景框左上角部件坐标（构造定）
  QPointF drag_last_;     // 拖移中的鼠标上一部件坐标
  bool dragging_{false};
  QSlider* zoom_slider_{nullptr};
};

} // namespace memex::client
