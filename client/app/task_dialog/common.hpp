// task_dialog 拆分共性面（LAY-012 自 task_dialog.cpp 头部逐字搬移）：
// include 块供各域模块引用；方法声明仍在 task_dialog.hpp。
#pragma once

#include "../task_dialog.hpp"
#include "../collab_defaults.hpp"

#include <QCalendarWidget>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QGroupBox>
#include <QGridLayout>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QSplitter>
#include <QTextCharFormat>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <QtAlgorithms>

#include "engine/collab/files_client.hpp"
#include "engine/task/dingtalk_provider.hpp"
#include "engine/task/feishu_provider.hpp"
#include "engine/task/github_provider.hpp"
#include "engine/task/task_http.hpp"
#include "../notify_center.hpp"
#include "../task_provider_settings.hpp"
#include "../task_provider_store.hpp"
#include "../task_template_settings.hpp"
