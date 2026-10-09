// R27-3 外部任务 provider 凭据存储（加密落盘）：口令派生 KEK（PBKDF2
// 600000 轮，与密码箱同水位）→ AES-256-GCM 加密整包 JSON（QSettings 落
// salt/iters/nonce/ct 四键）。口令与明文只存会话内存，落盘全程密文；
// 解锁态换口令=新盐重包裹。单例供设置面与 TaskDialog 共用；测试以
// QSettings::setPath(INI) 重定向隔离（test_theme 同法）。
// 加密原语复用 R24-3 密码箱件（memex::vault）。
#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

namespace memex::client {

class TaskProviderStore : public QObject {
  Q_OBJECT
 public:
  static TaskProviderStore& instance();
  // org/app 显式给定（QSettings 惯例同 QSettings("memex","collab")）；
  // 测试以独立 org 名隔离，绝不碰真配置
  explicit TaskProviderStore(
      QString org = QStringLiteral("memex"),
      QString app = QStringLiteral("task-providers"),
      QObject* parent = nullptr);
  ~TaskProviderStore() override;
  TaskProviderStore(const TaskProviderStore&) = delete;
  TaskProviderStore& operator=(const TaskProviderStore&) = delete;

  // 落盘是否已有加密包（与解锁与否无关）
  bool has_store() const;
  // 会话内存是否已解包（解锁态才可读写配置）
  bool is_unlocked() const { return unlocked_; }
  // 首建：无落盘包才建（已有=false）；建后即解锁态（空配置）
  bool create(const QString& passphrase);
  // 解包：口令错/包损坏=false（GCM tag 校验即口令校验）
  bool unlock(const QString& passphrase);
  // 解锁态换口令：新盐重派生重加密；未解锁=false
  bool change_passphrase(const QString& next);
  // 上锁：清会话内存（配置与 KEK 不留）
  void lock();

  // —— 解锁态读写；未解锁一律空读/拒写 ——
  QStringList providers() const;
  bool contains(const QString& provider_id) const;
  QJsonObject config(const QString& provider_id) const; // 未配=空对象
  void save(const QString& provider_id, const QJsonObject& fields);
  void remove(const QString& provider_id);

 signals:
  // 配置增删或换口令后发出（设置面↔TaskDialog 联动刷新）
  void changed();

 private:
  bool persist(); // 整包加密落盘（解锁态）
  void reset_memory();

  QString org_;
  QString app_;
  bool has_store_{false};
  bool unlocked_{false};
  QByteArray kek_;
  QHash<QString, QJsonObject> configs_;
};

} // namespace memex::client
