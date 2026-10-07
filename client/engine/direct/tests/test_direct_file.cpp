// T1.3 验收：双实例直连文件传输——
// ① ≥100MB 文件传输，进度单调推进，落地整文件 SHA-256 与源一致（A10）；
// ② 多层目录传输，落地结构与逐文件哈希一致；
// ③ 中断续传：取消后接收侧保留 .memex-part，重发从偏移续传，终态哈希一致；
// 全程无服务端进程参与。
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>

#include <algorithm>
#include <functional>

#include <engine/direct/direct_engine.hpp>

using memex::client::DirectEngine;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      qCritical("FAIL %s:%d %s", __FILE__, __LINE__, #cond);                 \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

bool wait_until(const std::function<bool()>& cond, int timeout_ms) {
  QElapsedTimer timer;
  timer.start();
  while (!cond()) {
    if (timer.elapsed() > timeout_ms) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    QThread::msleep(5);
  }
  return true;
}

QString file_sha256(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash h(QCryptographicHash::Sha256);
  while (!f.atEnd()) h.addData(f.read(1 << 20));
  return QString::fromLatin1(h.result().toHex());
}

// 生成 size 字节的可复现伪随机内容（非纯重复模式）
bool make_file(const QString& path, qint64 size) {
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) return false;
  const qint64 kBlock = 1 << 20;
  QByteArray block(kBlock, 0);
  quint64 seed = 0x9E3779B97F4A7C15ull;
  for (qint64 off = 0; off < size; off += kBlock) {
    const int n = static_cast<int>(std::min<qint64>(kBlock, size - off));
    for (int i = 0; i < n; ++i) {
      seed = seed * 6364136223846793005ull + 1442695040888963407ull;
      block[i] = char((seed >> 33) & 0xFF);
    }
    if (f.write(block.constData(), n) != n) return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  // 独立 UDP 发现口：并发测试进程互不串扰（生产默认口 2425 不受影响）
  {
    QUdpSocket probe;
    if (probe.bind(QHostAddress::AnyIPv4, 0,
                   QAbstractSocket::ShareAddress |
                       QAbstractSocket::ReuseAddressHint)) {
      qputenv("MEMEX_TEST_DISCOVERY_PORT",
              QByteArray::number(probe.localPort()));
      probe.close();
    }
  }
  QCoreApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("memex-test"));
  QCoreApplication::setApplicationName(QStringLiteral("direct-file-test"));

  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  const QString db_a = tmp.filePath(QStringLiteral("a.db"));
  const QString db_b = tmp.filePath(QStringLiteral("b.db"));
  const QString dl_b = tmp.filePath(QStringLiteral("dl-b"));
  QDir().mkpath(dl_b);

  DirectEngine a("dev-A", db_a);
  DirectEngine b("dev-B", db_b);
  b.set_download_dir(dl_b);

  // 发送侧终态（每相开始前复位）
  bool fin_done = false;
  bool fin_ok = false;
  // 接收侧观察
  QString got_path;
  int received_count = 0;
  quint64 last_done = 0;
  bool progress_monotonic = true;
  // 相位③：进度到阈值即取消（句柄在 send 之后填入，闭包按引用读取）
  bool cancel_armed = false;
  bool cancel_fired = false;
  quint64 cancel_seen = 0;
  std::string cancel_tid;

  QObject::connect(&a, &DirectEngine::file_finished, &a,
                   [&](const QString&, bool ok, const QString&) {
                     fin_done = true;
                     fin_ok = ok;
                   });
  QObject::connect(&b, &DirectEngine::file_received, &b,
                   [&](const QString&, const QString& path) {
                     got_path = path;
                     ++received_count;
                   });
  QObject::connect(&b, &DirectEngine::file_progress, &b,
                   [&](const QString&, quint64 done, quint64) {
                     if (done < last_done) progress_monotonic = false;
                     last_done = done;
                     if (cancel_armed && !cancel_fired &&
                         done >= 4u * 1024 * 1024) {
                       cancel_fired = true;
                       cancel_seen = done;
                       a.cancel_transfer(cancel_tid);
                     }
                   });

  CHECK(a.start());
  CHECK(b.start());
  CHECK(wait_until([&] { return a.has_peer("dev-B") && b.has_peer("dev-A"); },
                   8000));

  // ── ① 120MB 文件（压过 100MB 线）────────────────────────────────
  const QString src_big = tmp.filePath(QStringLiteral("big.bin"));
  constexpr qint64 kBigSize = 120 * 1024 * 1024;
  CHECK(make_file(src_big, kBigSize));
  const QString big_sha = file_sha256(src_big);
  CHECK(!big_sha.isEmpty());

  fin_done = false;
  got_path.clear();
  last_done = 0;
  const std::string big_tid = a.send_file("dev-B", src_big);
  CHECK(!big_tid.empty());
  CHECK(wait_until([&] { return fin_done && !got_path.isEmpty(); }, 180000));
  CHECK(fin_ok);
  CHECK(progress_monotonic);
  CHECK(got_path == QDir(dl_b).filePath(QStringLiteral("big.bin")));
  CHECK(file_sha256(got_path) == big_sha);
  CHECK(last_done == static_cast<quint64>(kBigSize));

  // ── ② 中断续传（48MB，接收 4MB 处取消）─────────────────────────
  const QString src_mid = tmp.filePath(QStringLiteral("mid.bin"));
  constexpr qint64 kMidSize = 48 * 1024 * 1024;
  CHECK(make_file(src_mid, kMidSize));
  const QString mid_sha = file_sha256(src_mid);

  fin_done = false;
  fin_ok = false;
  got_path.clear();
  cancel_fired = false;
  cancel_seen = 0;
  cancel_armed = true;
  const std::string mid_tid = a.send_file("dev-B", src_mid);
  cancel_tid = mid_tid;
  CHECK(!mid_tid.empty());

  bool receiver_failed = false;
  QObject::connect(&b, &DirectEngine::file_finished, &b,
                   [&](const QString&, bool ok, const QString&) {
                     if (!ok) receiver_failed = true;
                   });

  CHECK(wait_until([&] { return cancel_fired; }, 30000));
  CHECK(wait_until([&] { return fin_done && receiver_failed; }, 30000));
  CHECK(!fin_ok); // 首程被中止
  cancel_armed = false;

  // 接收侧保留部分文件（0 < part < 全长）
  const QString mid_part =
      QDir(dl_b).filePath(QStringLiteral("mid.bin.memex-part"));
  CHECK(wait_until([&] { return QFile::exists(mid_part); }, 5000));
  const qint64 part_size = QFileInfo(mid_part).size();
  CHECK(part_size > 0);
  CHECK(part_size < kMidSize);
  CHECK(cancel_seen > 0);

  // 重发：从偏移续传，终态哈希一致，部分文件落地后消失
  fin_done = false;
  fin_ok = false;
  got_path.clear();
  last_done = 0;
  progress_monotonic = true;
  quint64 resume_start = 0;
  bool resume_start_seen = false;
  QObject::connect(&b, &DirectEngine::file_progress, &b,
                   [&](const QString&, quint64 done, quint64) {
                     if (!resume_start_seen) {
                       resume_start = done;
                       resume_start_seen = true;
                     }
                   });
  const std::string mid_tid2 = a.send_file("dev-B", src_mid);
  CHECK(!mid_tid2.empty());
  CHECK(wait_until([&] { return fin_done && !got_path.isEmpty(); }, 180000));
  CHECK(fin_ok);
  CHECK(last_done == static_cast<quint64>(kMidSize));
  if (file_sha256(got_path) != mid_sha) {
    qint64 diff_at = -1;
    int diff_count = 0;
    QFile fs(src_mid), fd(got_path);
    if (fs.open(QIODevice::ReadOnly) && fd.open(QIODevice::ReadOnly)) {
      constexpr qint64 kBuf = 1 << 20;
      QByteArray bs, bd;
      qint64 off = 0;
      while (!fs.atEnd() && !fd.atEnd()) {
        bs = fs.read(kBuf);
        bd = fd.read(kBuf);
        const qsizetype n = std::min(bs.size(), bd.size());
        for (qsizetype i = 0; i < n; ++i) {
          if (bs[i] != bd[i]) {
            if (diff_at < 0) diff_at = off + i;
            ++diff_count;
          }
        }
        off += n;
      }
    }
    qWarning() << "DBG 续传不符：src" << mid_sha << "dst"
               << file_sha256(got_path)
               << "首差字节" << diff_at << "差异字节数" << diff_count
               << "续传起点" << resume_start;
    {
      QFile fs(src_mid), fd(got_path);
      if (fs.open(QIODevice::ReadOnly) && fd.open(QIODevice::ReadOnly)) {
        qWarning() << "DBG got_path =" << got_path
                   << "src头32" << fs.read(32).toHex()
                   << "dst头32" << fd.read(32).toHex();
        fs.seek(resume_start);
        fd.seek(resume_start);
        qWarning() << "DBG src@起点" << fs.read(16).toHex()
                   << "dst@起点" << fd.read(16).toHex();
      }
    }
  }
  CHECK(file_sha256(got_path) == mid_sha);
  CHECK(!QFile::exists(mid_part));

  // ── ③ 多层目录 ─────────────────────────────────────────────────
  const QString src_dir = tmp.filePath(QStringLiteral("docs-src"));
  CHECK(QDir().mkpath(src_dir));
  struct Spec {
    const char* rel;
    qint64 size;
  };
  const Spec specs[] = {
      {"a.txt", 1024},
      {"sub1/b.txt", 4096},
      {"sub1/sub2/c.txt", 128},
      {"sub1/sub2/d.bin", 512 * 1024},
      {"sub3/e.txt", 2048},
      {"sub3/sub4/f.bin", 1024 * 1024},
  };
  for (const Spec& s : specs) {
    const QString rel = QString::fromLatin1(s.rel);
    QDir().mkpath(QFileInfo(QDir(src_dir).filePath(rel)).absolutePath());
    CHECK(make_file(QDir(src_dir).filePath(rel), s.size));
  }

  fin_done = false;
  got_path.clear();
  received_count = 0;
  bool dir_done = false;
  bool dir_ok = false;
  QObject::connect(&a, &DirectEngine::directory_finished, &a,
                   [&](const QString&, bool ok) {
                     dir_done = true;
                     dir_ok = ok;
                   });

  const QString job_id = a.send_directory("dev-B", src_dir);
  CHECK(!job_id.isEmpty());
  CHECK(wait_until([&] { return dir_done; }, 180000));
  CHECK(dir_ok);
  CHECK(received_count == 6);

  // 落地结构逐文件哈希比对
  for (const Spec& s : specs) {
    const QString rel = QString::fromLatin1(s.rel);
    const QString src = QDir(src_dir).filePath(rel);
    const QString dst = QDir(dl_b).filePath(rel);
    CHECK(QFile::exists(dst));
    CHECK(file_sha256(src) == file_sha256(dst));
  }

  // —— 平台-10 文件旁路授权门（stub 授权面：裁决由测试驱动）——
  // 进度/终态/取消全程以授权关联号贯穿；拒＝零字节零终态错误理由
  {
    CHECK(make_file(tmp.filePath(QStringLiteral("gate.bin")), 64 * 1024));
    const QString gate_src = tmp.filePath(QStringLiteral("gate.bin"));
    quint64 last_req = 0;
    a.set_file_authorizer([&](const memex::client::FileAuthzRequest& r) {
      last_req = r.req;
    });

    // ① 拒：终态 false 且接收侧零动静
    fin_done = false;
    const std::string deny_id = a.send_file("dev-B", gate_src);
    CHECK(!deny_id.empty());
    CHECK(deny_id == std::to_string(last_req)); // 返回值＝关联号
    CHECK(!fin_done); // 裁决前不起传不终态
    a.file_authz_resolved(last_req, false, "deny:cross-department");
    CHECK(fin_done && !fin_ok);
    CHECK(received_count == 6);

    // ② 允：起传落地，关联号即传输 ID
    fin_done = false;
    const std::string allow_id = a.send_file("dev-B", gate_src);
    CHECK(allow_id == std::to_string(last_req));
    a.file_authz_resolved(last_req, true, {});
    CHECK(wait_until([&] { return fin_done; }, 30000));
    CHECK(fin_ok);
    CHECK(got_path.endsWith(QStringLiteral("gate.bin")));
    CHECK(file_sha256(gate_src) == file_sha256(got_path));

    // ③ 待决撤单：裁决前取消即本地终态失败；迟到裁决不复活已撤单
    fin_done = false;
    const std::string cancel_id = a.send_file("dev-B", gate_src);
    CHECK(!cancel_id.empty());
    a.cancel_transfer(cancel_id);
    CHECK(fin_done && !fin_ok);
    fin_done = false;
    a.file_authz_resolved(last_req, true, {}); // pending 已清，此单无效
    CHECK(!fin_done);

    // ④ 目录作业拒：directory_finished(false)，逐文件零发送
    const QString gdir = tmp.filePath(QStringLiteral("gate-dir"));
    QDir().mkpath(gdir);
    CHECK(make_file(QDir(gdir).filePath(QStringLiteral("g1.bin")), 4096));
    CHECK(make_file(QDir(gdir).filePath(QStringLiteral("g2.bin")), 8192));
    received_count = 0;
    dir_done = false;
    dir_ok = true;
    const QString gjid = a.send_directory("dev-B", gdir);
    CHECK(gjid == QString::number(last_req));
    a.file_authz_resolved(last_req, false, "deny:forward-forbidden");
    CHECK(dir_done && !dir_ok);
    CHECK(received_count == 0);

    // ⑤ 目录作业允：关联号即作业 ID，逐文件串行落地
    dir_done = false;
    const QString gjid2 = a.send_directory("dev-B", gdir);
    CHECK(gjid2 == QString::number(last_req));
    a.file_authz_resolved(last_req, true, {});
    CHECK(wait_until([&] { return dir_done; }, 60000));
    CHECK(dir_ok);
    CHECK(received_count == 2);
    CHECK(file_sha256(QDir(gdir).filePath("g1.bin")) ==
          file_sha256(QDir(dl_b).filePath("g1.bin")));
    CHECK(file_sha256(QDir(gdir).filePath("g2.bin")) ==
          file_sha256(QDir(dl_b).filePath("g2.bin")));

    a.set_file_authorizer({}); // 摘除授权面（后续无腿）
  }

  a.stop();
  b.stop();

  if (g_failures == 0) {
    qInfo("direct file tests: all passed");
    return 0;
  }
  qCritical("direct file tests: %d failure(s)", g_failures);
  return 1;
}
