// 品牌物料客户端面（设计稿 docs/design/品牌物料.md §4）：拉取服务器品牌
// 配置并应用（界面标题/登录窗/侧栏/窗口图标随换）——读面免鉴权
// GET（QNetworkAccessManager 直拉，不依赖文件面登录）；本地缓存+版本号
// （变更热生效，离线用缓存）；未配置＝memex 默认标兜底（没配就不变）。
// 可实例化（测试直构局部实例）＋instance() 应用级便捷单例。
#pragma once

#include <QColor>
#include <QIcon>
#include <QObject>
#include <QPixmap>
#include <QString>

class QNetworkAccessManager;

namespace memex::client {

class BrandKit : public QObject {
  Q_OBJECT
 public:
  explicit BrandKit(QObject* parent = nullptr);

  // 应用级单例（主窗/登录窗走这里；测试可直构局部实例隔离状态）
  static BrandKit& instance();

  // —— 当前品牌状态（空/无效＝未配置＝memex 默认标兜底）——
  const QString& company_name() const { return company_name_; }
  const QColor& accent() const { return accent_; }
  const QString& slogan() const { return slogan_; }
  const QPixmap& logo() const { return logo_; }
  const QPixmap& splash() const { return splash_; }
  qint64 version() const { return version_; }
  // 有牌＝公司名/logo/accent/slogan 任一已配
  bool branded() const;

  // 拉取服务器品牌配置（免鉴权 GET /files/branding；version 与本地不同
  // 再拉 PNG 字节面）。成功应用后发 brand_applied 并落缓存；失败静默
  // （文件面未启用/离线＝保持现状，用缓存或默认标）。
  void fetch(const QString& host, quint16 files_port);

  // 启动时先行：从本地缓存恢复（离线也有牌）。回 true=缓存命中。
  bool load_cache();

  // —— 应用面取值（主窗/登录窗组装用；默认标兜底在此收口）——
  // 窗口标题：公司名非空＝公司名，否则 "Memex"
  QString window_title() const;
  // 窗口/托盘图标：logo 非空＝QIcon(logo)，否则 memex 默认标（qrc 多尺寸）
  QIcon window_icon() const;

  // 缓存目录（QStandardPaths::AppCacheLocation/brand；测试可注入私有目录）
  void set_cache_dir(const QString& dir) { cache_dir_ = dir; }

 signals:
  // 拉取应用完成（含回默认标）——主窗接此信号自行换牌（标题/图标/侧栏行）
  void brand_applied();

 private:
  void apply_from_server(const QJsonObject& meta);
  void save_cache();
  QPixmap load_png(const QString& file) const;
  const QString& ensure_cache_dir() const; // 空＝AppCacheLocation/brand 缺省

  QNetworkAccessManager* nam_ = nullptr;
  QString base_url_; // http://host:port（fetch 时记，PNG 跟随拉取用）
  QString company_name_;
  QColor accent_;
  QString slogan_;
  QPixmap logo_;
  QPixmap splash_;
  qint64 version_ = -1; // -1=未拉过（load_cache 成功后为缓存版本）
  mutable QString cache_dir_; // 空＝AppCacheLocation/brand 缺省（惰性初始化）
};

} // namespace memex::client
