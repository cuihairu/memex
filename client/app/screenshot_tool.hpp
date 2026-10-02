// 截图与标注（T4.4）：区域选择（跨多显示器／高 DPI 设备像素对齐）→ 取屏 →
// 涂鸦／箭头／马赛克／文字标注 → 确认即发送（PNG 临时文件走既有文件通道）。
// 取屏失败（如 Wayland 会话无门户授权）明确降级提示，不静默失败（A21）。
#pragma once

#include <QColor>
#include <QObject>
#include <QPainter>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

#include <QScreen>
#include <QWidget>

class QLineEdit;

namespace memex::client {

// —— 标注命令与画布 ——
// 全部以图像（设备）像素坐标记录，与显示缩放无关。

struct AnnotationCommand {
  enum class Type { kPen, kArrow, kMosaic, kText };
  Type type{Type::kPen};
  QColor color{Qt::black};
  int width{6};            // 图像像素
  QVector<QPoint> points;  // 涂鸦折线／箭头两端
  QRect rect;              // 马赛克区域
  QString text;            // 文字内容
  QPoint text_pos;         // 文字锚点（图像像素，左上）
};

// 标注画布：原图＋命令栈渲染。马赛克破坏性作用于底图副本，涂鸦／箭头／文字
// 绘于透明覆盖层；撤销＝弹栈重渲染（渲染始终从原图重建，撤销天然正确）。
class AnnotationCanvas : public QObject {
  Q_OBJECT

public:
  explicit AnnotationCanvas(QObject* parent = nullptr);

  void setImage(const QPixmap& img);
  bool hasImage() const { return !original_.isNull(); }
  QSize imageSize() const { return original_.size(); }

  void addCommand(const AnnotationCommand& cmd);
  bool undo(); // 撤销末条命令；空栈返回 false
  void clear();
  int commandCount() const { return commands_.size(); }

  QPixmap render() const; // 底图＋覆盖层合成（全分辨率）
  bool save(const QString& path) const;

private:
  static void applyMosaic(QPixmap& base, const AnnotationCommand& cmd);

  QPixmap original_; // 原始截屏（马赛克只作用于其副本）
  QVector<AnnotationCommand> commands_;
};

// —— 区域选择 ——

// 选区（虚拟屏设备像素）与单屏逻辑几何求交，返回该屏内的逻辑像素矩形；
// 不相交返回空矩形。覆盖层只画本屏部分——多显示器下选区可跨屏。
QRect region_to_local(const QRect& device_rect, const QPoint& logical_top_left,
                      const QSize& logical_size, double dpr);

// 区域选择共享状态（工具持有，各覆盖层只读）。
struct SelectionState {
  bool selecting{false};
  QPoint start;  // 虚拟屏设备像素
  QRect current; // 虚拟屏设备像素
};

// 区域选择覆盖层（每屏一个）：全屏压暗＋选区高亮＋尺寸标签；
// 拖拽跨屏选择，松开确认，Esc 取消。坐标为虚拟屏设备像素。
class RegionSelectOverlay : public QWidget {
  Q_OBJECT

public:
  RegionSelectOverlay(QScreen* screen, SelectionState* state,
                      QWidget* parent = nullptr);

signals:
  void pressed(const QPoint& device_pt);
  void moved(const QPoint& device_pt);
  void released(const QPoint& device_pt);
  void escapePressed();

protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void keyPressEvent(QKeyEvent*) override;

private:
  QScreen* screen_;
  SelectionState* state_;
};

// —— 标注窗 ——

// 标注窗：工具条（涂鸦／箭头／马赛克／文字＋颜色＋粗细＋撤销）＋画布。
// 确认（Enter）存 PNG 出 confirmed 路径；取消（Esc）出 cancelled。
class AnnotationWidget : public QWidget {
  Q_OBJECT

public:
  // 画布区：承载图像显示与绘制鼠标事件（嵌套类可访问标注窗私有成员）。
  class CanvasView : public QWidget {
  public:
    explicit CanvasView(AnnotationWidget* owner)
        : QWidget(owner), owner_(owner) {}
    void paintEvent(QPaintEvent*) override {
      QPainter p(this);
      p.fillRect(rect(), QColor(QStringLiteral("#f5f0e8")));
      p.drawPixmap(owner_->image_pos_, owner_->display_);
    }
    void mousePressEvent(QMouseEvent* e) override { owner_->canvasPress(e); }
    void mouseMoveEvent(QMouseEvent* e) override { owner_->canvasMove(e); }
    void mouseReleaseEvent(QMouseEvent* e) override {
      owner_->canvasRelease(e);
    }

  private:
    AnnotationWidget* owner_;
  };

  explicit AnnotationWidget(QWidget* parent = nullptr);
  void setImage(const QPixmap& img, QScreen* screen);
  AnnotationCanvas* canvas() { return &canvas_; }
  void confirm();

signals:
  void confirmed(const QString& saved_path);
  void cancelled();
  void failed(const QString& reason);

protected:
  void keyPressEvent(QKeyEvent*) override;

private:
  void refreshDisplay();
  void cancelTextEdit();
  void commitTextEdit();
  void setTool(int tool); // 0 涂鸦 1 箭头 2 马赛克 3 文字
  void setColor(const QColor& c);
  void setWidth(int w);
  QPoint toImage(const QPoint& view_pos) const; // 控件坐标 → 图像像素
  void canvasPress(QMouseEvent* e);
  void canvasMove(QMouseEvent* e);
  void canvasRelease(QMouseEvent* e);

  AnnotationCanvas canvas_;
  int tool_{0};
  QColor color_{QColor(QStringLiteral("#e16531"))};
  int width_{6};
  bool drawing_{false};
  AnnotationCommand pending_;
  double scale_{1.0};
  QPoint image_pos_{0, 0}; // 画布内图像左上角（控件坐标）
  QPixmap display_;        // 缩放后的渲染
  QWidget* bar_{nullptr};
  QWidget* canvas_view_{nullptr};
  QLineEdit* text_edit_{nullptr};
};

// —— 截图工具主流程 ——
// start()：区域选择（真实取屏）；startWithImage()：测试缝，跳过取屏直接标注。
// 取屏失败出 failed（界面层降级提示）；标注确认出 confirmed(PNG 路径)。

class ScreenshotTool : public QObject {
  Q_OBJECT

public:
  explicit ScreenshotTool(QObject* parent = nullptr);
  ~ScreenshotTool() override;

  void start();
  void startWithImage(const QPixmap& img);
  bool isRunning() const { return running_; }
  AnnotationCanvas* canvas(); // 测试缝：直接注入标注命令
  void confirmCurrent();      // 测试缝：保存当前渲染并出 confirmed

signals:
  void captured(const QPixmap& img); // 区域选定（已取屏）
  void confirmed(const QString& saved_path);
  void cancelled();
  void failed(const QString& reason);

private slots:
  void onOverlayPressed(const QPoint& device_pt);
  void onOverlayMoved(const QPoint& device_pt);
  void onOverlayReleased(const QPoint& device_pt);
  void onOverlayEscape();

private:
  void showOverlays();
  void closeOverlays();
  void repaintOverlays();
  void onWidgetConfirmed(const QString& path);
  void onWidgetCancelled();
  void onWidgetFailed(const QString& reason);

  SelectionState state_;
  QRect last_region_; // 最近一次取屏区域（设备像素；标注窗定位用）
  QVector<RegionSelectOverlay*> overlays_;
  AnnotationWidget* widget_{nullptr};
  bool running_{false};
};

} // namespace memex::client
