#include "brand_kit.hpp"

#include <QDir>
#include <QFile>
#include <QFont>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QStandardPaths>

#include <memory>

namespace memex::client {
namespace {

// 默认标兜底（未配置＝memex 自己的标）：qrc 多尺寸优先，资源缺失（裁剪
// 构建）回落程序绘制示意——与主窗 brand_icon() 同源口径
QIcon default_icon() {
  QIcon icon;
  for (int s : {16, 32, 48, 64, 128, 256}) {
    QPixmap pm(QStringLiteral(":/icons/memex-%1.png").arg(s));
    if (!pm.isNull()) icon.addPixmap(pm);
  }
  if (!icon.availableSizes().isEmpty()) return icon;
  QPixmap pm(64, 64);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setBrush(QColor(QStringLiteral("#e16531")));
  p.setPen(Qt::NoPen);
  p.drawRoundedRect(pm.rect().adjusted(2, 2, -2, -2), 14, 14);
  p.setPen(Qt::white);
  QFont f = p.font();
  f.setPixelSize(38);
  f.setBold(true);
  p.setFont(f);
  p.drawText(pm.rect(), Qt::AlignCenter, QStringLiteral("M"));
  return QIcon(pm);
}

QByteArray read_all(const QString& path) {
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

BrandKit::BrandKit(QObject* parent) : QObject(parent) {}

BrandKit& BrandKit::instance() {
  static BrandKit kit;
  return kit;
}

const QString& BrandKit::ensure_cache_dir() const {
  if (cache_dir_.isEmpty()) {
    // 测试覆写（同 emoji 缓存先例）：隔离真实用户缓存
    const QByteArray override_dir = qgetenv("MEMEX_TEST_BRAND_DIR");
    if (!override_dir.isEmpty()) {
      cache_dir_ = QString::fromUtf8(override_dir);
    } else {
      cache_dir_ = QStandardPaths::writableLocation(
                       QStandardPaths::AppDataLocation) +
                   QStringLiteral("/brand");
    }
  }
  return cache_dir_;
}

bool BrandKit::branded() const {
  return !company_name_.isEmpty() || !logo_.isNull() || accent_.isValid() ||
         !slogan_.isEmpty();
}

QString BrandKit::window_title() const {
  return company_name_.isEmpty() ? QStringLiteral("Memex") : company_name_;
}

QIcon BrandKit::window_icon() const {
  return logo_.isNull() ? default_icon() : QIcon(logo_);
}

void BrandKit::fetch(const QString& host, quint16 files_port) {
  if (!nam_) nam_ = new QNetworkAccessManager(this);
  base_url_ = QStringLiteral("http://%1:%2")
                  .arg(host, QString::number(files_port));
  auto* reply = nam_->get(QNetworkRequest(
      QUrl(base_url_ + QStringLiteral("/files/branding"))));
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    reply->deleteLater();
    // 失败静默（文件面未启用/离线＝保持现状，用缓存或默认标）
    if (reply->error() != QNetworkReply::NoError) return;
    const auto doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) return;
    apply_from_server(doc.object());
  });
}

void BrandKit::apply_from_server(const QJsonObject& meta) {
  company_name_ = meta.value(QStringLiteral("company_name")).toString();
  slogan_ = meta.value(QStringLiteral("slogan")).toString();
  const QString accent_s = meta.value(QStringLiteral("accent")).toString();
  accent_ = accent_s.isEmpty() ? QColor() : QColor(accent_s);
  version_ = static_cast<qint64>(
      meta.value(QStringLiteral("version")).toDouble(0));
  // PNG 字节面跟随拉取（meta 只带 has_* 标志）；未配＝清本地位图回落默认标
  logo_ = QPixmap();
  splash_ = QPixmap();
  const QStringList assets{
      meta.value(QStringLiteral("has_logo")).toBool() ? QStringLiteral("logo")
                                                      : QString(),
      meta.value(QStringLiteral("has_splash")).toBool()
          ? QStringLiteral("splash")
          : QString()};
  // 计数挂 shared_ptr（lambda 异步回悬垂栈变量＝崩溃；shared 计数随
  // lambda 存活）
  const auto pending = std::make_shared<int>(0);
  for (const QString& a : assets) {
    if (!a.isEmpty()) ++*pending;
  }
  if (*pending == 0) {
    save_cache();
    emit brand_applied();
    return;
  }
  for (const QString& a : assets) {
    if (a.isEmpty()) continue;
    auto* reply = nam_->get(QNetworkRequest(QUrl(
        base_url_ + QStringLiteral("/files/branding/") + a)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, a, pending] {
      reply->deleteLater();
      if (reply->error() == QNetworkReply::NoError) {
        QPixmap pm;
        pm.loadFromData(reply->readAll(), "PNG");
        if (a == QStringLiteral("logo")) {
          logo_ = pm;
        } else {
          splash_ = pm;
        }
      }
      if (--*pending == 0) {
        save_cache();
        emit brand_applied();
      }
    });
  }
}

bool BrandKit::load_cache() {
  const QByteArray meta =
      read_all(ensure_cache_dir() + QStringLiteral("/brand.json"));
  if (meta.isEmpty()) return false;
  const auto doc = QJsonDocument::fromJson(meta);
  if (!doc.isObject()) return false;
  const auto o = doc.object();
  company_name_ = o.value(QStringLiteral("company_name")).toString();
  slogan_ = o.value(QStringLiteral("slogan")).toString();
  const QString accent_s = o.value(QStringLiteral("accent")).toString();
  accent_ = accent_s.isEmpty() ? QColor() : QColor(accent_s);
  version_ =
      static_cast<qint64>(o.value(QStringLiteral("version")).toDouble(-1));
  logo_ = load_png(QStringLiteral("logo.png"));
  splash_ = load_png(QStringLiteral("splash.png"));
  emit brand_applied();
  return true;
}

QPixmap BrandKit::load_png(const QString& file) const {
  QPixmap pm;
  pm.load(ensure_cache_dir() + QLatin1Char('/') + file);
  return pm;
}

void BrandKit::save_cache() {
  const QString& dir = ensure_cache_dir();
  QDir().mkpath(dir);
  QJsonObject o;
  o.insert(QStringLiteral("company_name"), company_name_);
  o.insert(QStringLiteral("accent"),
           accent_.isValid() ? accent_.name(QColor::HexRgb) : QString());
  o.insert(QStringLiteral("slogan"), slogan_);
  o.insert(QStringLiteral("version"), static_cast<double>(version_));
  QFile f(dir + QStringLiteral("/brand.json"));
  if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    f.write(QJsonDocument(o).toJson());
  }
  logo_.save(dir + QStringLiteral("/logo.png"), "PNG");
  splash_.save(dir + QStringLiteral("/splash.png"), "PNG");
}

} // namespace memex::client
