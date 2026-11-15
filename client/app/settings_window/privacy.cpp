// settings_window.cpp 拆分模块（LAY-016）：隐私页（联系人/在线状态/数据与存储）；工厂与公共面见 common.hpp，方法声明在 settings_window.hpp。
#include "common.hpp"

namespace memex::client {

QWidget* SettingsWindow::build_privacy_page() {
  auto* page = new QWidget(this);
  page->setProperty("pageTitle", QStringLiteral("隐私"));
  auto* root = new QVBoxLayout(page);
  root->setContentsMargins(28, 24, 28, 24);
  root->setSpacing(10);

  root->addWidget(page_title_label(QStringLiteral("隐私"), page));
  root->addWidget(page_sub_label(
      QStringLiteral("查看消息可见性与本机数据保护相关选项。"), page));
  root->addSpacing(6);

  // 原型 .tag 口径的静态标签（描边小胶囊，亮暗随令牌重刷）
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

  // —— 组：联系人（黑名单＝网段黑名单真实入口；加好友验证无流程）——
  auto* contacts = make_group(QStringLiteral("联系人"), page);
  auto* c_lay = new QVBoxLayout(contacts);
  c_lay->setContentsMargins(14, 18, 14, 10);

  {
    auto* ctrl = new QWidget(page);
    auto* c = new QHBoxLayout(ctrl);
    c->setContentsMargins(0, 0, 0, 0);
    c->setSpacing(8);
    c->addWidget(
        mk_tag(QStringLiteral("settings_priv_net_tag"),
               QStringLiteral("当前提供网段黑名单")));
    auto* btn = new QPushButton(QStringLiteral("管理网络黑名单"), ctrl);
    btn->setObjectName(QStringLiteral("settings_priv_net"));
    if (win_) {
      connect(btn, &QPushButton::clicked, this,
              [this] { win_->show_net_dialog(); });
    } else {
      btn->setEnabled(false); // 测试隔离（无主窗＝无真实入口可开）
      btn->setToolTip(QStringLiteral("未接入：测试隔离无主窗。"));
    }
    c->addWidget(btn);
    add_setting_row(c_lay, QStringLiteral("黑名单"),
                    QStringLiteral("管理已屏蔽的账号。"), ctrl);
  }

  {
    auto* btn = new QPushButton(QStringLiteral("暂未提供"), page);
    btn->setObjectName(QStringLiteral("settings_priv_friend"));
    btn->setEnabled(false);
    btn->setToolTip(QStringLiteral("未提供：Memex 当前没有好友申请与验证流程。"));
    add_setting_row(c_lay, QStringLiteral("加好友验证"),
                    QStringLiteral("Memex 当前没有好友申请与验证流程。"), btn);
  }

  root->addWidget(contacts);

  // —— 组：在线状态（自动＝协作连接与设备发现驱动，无手动开关）——
  auto* presence = make_group(QStringLiteral("在线状态"), page);
  auto* p_lay = new QVBoxLayout(presence);
  p_lay->setContentsMargins(14, 18, 14, 10);
  {
    auto* ctrl = new QWidget(page);
    auto* c = new QHBoxLayout(ctrl);
    c->setContentsMargins(0, 0, 0, 0);
    c->setSpacing(8);
    c->addWidget(mk_tag(QStringLiteral("settings_priv_online_tag"),
                        QStringLiteral("自动")));
    c->addStretch();
    add_setting_row(p_lay, QStringLiteral("在线状态"),
                    QStringLiteral("在线状态由协作连接与设备发现自动更新。"),
                    ctrl);
  }
  root->addWidget(presence);

  // —— 组：数据与存储（存储说明＝真实本机目录；锁屏＝主窗共用入口）——
  auto* storage = make_group(QStringLiteral("数据与存储"), page);
  auto* s_lay = new QVBoxLayout(storage);
  s_lay->setContentsMargins(14, 18, 14, 10);

  {
    auto* btn = new QPushButton(QStringLiteral("查看存储说明"), page);
    btn->setObjectName(QStringLiteral("settings_priv_storage"));
    connect(btn, &QPushButton::clicked, this, [this, page] {
      const QString base =
          QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
      auto* box = new QMessageBox(page);
      box->setObjectName(QStringLiteral("settings_priv_storage_note"));
      box->setIcon(QMessageBox::Information);
      box->setWindowTitle(QStringLiteral("存储说明"));
      box->setText(QStringLiteral("本机消息数据"));
      box->setInformativeText(QStringLiteral(
          "直连消息：仅保存在双方本机（会话窗口台账，不经服务端）。\n"
          "协作消息：按服务端归档策略处理（全量归档；审计与检索见归档检"
          "索台）。\n\n本机数据目录：\n%1\n（avatars 头像 / brand 品牌缓存 "
          "/ emoji 表情 / sent-shots 已发截图 / crashes 崩溃转储）")
          .arg(base));
      box->setStandardButtons(QMessageBox::Ok);
      box->setModal(false); // 非模态：不阻塞（同页可继续操作）
      box->setAttribute(Qt::WA_DeleteOnClose);
      box->show();
    });
    add_setting_row(s_lay, QStringLiteral("本机消息数据"),
                    QStringLiteral("直连消息保存在本机；协作消息按服务端归档"
                                   "策略处理。"),
                    btn);
  }

  {
    auto* btn = new QPushButton(QStringLiteral("离开与锁屏…"), page);
    btn->setObjectName(QStringLiteral("settings_priv_away"));
    if (win_) {
      connect(btn, &QPushButton::clicked, this,
              [this] { win_->show_away_dialog(); });
    } else {
      btn->setEnabled(false);
      btn->setToolTip(QStringLiteral("未接入：测试隔离无主窗。"));
    }
    add_setting_row(s_lay, QStringLiteral("本机锁屏"),
                    QStringLiteral("离开时可启用密码锁屏；锁屏只显示未读数。"),
                    btn);
  }

  root->addWidget(storage);
  root->addStretch();

  auto* footnote = new QLabel(
      QStringLiteral("原型说明：控件仅展示布局，不代表尚未实现的功能已可操作。"
                     "加好友验证暂未提供。"),
      page);
  footnote->setWordWrap(true);
  footnote->setStyleSheet(QStringLiteral("color:#9AA0A6;font-size:11px;"));
  root->addWidget(footnote);

  return page;
}

} // namespace memex::client
