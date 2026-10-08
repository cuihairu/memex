// 崩溃采集验收（Crashpad 真链路）：XDG 隔离目录起真 memex_client，
// MEMEX_CRASH_TEST=1 → 启动 2s 后空指针写入 → Crashpad 独立 handler 接管
// → AppData/crashes 落 .dmp。断言：进程异常退出 + dump 非空落盘；
// 客户端日志（[崩溃采集] 初始化/触发行）合并输出随验收记录打印。
// 符号化还原（dump_syms + minidump_stackwalk）是开发机侧手工步骤，见
// docs/src/guide/crash-reporting.md 实现段——不在本腿断言（避免依赖
// 本机 rust 工具链，CI 门禁只盯 dump 落盘这一环）。
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      qCritical("FAIL %s:%d %s", __FILE__, __LINE__, #cond);                 \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);

  // XDG 全隔离：dump 落临时目录（不碰用户真 AppData），配置/缓存同隔离。
  QTemporaryDir xdg;
  CHECK(xdg.isValid());
  const QString data_home = xdg.path() + QStringLiteral("/data");
  CHECK(QDir().mkpath(data_home));
  CHECK(QDir().mkpath(xdg.path() + QStringLiteral("/config")));

  QProcess p;
  p.setProcessChannelMode(QProcess::MergedChannels);
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
  env.insert(QStringLiteral("MEMEX_CRASH_TEST"), QStringLiteral("1"));
  env.insert(QStringLiteral("XDG_DATA_HOME"), data_home);
  env.insert(QStringLiteral("XDG_CONFIG_HOME"), xdg.path() + QStringLiteral("/config"));
  p.setProcessEnvironment(env);
  p.setProgram(QStringLiteral(MEMEX_CLIENT_BIN));
  p.start();
  CHECK(p.waitForStarted(10000));

  // 等 handler 落盘：崩溃发生在启动 2s 后，dump 落 <data>/…/crashes 任一区
  //（new/completed/pending）。handler 是独立进程，写盘可略晚于客户端死亡，
  // 所以以 dump 出现为准，40s 上界（正常 3~6s）。
  const auto find_dumps = [&]() {
    QStringList found;
    QDirIterator it(data_home, {QStringLiteral("*.dmp")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) found << it.next();
    return found;
  };
  QStringList dumps;
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 40000) {
    dumps = find_dumps();
    if (!dumps.isEmpty()) break;
    QThread::msleep(200);
  }

  // 客户端应在 2s 后崩溃退出（10s 等待上界；没等到＝还活着，后续 CHECK 报）
  const bool finished = p.waitForFinished(10000);
  if (finished) {
    CHECK(p.exitStatus() == QProcess::CrashExit);
  } else {
    p.kill();
    p.waitForFinished(2000);
  }

  // 客户端日志（[崩溃采集] 初始化成功/故意崩溃两行）随验收记录打印
  const QByteArray log = p.readAll();
  if (!log.isEmpty()) std::fputs(log.constData(), stdout);

  CHECK(!dumps.isEmpty());
  for (const QString& d : dumps) {
    const QFileInfo fi(d);
    CHECK(fi.size() > 0);
    std::printf("[dump] %s (%lld bytes)\n", qUtf8Printable(d),
                static_cast<long long>(fi.size()));
  }
  if (g_failures == 0) {
    std::printf("OK 崩溃采集：dump 已落盘，共 %d 份\n",
                static_cast<int>(dumps.size()));
    return 0;
  }
  std::printf("FAIL 崩溃采集：失败 %d 项\n", g_failures);
  return 1;
}
