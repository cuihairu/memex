#include <app/net_guard.hpp>

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QSettings>

namespace memex::client {

namespace net_guard {

namespace {
quint32 ipv4_of(const QHostAddress& addr, bool* ok) {
  return addr.toIPv4Address(ok);
}
} // namespace

QString validate_cidr(const QString& cidr_in) {
  const QString s = cidr_in.trimmed();
  const int slash = s.indexOf('/');
  if (slash < 0) return QStringLiteral("段须为 a.b.c.d/掩码位（如 192.168.10.0/24）");
  const QStringList parts = s.left(slash).split(QLatin1Char('.'));
  if (parts.size() != 4)
    return QStringLiteral("IP 节数须为 4（点分十进制）");
  for (const QString& p : parts) {
    bool ok = false;
    const int v = p.toInt(&ok);
    if (!ok || p.isEmpty() || v < 0 || v > 255)
      return QStringLiteral("IP 各节须为 0..255 的整数");
  }
  bool len_ok = false;
  const int len = s.mid(slash + 1).toInt(&len_ok);
  if (!len_ok || s.mid(slash + 1).isEmpty() || len < 0 || len > 32)
    return QStringLiteral("掩码位须为 0..32 的整数");
  return QString();
}

bool cidr_matches(const QString& cidr, const QHostAddress& addr) {
  if (!validate_cidr(cidr).isEmpty()) return false; // 坏形态不匹配＝不误伤
  const QString s = cidr.trimmed();
  const int slash = s.indexOf('/');
  QHostAddress net(s.left(slash));
  const int len = s.mid(slash + 1).toInt();
  bool net_ok = false;
  bool addr_ok = false;
  const quint32 net32 = ipv4_of(net, &net_ok);
  const quint32 addr32 = ipv4_of(addr, &addr_ok);
  if (!net_ok || !addr_ok) return false; // 仅 IPv4 参与判定
  if (len == 0) return true;             // /0＝全网段
  const quint32 mask = (len == 32) ? 0xFFFFFFFFu
                                   : (0xFFFFFFFFu << (32 - len));
  return (net32 & mask) == (addr32 & mask);
}

bool blocked(const QStringList& cidrs, const QHostAddress& addr) {
  for (const QString& c : cidrs)
    if (cidr_matches(c, addr)) return true;
  return false;
}

} // namespace net_guard

namespace net_blacklist {

bool enabled() {
  return QSettings().value(QStringLiteral("net/blacklist_enabled"), false)
      .toBool();
}

void set_enabled(bool on) {
  QSettings().setValue(QStringLiteral("net/blacklist_enabled"), on);
}

QStringList entries() {
  return QSettings()
      .value(QStringLiteral("net/blacklist"))
      .toStringList();
}

void set_entries(const QStringList& cidrs) {
  QSettings().setValue(QStringLiteral("net/blacklist"), cidrs);
}

} // namespace net_blacklist

namespace remote_control {

namespace {
QString salt_key() { return QStringLiteral("remote/pwd_salt"); }
QString hash_key() { return QStringLiteral("remote/pwd_hash"); }

QByteArray hash_pwd(const QString& salt, const QString& pwd) {
  return QCryptographicHash::hash((salt + pwd).toUtf8(),
                                  QCryptographicHash::Sha256);
}
} // namespace

bool enabled() {
  return QSettings().value(QStringLiteral("remote/enabled"), false).toBool();
}

void set_enabled(bool on) { QSettings().setValue(QStringLiteral("remote/enabled"), on); }

bool has_password() {
  QSettings s;
  return s.contains(salt_key()) && s.contains(hash_key());
}

bool set_password(const QString& pwd) {
  if (pwd.isEmpty()) return false;
  QString salt;
  for (int i = 0; i < 4; ++i)
    salt += QStringLiteral("%1").arg(QRandomGenerator::system()->generate(),
                                     8, 16, QLatin1Char('0'));
  QSettings s;
  s.setValue(salt_key(), salt);
  s.setValue(hash_key(), QString::fromLatin1(hash_pwd(salt, pwd).toHex()));
  return true;
}

bool verify_password(const QString& pwd) {
  QSettings s;
  if (!has_password()) return false;
  const QString salt = s.value(salt_key()).toString();
  const QString expect = s.value(hash_key()).toString();
  return QString::fromLatin1(hash_pwd(salt, pwd).toHex()) == expect;
}

} // namespace remote_control

} // namespace memex::client
