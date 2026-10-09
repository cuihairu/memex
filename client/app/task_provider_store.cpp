#include "task_provider_store.hpp"

#include <QJsonDocument>
#include <QSettings>

#include "group_vault_crypto.hpp"

namespace memex::client {
namespace {
constexpr int kPbkdf2Iters = 600000; // 与密码箱同水位（wingman 口径）
constexpr const char* kGroup = "task-providers";
} // namespace

TaskProviderStore& TaskProviderStore::instance() {
  static TaskProviderStore store;
  return store;
}

TaskProviderStore::TaskProviderStore(QString org, QString app, QObject* parent)
    : QObject(parent), org_(std::move(org)), app_(std::move(app)) {
  QSettings s(org_, app_);
  s.beginGroup(QLatin1String(kGroup));
  has_store_ = s.contains(QStringLiteral("ct"));
}

TaskProviderStore::~TaskProviderStore() = default;

bool TaskProviderStore::has_store() const { return has_store_; }

bool TaskProviderStore::create(const QString& passphrase) {
  if (has_store_ || passphrase.isEmpty()) return false;
  QSettings s(org_, app_);
  s.beginGroup(QLatin1String(kGroup));
  const QString salt = vault::random_b64(16);
  if (salt.isEmpty()) return false;
  kek_ = vault::derive_kek(passphrase, salt, kPbkdf2Iters);
  if (kek_.isEmpty()) return false;
  s.setValue(QStringLiteral("salt"), salt);
  s.setValue(QStringLiteral("iters"), kPbkdf2Iters);
  has_store_ = true;
  unlocked_ = true; // 首建即解锁态（KEK 已在手，无需 reset_memory 擦除）
  configs_.clear();
  return persist();
}

bool TaskProviderStore::unlock(const QString& passphrase) {
  if (!has_store_ || passphrase.isEmpty()) return false;
  if (unlocked_) return true; // 已解锁幂等
  QSettings s(org_, app_);
  s.beginGroup(QLatin1String(kGroup));
  const QString salt = s.value(QStringLiteral("salt")).toString();
  const int iters = s.value(QStringLiteral("iters")).toInt();
  const QString ct = s.value(QStringLiteral("ct")).toString();
  const QString nonce = s.value(QStringLiteral("nonce")).toString();
  if (salt.isEmpty() || ct.isEmpty() || nonce.isEmpty() || iters < 10000) {
    return false; // 落盘包残缺：当作口令不对处理（不静默重置）
  }
  const QByteArray kek = vault::derive_kek(passphrase, salt, iters);
  const QString plain = vault::decrypt_secret(kek, ct, nonce);
  if (plain.isEmpty()) return false; // tag 校验不过=口令错
  const QJsonDocument doc = QJsonDocument::fromJson(plain.toUtf8());
  const QJsonObject providers =
      doc.object().value(QStringLiteral("providers")).toObject();
  for (auto it = providers.begin(); it != providers.end(); ++it) {
    configs_.insert(it.key(), it.value().toObject());
  }
  kek_ = kek;
  unlocked_ = true;
  return true;
}

bool TaskProviderStore::change_passphrase(const QString& next) {
  if (!unlocked_ || next.isEmpty()) return false;
  QSettings s(org_, app_);
  s.beginGroup(QLatin1String(kGroup));
  const QString salt = vault::random_b64(16);
  if (salt.isEmpty()) return false;
  const QByteArray kek = vault::derive_kek(next, salt, kPbkdf2Iters);
  if (kek.isEmpty()) return false;
  s.setValue(QStringLiteral("salt"), salt);
  s.setValue(QStringLiteral("iters"), kPbkdf2Iters);
  kek_ = kek;
  return persist(); // 新 KEK 下整包重加密
}

void TaskProviderStore::lock() {
  reset_memory();
  emit changed();
}

bool TaskProviderStore::persist() {
  if (!unlocked_) return false;
  QJsonObject providers;
  for (auto it = configs_.constBegin(); it != configs_.constEnd(); ++it) {
    providers.insert(it.key(), it.value());
  }
  QJsonObject root;
  root.insert(QStringLiteral("providers"), providers);
  QString nonce;
  const QString ct =
      vault::encrypt_secret(kek_, QString::fromUtf8(
                                    QJsonDocument(root).toJson(
                                        QJsonDocument::Compact)),
                            &nonce);
  if (ct.isEmpty() || nonce.isEmpty()) return false;
  QSettings s(org_, app_);
  s.beginGroup(QLatin1String(kGroup));
  s.setValue(QStringLiteral("ct"), ct);
  s.setValue(QStringLiteral("nonce"), nonce);
  return true;
}

void TaskProviderStore::reset_memory() {
  kek_.fill(0);
  kek_.clear();
  configs_.clear();
  unlocked_ = false;
}

QStringList TaskProviderStore::providers() const {
  return unlocked_ ? QStringList(configs_.keyBegin(), configs_.keyEnd())
                   : QStringList();
}

bool TaskProviderStore::contains(const QString& provider_id) const {
  return unlocked_ && configs_.contains(provider_id);
}

QJsonObject TaskProviderStore::config(const QString& provider_id) const {
  return unlocked_ ? configs_.value(provider_id) : QJsonObject();
}

void TaskProviderStore::save(const QString& provider_id,
                             const QJsonObject& fields) {
  if (!unlocked_ || provider_id.isEmpty()) return;
  configs_.insert(provider_id, fields);
  if (persist()) emit changed();
}

void TaskProviderStore::remove(const QString& provider_id) {
  if (!unlocked_ || !configs_.contains(provider_id)) return;
  configs_.remove(provider_id);
  if (persist()) emit changed();
}

} // namespace memex::client
