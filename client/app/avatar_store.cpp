#include "avatar_store.hpp"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <cstdint>

namespace memex::client {

namespace {

// 缓存文件名：账号段做文件系统安全替换（账号本是 ASCII 标识，此为兜底）
QString cache_base(const QString& account, qint64 ver, int size) {
  QString safe;
  safe.reserve(account.size());
  for (const QChar c : account) {
    const char16_t u = c.unicode();
    safe.append((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                        (u >= '0' && u <= '9') || u == '-' || u == '_'
                    ? c
                    : QLatin1Char('_'));
  }
  return QStringLiteral("%1_%2_%3.png").arg(safe).arg(ver).arg(size);
}

} // namespace

int default_avatar_index(const QString& account) {
  const QByteArray bytes = account.toUtf8();
  std::uint32_t h = 2166136261u; // FNV-1a 32 位偏移基
  for (unsigned char c : bytes) {
    h ^= c;
    h *= 16777619u; // FNV-1a 32 位质数
  }
  return static_cast<int>(h % kDefaultAvatarCount) + 1;
}

QPixmap default_avatar(const QString& account, int size) {
  return QPixmap(QStringLiteral(":/avatars/avatar-%1.png")
                     .arg(default_avatar_index(account)))
      .scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

QString avatar_cache_dir() {
  const QString over = qEnvironmentVariable("MEMEX_TEST_AVATAR_DIR");
  if (!over.isEmpty()) return over;
  return QDir(QStandardPaths::writableLocation(
                  QStandardPaths::AppDataLocation))
      .filePath(QStringLiteral("avatars"));
}

QPixmap cached_avatar(const QString& account, qint64 ver, int size) {
  if (ver <= 0) return {};
  const QString path =
      QDir(avatar_cache_dir()).filePath(cache_base(account, ver, size));
  if (!QFileInfo::exists(path)) return {};
  QPixmap pm(path);
  if (pm.isNull()) return {}; // 损坏文件视同未命中
  return pm;
}

void store_avatar_cache(const QString& account, qint64 ver, int size,
                        const QPixmap& pixmap) {
  if (ver <= 0 || pixmap.isNull()) return;
  QDir dir(avatar_cache_dir());
  if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) return;
  pixmap.save(dir.filePath(cache_base(account, ver, size)), "PNG");
}

QPixmap avatar_for(const QString& account, qint64 ver, int size) {
  if (ver > 0) {
    const QPixmap hit = cached_avatar(account, ver, size);
    if (!hit.isNull()) return hit;
  }
  return default_avatar(account, size);
}

QString avatar_tooltip_src(const QString& account, qint64 ver, int size) {
  if (ver > 0) {
    const QString path = QDir(avatar_cache_dir())
                             .filePath(cache_base(account, ver, size));
    if (QFileInfo::exists(path)) return path;
  }
  return QStringLiteral(":/avatars/avatar-%1.png")
      .arg(default_avatar_index(account));
}

} // namespace memex::client
