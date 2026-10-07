// R24-3 群密码箱窗口冒烟：真服务端进程 × 离屏 QDialog。
// 先走加密自证（PBKDF2/GCM 往返＋错口令拒＋篡改拒）→ 群主建箱→解锁→
// 条目加密存取→列表掩码（不露密文）→查看解密→复制留痕→编辑重加密→
// 改箱密码（同 DEK 重包）→重置箱（新 DEK 全量重加密）→审计窗；成员腿：
// 默认全成员可解锁、名单收窄后 info 403、恢复后回落。权限 403 全链与
// b64 存取面走 test_files_client 引擎级腿，不在此重复。
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>

#include <app/group_vault_crypto.hpp>
#include <app/group_vault_dialog.hpp>
#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>

using memex::client::CollabEngine;
using memex::client::GroupVaultDialog;
using memex::client::LocalStore;

#ifndef MEMEX_SERVER_BIN
#error "MEMEX_SERVER_BIN 未定义（应传入 $<TARGET_FILE:memex_server>）"
#endif

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
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    QThread::msleep(5);
  }
  return cond();
}

quint16 free_port() {
  QTcpServer probe;
  probe.listen(QHostAddress::LocalHost, 0);
  const quint16 port = probe.serverPort();
  probe.close();
  return port;
}

