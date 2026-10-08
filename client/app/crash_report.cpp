#include "crash_report.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <crashpad/client/crashpad_client.h>
#include <crashpad/client/crash_report_database.h>

#include <map>
#include <string>

#ifndef MEMEX_VERSION
#define MEMEX_VERSION "dev"
#endif

namespace memex::client {
namespace {

// handler 探寻：分发体与 crashpad_handler 同目录（构建后拷贝跟随）；开发树
// 兜底 PATH（手工跑构建产物时同目录拷贝已覆盖）。
QString locate_handler() {
  const QString local =
      QCoreApplication::applicationDirPath() + QStringLiteral("/crashpad_handler");
  if (QFileInfo::exists(local)) return local;
  const QString on_path = QStandardPaths::findExecutable(QStringLiteral("crashpad_handler"));
  return on_path;
}

// 本地既有 dump 份数据（new/completed/pending 三区）：与运行日志对时——
// 下次启动即可看到上次崩溃是否真落了盘。
int count_local_dumps(const QString& db_path) {
  int n = 0;
  for (const char* sub : {"new", "completed", "pending"}) {
    const QDir d(db_path + QLatin1Char('/') + QLatin1String(sub));
    n += d.entryList({QStringLiteral("*.dmp")}, QDir::Files).size();
  }
  return n;
}

}  // namespace

bool init_crash_reporting() {
  const QString db_path =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
      QStringLiteral("/crashes");
  if (!QDir().mkpath(db_path)) {
    qWarning("[崩溃采集] dump 目录创建失败：%s（崩溃采集未启用）",
             qUtf8Printable(db_path));
    return false;
  }

  const int prior = count_local_dumps(db_path);
  if (prior > 0) {
    qInfo("[崩溃采集] 本地已有崩溃报告 %d 份（%s/{new,completed,pending}）",
          prior, qUtf8Printable(db_path));
  }

  const QString handler = locate_handler();
  if (handler.isEmpty()) {
    qWarning("[崩溃采集] 未找到 crashpad_handler（应随包与客户端同目录），"
             "崩溃采集未启用");
    return false;
  }

  auto database = crashpad::CrashReportDatabase::Initialize(
      base::FilePath(db_path.toStdString()));
  if (!database) {
    qWarning("[崩溃采集] dump 数据库初始化失败：%s（崩溃采集未启用）",
             qUtf8Printable(db_path));
    return false;
  }

  // handler 常驻至进程结束（函数内静态：client socket 生命周期须覆盖全程）。
  static crashpad::CrashpadClient client;
  const bool started = client.StartHandler(
      base::FilePath(handler.toStdString()),
      base::FilePath(db_path.toStdString()),
      base::FilePath(),  // 指标面：空＝不建
      std::string(),     // 上报 URL：空＝只本地落盘，不外发（数据外发默认关）
      {{"product", "memex-client"}, {"version", MEMEX_VERSION}},
      std::vector<std::string>(),  // handler 附加参数：无
      /*restartable=*/true, /*asynchronous_start=*/false);
  if (!started) {
    qWarning("[崩溃采集] handler 启动失败：%s（崩溃采集未启用）",
             qUtf8Printable(handler));
    return false;
  }
  qInfo("[崩溃采集] Crashpad 已启动：handler=%s dump=%s（只本地落盘不外发）",
        qUtf8Printable(handler), qUtf8Printable(db_path));
  return true;
}

}  // namespace memex::client
