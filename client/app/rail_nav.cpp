#include "rail_nav.hpp"

#include <QEvent>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include <cmath>

namespace memex::client {
namespace {
constexpr int kRailW = 60;    // 原型 --rail-w
constexpr int kItem = 42;     // 原型 .rail-item 边长
constexpr int kGap = 4;       // 原型 .rail gap
constexpr int kPad = 10;      // 上下留白（原型 12px，项位另计头像下移量取整）
constexpr int kIconGrid = 24; // lucide 24 网格
}  // namespace

RailNav::RailNav(QWidget* parent) : QWidget(parent) {
  setFixedWidth(kRailW);
  setMouseTracking(true);
}

void RailNav::set_items(const QVector<Item>& items) {
  items_ = items;
  active_ = -1;
  hover_ = -1;
  for (int i = 0; i < items_.size(); ++i) {
    if (!items_[i].spacer && items_[i].id == QStringLiteral("messages")) {
      active_ = i; // 消息工作区常亮（本版文件/架构走对话框，不换屏）
    }
  }
  update();
}

void RailNav::set_active(const QString& id) {
  for (int i = 0; i < items_.size(); ++i) {
    if (items_[i].id == id) {
      active_ = i;
      update();
      return;
    }
  }
}

void RailNav::set_avatar_text(const QString& text) {
  // 首字牌（原型 avatar-me：张／访＝名首字；账号取首字符）
  avatar_text_ = text.isEmpty() ? QStringLiteral("我") : text.left(1);
  update();
}

void RailNav::set_presence(bool show, bool online) {
  presence_show_ = show;
  presence_online_ = online;
  update();
}

void RailNav::apply_tokens(const ThemeTokens& t) {
  t_ = t;
  has_tokens_ = true;
  update();
}

QRect RailNav::item_rect(int index) const {
  int spacer_idx = -1;
  for (int i = 0; i < items_.size(); ++i) {
    if (items_[i].spacer) {
      spacer_idx = i;
      break;
    }
  }
  const int y_top = kPad + index * (kItem + kGap);
  if (spacer_idx < 0 || index < spacer_idx) {
    return QRect((kRailW - kItem) / 2, y_top, kItem, kItem);
  }
  // 底部组：自底向上堆叠（原型 spacer 弹性把下组压到栏底）
  const int n_bottom = items_.size() - spacer_idx - 1;
  const int k = index - spacer_idx - 1;
  const int y = height() - kPad - (n_bottom - k) * kItem - (n_bottom - 1 - k) * kGap;
  return QRect((kRailW - kItem) / 2, y, kItem, kItem);
}

int RailNav::item_at(const QPoint& pos) const {
  for (int i = 0; i < items_.size(); ++i) {
    if (items_[i].spacer) continue;
    if (item_rect(i).contains(pos)) return i;
  }
  return -1;
}

void RailNav::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QColor border = has_tokens_ ? t_.border : palette().color(QPalette::Mid);
  const QColor bg = has_tokens_ ? t_.surface_alt : palette().color(QPalette::Window);
  p.fillRect(rect(), bg);
  p.setPen(border);
  p.drawLine(width() - 1, 0, width() - 1, height());

  for (int i = 0; i < items_.size(); ++i) {
    if (items_[i].spacer) continue;
    const QRect r = item_rect(i);
    if (r.bottom() < 0 || r.top() > height()) continue;
    const bool active = i == active_;
    if (active && has_tokens_) {
      // 原型 .rail-item.active：品牌浅底圆角方位
      p.setPen(Qt::NoPen);
      p.setBrush(t_.brand_tint);
      p.drawRoundedRect(r, 10, 10);
      // 左缘 3px 品牌条（.rail-item.active::before）
      p.setBrush(t_.brand);
      QRectF pill(0, r.top() + 10, 3, r.height() - 20);
      p.drawRoundedRect(pill, 1.5, 1.5);
    } else if (i == hover_ && has_tokens_ && !items_[i].disabled) {
      p.setPen(Qt::NoPen);
      p.setBrush(t_.brand_wash);
      p.drawRoundedRect(r, 10, 10);
    }
    if (items_[i].avatar) {
      // 本人头像牌（原型 .avatar-me：34px 圆角 10）＋在线点
      const qreal av = 34;
      const QRectF a(r.left() + (r.width() - av) / 2,
                     r.top() + (r.height() - av) / 2, av, av);
      if (has_tokens_) {
        p.setPen(Qt::NoPen);
        p.setBrush(t_.brand_tint);
        p.drawRoundedRect(a, 10, 10);
        p.setPen(QPen(t_.brand_text, 1));
      }
      QFont f = font();
      f.setBold(true);
      f.setPixelSize(15);
      p.setFont(f);
      p.drawText(a, Qt::AlignCenter, avatar_text_);
      if (presence_show_ && has_tokens_) {
        const qreal d = 4.5;
        QPointF c(a.right() - d + 1, a.bottom() - d + 1);
        p.setPen(QPen(bg, 2));
        p.setBrush(presence_online_ ? t_.success : t_.text_muted);
        p.drawEllipse(c, d, d);
      }
      continue;
    }
    QColor icon_col = has_tokens_ ? t_.text_muted : palette().color(QPalette::WindowText);
    if (active && has_tokens_) icon_col = t_.brand_text;
    else if (i == hover_ && has_tokens_) icon_col = t_.text;
    if (items_[i].disabled) {
      icon_col = has_tokens_ ? t_.text_muted : icon_col;
      icon_col.setAlpha(100); // 占位项置灰：无功能体不冒充可用入口
    }
    paint_icon(p, items_[i].icon, QRectF(r), icon_col);
  }
}

