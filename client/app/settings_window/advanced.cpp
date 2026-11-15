// settings_window.cpp 拆分模块（LAY-016）：高级页（网络/诊断/关于）；工厂与公共面见 common.hpp，方法声明在 settings_window.hpp。
#include "common.hpp"

namespace memex::client {

QWidget* SettingsWindow::build_advanced_page() {
  auto* page = new QWidget(this);
  page->setProperty("pageTitle", QStringLiteral("高级"));
  auto* root = new QVBoxLayout(page);
  root->setContentsMargins(28, 24, 28, 24);
  root->setSpacing(10);

  root->addWidget(page_title_label(QStringLiteral("高级"), page));
  root->addWidget(
      page_sub_label(QStringLiteral("连接、安全过滤与诊断信息。"), page));
  root->addSpacing(6);

  // 原型 .tag 口径的静态标签（描边小胶囊，亮暗随令牌重刷；同隐私页 mk_tag）
  const auto mk_tag = [page](const QString& name, const QString& text) {
    auto* tag = new QLabel(text, page);
    tag->setObjectName(name);
    const auto restyle = [tag]() {
      const auto& t = ThemeManager::instance().tokens();
      tag->setStyleSheet(
          QStringLiteral("color:%1;border:1px solid %2;border-radius:6px;"
                         "padding:3px 8px;font-size:12px;")
              .arg(t.text_muted.name(), t.border.name()));
    };
    restyle();
    connect(&ThemeManager::instance(), &ThemeManager::theme_changed, tag,
            [restyle](const QString&) { restyle(); });
    return tag;
  };

  // —— 组：网络（域名解析覆盖＝连接设置共用入口；黑名单开关＝真实键）——
  auto* net = make_group(QStringLiteral("网络"), page);
  auto* n_lay = new QVBoxLayout(net);
  n_lay->setContentsMargins(14, 18, 14, 10);

  {
    auto* btn = new QPushButton(QStringLiteral("暂未提供"), page);
    btn->setObjectName(QStringLiteral("settings_adv_proxy"));
    btn->setEnabled(false); // 原型自标（无代理服务器配置面）
    btn->setToolTip(QStringLiteral("未提供：当前客户端未提供代理服务器配置。"));
    add_setting_row(n_lay, QStringLiteral("代理"),
                    QStringLiteral("当前客户端未提供代理服务器配置。"), btn);
  }

  {
    auto* btn = new QPushButton(QStringLiteral("连接设置…"), page);
    btn->setObjectName(QStringLiteral("settings_adv_dns"));
    if (win_) {
      connect(btn, &QPushButton::clicked, this,
              [this] { win_->show_conn_settings_dialog(); });
    } else {
      btn->setEnabled(false); // 测试隔离（无主窗＝无真实入口可开）
      btn->setToolTip(QStringLiteral("未接入：测试隔离无主窗。"));
    }
    add_setting_row(n_lay, QStringLiteral("域名解析覆盖"),
                    QStringLiteral("为协作服务配置主机名解析规则。"), btn);
  }

  {
    adv_blacklist_ = new QCheckBox(page);
    adv_blacklist_->setObjectName(QStringLiteral("settings_adv_blacklist"));
    adv_blacklist_->setChecked(net_blacklist::enabled());
    connect(adv_blacklist_, &QCheckBox::toggled, this, [](bool on) {
      net_blacklist::set_enabled(on); // 与网络与远程设置对话框同一真实键
    });
    add_setting_row(n_lay, QStringLiteral("网段黑名单"),
                    QStringLiteral("拦截指定 IPv4 网段的发现与新入站连接。"),
                    adv_blacklist_);
  }

  root->addWidget(net);

  // —— 组：诊断（运行日志：本版不写日志文件，诚实置灰）——
  auto* diag = make_group(QStringLiteral("诊断"), page);
  auto* d_lay = new QVBoxLayout(diag);
  d_lay->setContentsMargins(14, 18, 14, 10);

  {
    auto* btn = new QPushButton(QStringLiteral("暂未提供"), page);
    btn->setObjectName(QStringLiteral("settings_adv_dev"));
    btn->setEnabled(false); // 原型自标（无开发者选项面板）
    btn->setToolTip(QStringLiteral("未提供：当前未提供独立的开发者选项面板。"));
    add_setting_row(d_lay, QStringLiteral("开发者选项"),
                    QStringLiteral("当前未提供独立的开发者选项面板。"), btn);
  }

  {
    auto* btn = new QPushButton(QStringLiteral("暂未提供"), page);
    btn->setObjectName(QStringLiteral("settings_adv_log"));
    btn->setEnabled(false); // 客户端不写运行日志文件（崩溃转储另有目录）
    btn->setToolTip(
        QStringLiteral("未提供：当前版本不写运行日志文件（崩溃转储见隐私·"
                       "存储说明）。"));
    add_setting_row(d_lay, QStringLiteral("运行日志"),
                    QStringLiteral("当前版本不写运行日志文件；崩溃转储见隐私·"
                                   "存储说明。"),
                    btn);
  }

  root->addWidget(diag);

  // —— 组：关于（版本标签列真实安装版本；关于走主窗共用入口）——
  auto* about = make_group(QStringLiteral("关于"), page);
  auto* a_lay = new QVBoxLayout(about);
  a_lay->setContentsMargins(14, 18, 14, 10);

  {
    auto* ctrl = new QWidget(page);
    auto* c = new QHBoxLayout(ctrl);
    c->setContentsMargins(0, 0, 0, 0);
    c->setSpacing(8);
    c->addWidget(mk_tag(QStringLiteral("settings_adv_ver_tag"),
                        QStringLiteral(MEMEX_VERSION)));
    c->addStretch();
    add_setting_row(a_lay, QStringLiteral("版本"),
                    QStringLiteral("Memex 桌面客户端。"), ctrl);
  }

  {
    auto* btn = new QPushButton(QStringLiteral("关于 Memex…"), page);
    btn->setObjectName(QStringLiteral("settings_adv_about"));
    if (win_) {
      connect(btn, &QPushButton::clicked, this,
              [this] { win_->show_about(); });
    } else {
      btn->setEnabled(false);
      btn->setToolTip(QStringLiteral("未接入：测试隔离无主窗。"));
    }
    add_setting_row(a_lay, QStringLiteral("关于 Memex"),
                    QStringLiteral("查看品牌与版本信息。"), btn);
  }

  root->addWidget(about);
  root->addStretch();

  auto* footnote = new QLabel(
      QStringLiteral("原型说明：控件仅展示布局，不代表尚未实现的功能已可操作。"
                     "代理、开发者选项与运行日志暂未提供。"),
      page);
  footnote->setWordWrap(true);
  footnote->setStyleSheet(QStringLiteral("color:#9AA0A6;font-size:11px;"));
  root->addWidget(footnote);

  return page;
}


} // namespace memex::client
