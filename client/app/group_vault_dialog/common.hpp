// group_vault_dialog.cpp 拆分共性面（LAY-017 自 group_vault_dialog.cpp 逐字搬移）：include 块与匿名空间 KDF 常量/时间工具（inline 化）。
#pragma once

#include "../group_vault_dialog.hpp"
#include "../collab_defaults.hpp"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QVBoxLayout>

#include <engine/collab/files_client.hpp>

#include "../group_vault_crypto.hpp"

namespace memex::client {

// 列表条目角色：id（审计窗同构取值）
inline constexpr int kRoleId = Qt::UserRole + 2;
// 建箱/重包裹 KDF 迭代数（wingman 口径 600k；服务端下限 10000）
inline constexpr int kKdfIters = 600000;

inline QString fmt_time(qint64 ms) {
  return QDateTime::fromMSecsSinceEpoch(ms)
      .toString(QStringLiteral("MM-dd HH:mm"));
}
} // namespace memex::client