void RailNav::paint_icon(QPainter& p, Icon icon, const QRectF& box,
                         const QColor& color) const {
  p.save();
  const QPointF off(box.left() + (box.width() - kIconGrid) / 2,
                    box.top() + (box.height() - kIconGrid) / 2);
  p.translate(off);
  QPen pen(color, 1.8);
  pen.setCapStyle(Qt::RoundCap);
  pen.setJoinStyle(Qt::RoundJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);

  QPainterPath path;
  switch (icon) {
    case Icon::Chat: {
      p.drawEllipse(QRectF(4, 3.5, 15.5, 15.5));
      path.moveTo(7.6, 18.8);
      path.lineTo(6.6, 21.3);
      path.lineTo(10.7, 19.1);
      p.drawPath(path);
      break;
    }
    case Icon::Devices: {
      p.drawRoundedRect(QRectF(3, 4, 18, 13), 2, 2);
      p.drawLine(QPointF(12, 17), QPointF(12, 20.6));
      p.drawLine(QPointF(8, 20.6), QPointF(16, 20.6));
      break;
    }
    case Icon::Folder: {
      path.moveTo(3, 19);
      path.lineTo(3, 7);
      path.quadTo(3, 5, 5, 5);
      path.lineTo(8.6, 5);
      path.quadTo(9.8, 5, 10.5, 6);
      path.lineTo(11.2, 7);
      path.lineTo(19, 7);
      path.quadTo(21, 7, 21, 9);
      path.lineTo(21, 19);
      path.quadTo(21, 21, 19, 21);
      path.lineTo(5, 21);
      path.quadTo(3, 21, 3, 19);
      p.drawPath(path);
      break;
    }
    case Icon::Share: {
      p.drawRoundedRect(QRectF(3, 4.5, 18, 15), 2, 2);
      p.drawLine(QPointF(3, 10), QPointF(21, 10));
      break;
    }
    case Icon::Org: {
      // 前人（实线）＋后人（右半弧，左半被前人遮住——lucide users 同构）
      p.drawEllipse(QRectF(5.6, 5.2, 6.8, 6.8));
      path.moveTo(2.8, 19.6);
      path.cubicTo(3.6, 16.2, 6.0, 14.6, 9.0, 14.6);
      path.cubicTo(12.0, 14.6, 14.4, 16.2, 15.2, 19.6);
      p.drawPath(path);
      p.drawArc(QRectF(13.9, 6.1, 5.8, 5.8), 250 * 16, 210 * 16);
      QPainterPath sh;
      sh.moveTo(16.3, 15.0);
      sh.cubicTo(18.8, 15.4, 20.6, 17.0, 21.2, 19.6);
      p.drawPath(sh);
      break;
    }
    case Icon::Person: {
      p.drawEllipse(QRectF(8.2, 4.2, 7.6, 7.6));
      path.moveTo(5, 20);
      path.cubicTo(5.9, 16.5, 8.6, 14.9, 12, 14.9);
      path.cubicTo(15.4, 14.9, 18.1, 16.5, 19, 20);
      p.drawPath(path);
      break;
    }
    case Icon::Search: {
      p.drawEllipse(QRectF(3.8, 3.8, 12.8, 12.8));
      p.drawLine(QPointF(15.2, 15.2), QPointF(20, 20));
      break;
    }
    case Icon::Gear: {
      // 齿轮（原型 lucide settings 近形）：中孔＋厚短齿（8 向辐射读作齿冠）
      p.drawEllipse(QRectF(8.9, 8.9, 6.2, 6.2));
      QPen tooth = pen;
      tooth.setWidthF(3.0);
      p.setPen(tooth);
      const QPointF c(12, 12);
      for (int k = 0; k < 8; ++k) {
        const qreal a = k * M_PI / 4;
        p.drawLine(c + QPointF(6.2 * std::cos(a), 6.2 * std::sin(a)),
                   c + QPointF(8.2 * std::cos(a), 8.2 * std::sin(a)));
      }
      break;
    }
  }
  p.restore();
}

void RailNav::mousePressEvent(QMouseEvent* event) {
  const int i = item_at(event->pos());
  if (i >= 0 && !items_[i].disabled && !items_[i].id.isEmpty()) {
    emit item_clicked(items_[i].id);
  }
  QWidget::mousePressEvent(event);
}

void RailNav::mouseMoveEvent(QMouseEvent* event) {
  const int i = item_at(event->pos());
  if (i != hover_) {
    hover_ = i;
    update();
    if (i >= 0 && !items_[i].tip.isEmpty()) {
      QToolTip::showText(event->globalPos(), items_[i].tip, this, item_rect(i));
    } else {
      QToolTip::hideText();
    }
  }
  QWidget::mouseMoveEvent(event);
}

void RailNav::leaveEvent(QEvent* event) {
  if (hover_ != -1) {
    hover_ = -1;
    update();
  }
  QWidget::leaveEvent(event);
}

bool RailNav::event(QEvent* event) {
  if (event->type() == QEvent::ToolTip) {
    const QHelpEvent* help = static_cast<QHelpEvent*>(event);
    const int i = item_at(help->pos());
    if (i >= 0 && !items_[i].tip.isEmpty()) {
      QToolTip::showText(help->globalPos(), items_[i].tip, this, item_rect(i));
      return true;
    }
  }
  return QWidget::event(event);
}

}  // namespace memex::client
