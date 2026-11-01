#pragma once
#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

#include "theme.hpp"

// 左侧图标导航栏（原型 docs/design/prototypes/ 桌面屏 rail 面的 Qt 落地）：
// 60px 竖排、42px 方位圆角 10、选中态品牌浅底＋左缘 3px 品牌条（prototypes.css
// .rail 同款）；图标按原型 lucide 线稿 QPainter 手绘，颜色全走主题令牌。
namespace memex::client {

class RailNav : public QWidget {
  Q_OBJECT
 public:
  enum class Icon { Chat, Devices, Folder, Share, Org, Person, Search, Gear, Mail };
  struct Item {
    QString id;   // 点击回执标识（rail_clicked 参数）
    QString tip;  // 悬浮提示（原型 .tip 文案）
    Icon icon{Icon::Chat};
    bool spacer{false};   // 弹性占位（原型 .rail .spacer：上下两组分隔）
    bool disabled{false}; // 无功能体的原型占位项（共享空间）：置灰不可点
    bool avatar{false};   // 底部本人头像项（首字圆角牌＋在线点）
    int badge{-1};        // 未读徽章（原型 .n-count；-1=无徽章，0=不显）
  };
  explicit RailNav(QWidget* parent = nullptr);
  void set_items(const QVector<Item>& items); // 分组与顺序严格按原型
  void set_active(const QString& id);
  void set_avatar_text(const QString& text);  // 头像首字（账号/设备名首字）
  void set_presence(bool show, bool online);  // 协作态在线点（离线灰点）
  // 未读徽章（原型 .rail .n-count：17px 品牌底白字胶囊，right:2 top:2）：
  // count<=0 摘除（未读清零不冒充有待办）
  void set_badge(const QString& id, int count);
  int badge_count(const QString& id) const;   // 断言面：无徽章=-1
  void apply_tokens(const ThemeTokens& t);    // 主题切换重刷（令牌即状态）
  QSize sizeHint() const override { return {60, 240}; }
 signals:
  void item_clicked(const QString& id);
 protected:
  void paintEvent(QPaintEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;
  bool event(QEvent* event) override; // QEvent::ToolTip：悬浮出原型 .tip 文案
 private:
  int item_at(const QPoint& pos) const;
  QRect item_rect(int index) const;
  void paint_icon(QPainter& p, Icon icon, const QRectF& box,
                  const QColor& color) const;
  void paint_badge(QPainter& p, const QRect& r, int count); // .n-count 胶囊

  QVector<Item> items_;
  int active_{-1};
  int hover_{-1};
  QString avatar_text_{QStringLiteral("我")};
  bool presence_show_{false};
  bool presence_online_{false};
  ThemeTokens t_;
  bool has_tokens_{false};
};

}  // namespace memex::client
