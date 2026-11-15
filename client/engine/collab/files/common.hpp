// files_client 共性面（LAY-007 自 files_client.cpp 匿名空间逐字搬移）。
// 超时常量 inline 化（多模块共用同一份定义）；公共头见 files_client.hpp。
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QUrl>
#include <QUrlQuery>

namespace memex::client {

inline constexpr int kMetaTimeoutMs = 15000;              // 元数据面 15s
inline constexpr int kByteTimeoutMs = 10 * 60 * 1000;     // 字节面 512MiB 上限给足

} // namespace memex::client
