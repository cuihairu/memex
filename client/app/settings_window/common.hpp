// settings_window.cpp 拆分共性面（LAY-016 自 settings_window.cpp 逐字搬移）：include 块与匿名空间 4 个控件工厂（inline 化）。
#pragma once

#include "../settings_window.hpp"

#include "../avatar_store.hpp"
#include "../main_window.hpp"
#include "../net_guard.hpp"
#include "../notify_prefs.hpp"
#include "../shortcut_settings.hpp"
#include "../theme.hpp"

#include <QBitmap>
#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QTime>
#include <QTimeEdit>
#include <QVBoxLayout>

namespace memex::client {


// 页面标题（原型 .page-title：20px 700）
inline QLabel* page_title_label(const QString& text, QWidget* parent) {
  auto* l = new QLabel(text, parent);
  QFont f = l->font();
  f.setPointSize(15);
  f.setBold(true);
  l->setFont(f);
  return l;
}

// 页面副题（原型 .page-sub：text-2 12px）
inline QLabel* page_sub_label(const QString& text, QWidget* parent) {
  auto* l = new QLabel(text, parent);
  l->setStyleSheet(QStringLiteral("color:#80868B;font-size:12px;"));
  l->setWordWrap(true);
  return l;
}

// 原型 .group：面板底＋描边＋组头。描边/圆角/标题色走应用主题 QGroupBox
// 规则（%border%/%radius%/%text_muted% 令牌，亮暗自适应，不硬编码浅色）。
inline QGroupBox* make_group(const QString& title, QWidget* page) {
  return new QGroupBox(title, page);
}

// 原型 .setting-row：标题 b（13px）＋副题 small（11.5px text-2）＋右端控件。
// control 置灰＝未实现（诚实降级）。
inline void add_setting_row(QVBoxLayout* group, const QString& name,
                     const QString& sub, QWidget* control) {
  auto* row = new QHBoxLayout;
  auto* copy = new QVBoxLayout;
  copy->setSpacing(2);
  auto* b = new QLabel(name, group->parentWidget());
  QFont bf = b->font();
  bf.setPointSize(10);
  bf.setBold(true);
  b->setFont(bf);
  copy->addWidget(b);
  auto* small = new QLabel(sub, group->parentWidget());
  small->setStyleSheet(QStringLiteral("color:#80868B;font-size:11px;"));
  small->setWordWrap(true);
  copy->addWidget(small);
  row->addLayout(copy, 1);
  row->addWidget(control, 0, Qt::AlignVCenter);
  group->addLayout(row);
  group->addSpacing(6);
}

} // namespace memex::client