// 加密面自证：派生确定性、包/解往返、错口令拒、条目加解往返、篡改拒
void crypto_selfcheck() {
  using namespace memex::vault;
  const QString salt = random_b64(16);
  CHECK(!salt.isEmpty());
  const QByteArray kek = derive_kek(QStringLiteral("box-pw"), salt, 600000);
  CHECK(kek.size() == 32);
  CHECK(kek == derive_kek(QStringLiteral("box-pw"), salt, 600000)); // 确定性
  CHECK(kek != derive_kek(QStringLiteral("box-pw2"), salt, 600000));

  const QByteArray dek = QByteArray::fromBase64(random_b64(32).toLatin1());
  CHECK(dek.size() == 32);
  const QString wrapped = wrap_dek(kek, dek);
  CHECK(!wrapped.isEmpty());
  CHECK(dek == unwrap_dek(kek, wrapped)); // 包/解往返
  CHECK(unwrap_dek(derive_kek(QStringLiteral("错口令"), salt, 600000),
                   wrapped)
            .isEmpty()); // 错口令 GCM 验签拒

  QString nonce;
  const QString ct = encrypt_secret(dek, QStringLiteral("S3cret!123"), &nonce);
  CHECK(!ct.isEmpty() && !nonce.isEmpty());
  CHECK(decrypt_secret(dek, ct, nonce) == QStringLiteral("S3cret!123"));
  // 篡改拒：密文首字节翻位→GCM 验签拒
  QByteArray tampered = QByteArray::fromBase64(ct.toLatin1());
  tampered[0] = static_cast<char>(tampered[0] ^ 0x01);
  CHECK(decrypt_secret(dek, QString::fromLatin1(tampered.toBase64()), nonce)
            .isEmpty());
}

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  const QString db = tmp.filePath(QStringLiteral("srv.db"));
  const QString server_bin = QStringLiteral(MEMEX_SERVER_BIN);

  crypto_selfcheck();

  for (const auto& row :
       {std::pair<QString, QString>{QStringLiteral("alice"),
                                    QStringLiteral("pass-1")},
        std::pair<QString, QString>{QStringLiteral("bob"),
                                    QStringLiteral("pass-2")}}) {
    CHECK(QProcess::execute(
              server_bin,
              {QStringLiteral("account"), QStringLiteral("add"), row.first,
               row.second, QStringLiteral("--db"), db}) == 0);
  }

  const quint16 collab_port = free_port();
  const quint16 files_port = free_port();
  QProcess server;
  server.setProcessChannelMode(QProcess::ForwardedChannels);
  server.start(server_bin,
               {QStringLiteral("serve"), QStringLiteral("--db"), db,
                QStringLiteral("--port"), QString::number(collab_port),
                QStringLiteral("--webhook-port"), QStringLiteral("0"),
                QStringLiteral("--files-port"), QString::number(files_port)});
  CHECK(server.waitForStarted(5000));
  CHECK(wait_until([&] {
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHost, files_port)
               ? (probe.close(), false)
               : true;
  }, 8000));

  // 建群（alice 建，拉 bob）：group_result 回执取 gid（顶层 connect）
  LocalStore store_ce;
  CHECK(store_ce.open(tmp.filePath(QStringLiteral("ce.db"))));
  CollabEngine ce;
  ce.attach_store(&store_ce);
  bool ce_in = false;
  quint64 gid = 0;
  QObject::connect(&ce, &CollabEngine::logged_in, &ce,
                   [&](const QString&, const QString&) { ce_in = true; });
  QObject::connect(&ce, &CollabEngine::group_result, &ce,
                   [&](bool ok, const QString&, const QString& op, quint64 id) {
                     if (ok && op == QStringLiteral("create")) gid = id;
                   });
  ce.login(QStringLiteral("127.0.0.1"), collab_port, QStringLiteral("alice"),
           QStringLiteral("pass-1"));
  CHECK(wait_until([&] { return ce_in; }, 8000));
  ce.create_group(QStringLiteral("密码箱测试群"), {QStringLiteral("bob")});
  CHECK(wait_until([&] { return gid > 0; }, 8000));

  // —— 群主腿：建箱→解锁→条目→查看/复制→改密→重置→审计 ——
  GroupVaultDialog owner;
  owner.connect_to(QStringLiteral("127.0.0.1"), files_port,
                   QStringLiteral("alice"), QStringLiteral("wrong"));
  CHECK(wait_until(
      [&] { return owner.status_text().contains(QStringLiteral("连接失败")); },
      8000));
  owner.connect_to(QStringLiteral("127.0.0.1"), files_port,
                   QStringLiteral("alice"), QStringLiteral("pass-1"));
  CHECK(wait_until([&] {
    return owner.is_connected() &&
           owner.status_text().contains(QStringLiteral("已连接"));
  }, 8000));
  owner.set_group(gid, QStringLiteral("密码箱测试群"));
  CHECK(owner.windowTitle().contains(QStringLiteral("密码箱测试群")));
  // 未建箱：空态占位＋exists=false
  CHECK(wait_until([&] { return owner.stream_count() == 1; }, 8000));
  CHECK(!owner.vault_exists());
  CHECK(!owner.unlocked());

  // 解锁未建箱拒；建箱→自动解锁
  CHECK(!owner.unlock(QStringLiteral("box-pw")));
  CHECK(owner.init_vault(QStringLiteral("box-pw")));
  CHECK(wait_until([&] { return owner.unlocked(); }, 8000));
  CHECK(owner.vault_exists());

  // 错箱密码拒（本地验签，不发请求）
  CHECK(!owner.unlock(QStringLiteral("wrong-box-pw")));
  CHECK(owner.status_text().contains(QStringLiteral("箱密码错误")));

  // 新建条目：列表落地且掩码（露名称/账号，不露密码）
  CHECK(owner.submit_entry(QStringLiteral("生产库WiFi"), QStringLiteral("wifi-ops"),
                           QStringLiteral("S3cret!123"),
                           QStringLiteral("https://wifi.corp"),
                           QStringLiteral("访客网络")));
  CHECK(wait_until([&] {
    return owner.stream_count() == 1 && owner.stream()->item(0) != nullptr &&
           owner.stream()->item(0)->text().contains(
               QStringLiteral("生产库WiFi")) &&
           owner.stream()->item(0)->text().contains(
               QStringLiteral("wifi-ops"));
  }, 8000));
  CHECK(!owner.stream()->item(0)->text().contains(QStringLiteral("S3cret")));

  // 查看：access 留痕→本地解密→详情区落明文
  owner.stream()->setCurrentRow(0);
  CHECK(owner.reveal_selected());
  CHECK(wait_until([&] {
    return owner.details_text().contains(QStringLiteral("S3cret!123"));
  }, 8000));
  CHECK(owner.details_text().contains(QStringLiteral("wifi-ops")));

  // 复制：显式动作留痕→剪贴板落密码
  CHECK(owner.copy_selected());
  CHECK(wait_until([&] {
    return owner.clipboard_text() == QStringLiteral("S3cret!123");
  }, 8000));

  // 编辑：取回明文预填（引擎腿已验留痕面）→新密码重加密落回
  CHECK(owner.edit_selected());
  CHECK(wait_until([&] {
    return owner.status_text().contains(QStringLiteral("正在编辑条目 #"));
  }, 8000));
  CHECK(owner.submit_entry(QStringLiteral("生产库WiFi"), QStringLiteral("wifi-ops"),
                           QStringLiteral("NewPass456"),
                           QStringLiteral("https://wifi.corp"),
                           QStringLiteral("访客网络")));
  CHECK(wait_until([&] {
    return owner.status_text().contains(QStringLiteral("条目已保存"));
  }, 8000));
  owner.stream()->setCurrentRow(0);
  CHECK(owner.reveal_selected());
  CHECK(wait_until([&] {
    return owner.details_text().contains(QStringLiteral("NewPass456")) &&
           !owner.details_text().contains(QStringLiteral("S3cret"));
  }, 8000));

  // 改箱密码：同 DEK 重包（旧口令验签→新包裹块）；新口令可解、旧口令拒
  CHECK(owner.change_vault_password(QStringLiteral("box-pw"),
                                    QStringLiteral("box-pw2")));
  CHECK(wait_until([&] {
    return owner.status_text().contains(QStringLiteral("箱密码已更改"));
  }, 8000));
  CHECK(owner.unlocked());
  CHECK(!owner.unlock(QStringLiteral("box-pw"))); // 旧口令：GCM 验签拒
  CHECK(owner.unlock(QStringLiteral("box-pw2")));

  // 重置箱：新 DEK 全量重加密（逐条留痕取回重加密落回→覆盖包裹块）
  CHECK(owner.reset_vault(QStringLiteral("box-pw2"), QStringLiteral("box-pw3")));
  CHECK(wait_until([&] {
    return owner.status_text().contains(QStringLiteral("重置完成"));
  }, 15000));
  owner.stream()->setCurrentRow(0);
  CHECK(owner.reveal_selected());
  CHECK(wait_until([&] {
    return owner.details_text().contains(QStringLiteral("NewPass456"));
  }, 8000)); // 新 DEK 解旧明文——重加密确实换了钥匙又没丢内容

  // 审计窗：非模态填充（reveal/copy/重置取回均落痕）
  owner.open_audit();
  CHECK(wait_until([&] { return owner.audit_count() > 0; }, 8000));
  CHECK(owner.audit_list() != nullptr);

  // —— 成员腿：默认全成员可解锁；名单收窄后 403；恢复后回落 ——
  GroupVaultDialog member;
  member.connect_to(QStringLiteral("127.0.0.1"), files_port,
                    QStringLiteral("bob"), QStringLiteral("pass-2"));
  CHECK(wait_until([&] { return member.is_connected(); }, 8000));
  member.set_group(gid, QStringLiteral("密码箱测试群"));
  CHECK(wait_until([&] { return member.vault_exists(); }, 8000));
  CHECK(!member.unlocked());
  CHECK(member.stream_count() == 0); // 未解锁不拉列表（掩码面也先锁）
  CHECK(member.unlock(QStringLiteral("box-pw3")));
  CHECK(wait_until([&] { return member.stream_count() == 1; }, 8000));
  member.stream()->setCurrentRow(0);
  CHECK(member.reveal_selected());
  CHECK(wait_until([&] {
    return member.details_text().contains(QStringLiteral("NewPass456"));
  }, 8000));

  // 群主收窄名单（bob 出列）→ bob info 403（解锁资格随名单失）
  CHECK(owner.set_acl({QStringLiteral("carol")}));
  CHECK(wait_until([&] {
    return owner.status_text().contains(QStringLiteral("授权名单已更新"));
  }, 8000));
  member.refresh();
  CHECK(wait_until([&] {
    return member.status_text().contains(QStringLiteral("操作失败"));
  }, 8000));
  // 恢复全成员（空名单）→ bob 回落可读
  CHECK(owner.set_acl({}));
  CHECK(wait_until([&] {
    return owner.status_text().contains(QStringLiteral("授权名单已更新"));
  }, 8000));
  member.refresh();
  CHECK(wait_until([&] {
    return member.status_text().contains(QStringLiteral("共 1 条"));
  }, 8000));

  server.terminate();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    qInfo("group vault dialog tests: all passed");
    return 0;
  }
  qCritical("group vault dialog tests: %d failure(s)", g_failures);
  return 1;
}
