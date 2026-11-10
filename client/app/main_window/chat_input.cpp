// 输入区增强（LAY-002 拆分）：表情面板与自定义表情导入、文字颜色。

#include "main_window.hpp"
#include "main_window/common.hpp"


#include <QColorDialog>
#include <QFileDialog>
#include <QIcon>
#include <QStandardPaths>
namespace memex::client {

void MainWindow::show_emoji_panel() {
  auto* dlg = new QDialog(this, Qt::Popup);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowTitle(QStringLiteral("表情"));
  auto* grid = new QGridLayout(dlg);
  grid->setContentsMargins(8, 8, 8, 8);
  grid->setHorizontalSpacing(4);
  grid->setVerticalSpacing(4);
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  QStringList builtin;
  builtin << QStringLiteral("😀") << QStringLiteral("😄") << QStringLiteral("😂")
          << QStringLiteral("🙂") << QStringLiteral("😉") << QStringLiteral("😊")
          << QStringLiteral("😍") << QStringLiteral("😘") << QStringLiteral("😎")
          << QStringLiteral("🤔") << QStringLiteral("😅") << QStringLiteral("😢")
          << QStringLiteral("😭") << QStringLiteral("😡") << QStringLiteral("👍")
          << QStringLiteral("👎") << QStringLiteral("👏") << QStringLiteral("🙏")
          << QStringLiteral("💪") << QStringLiteral("🎉") << QStringLiteral("❤️")
          << QStringLiteral("🔥") << QStringLiteral("✅") << QStringLiteral("❌");
  // 常用＝按本地使用频次降序（T4.5）
  std::sort(builtin.begin(), builtin.end(), [&settings](const QString& a, const QString& b) {
    return settings.value(QStringLiteral("emoji_use/") + a, 0).toInt() >
           settings.value(QStringLiteral("emoji_use/") + b, 0).toInt();
  });
  int i = 0;
  for (const QString& e : builtin) {
    auto* b = new QPushButton(e, dlg);
    b->setFixedSize(34, 34);
    b->setStyleSheet(QStringLiteral("QPushButton{border:none;font-size:18px;}"));
    connect(b, &QPushButton::clicked, this, [this, e, dlg] {
      input_box_->insert(e);
      input_box_->setFocus();
      // QSettings 不可拷贝，原实现按引用捕获 show_emoji_panel 的栈对象——
      // 面板常驻期间该引用已悬垂，点内置表情即 UB（BUG-003 排查抓到）；
      // 用时重建（组织/应用名与建面板排序处同口径）
      QSettings settings(QCoreApplication::organizationName(),
                         QCoreApplication::applicationName());
      const QString key = QStringLiteral("emoji_use/") + e;
      settings.setValue(key, settings.value(key, 0).toInt() + 1);
      dlg->close();
    });
    grid->addWidget(b, i / 8, i % 8);
    ++i;
  }
  // 图标组（需求批③）：符号图标插入消息——与表情面板同源（②表情包面板
  // 后续扩展即此面板），简版先行：点击即入输入框光标处，不记频次
  {
    const QStringList icons = {
        QStringLiteral("⚠"),  QStringLiteral("☎"),  QStringLiteral("✉"),
        QStringLiteral("🔒"), QStringLiteral("🔑"), QStringLiteral("📅"),
        QStringLiteral("📌"), QStringLiteral("📎"), QStringLiteral("⏰"),
        QStringLiteral("⭐"), QStringLiteral("💡"), QStringLiteral("🚀"),
        QStringLiteral("➤"),  QStringLiteral("✔"),  QStringLiteral("✘"),
        QStringLiteral("★")};
    const int label_row = (i + 7) / 8; // 空满当前行，标签整行占位
    auto* label =
        new QLabel(QStringLiteral("—— 图标（插入消息） ——"), dlg);
    grid->addWidget(label, label_row, 0, 1, 8);
    int r = label_row + 1;
    int c = 0;
    for (const QString& ic : icons) {
      auto* b = new QPushButton(ic, dlg);
      b->setFixedSize(34, 34);
      b->setStyleSheet(
          QStringLiteral("QPushButton{border:none;font-size:17px;}"));
      connect(b, &QPushButton::clicked, this, [this, ic, dlg] {
        input_box_->insert(ic);
        input_box_->setFocus();
        dlg->close();
      });
      grid->addWidget(b, r, c);
      if (++c >= 8) { c = 0; ++r; }
    }
    i = r * 8 + c; // 自定义表情从新行接排
  }
  // 自定义表情包：emoji 目录（emoji_dir，测试缝可覆盖）下的图片，
  // 点击即按文件通道发送
  const QString dir = emoji_dir();
  QDir().mkpath(dir);
  const QStringList files = QDir(dir).entryList(
      {QStringLiteral("*.png"), QStringLiteral("*.jpg"),
       QStringLiteral("*.jpeg"), QStringLiteral("*.gif")},
      QDir::Files);
  int row = i / 8;
  int col = i % 8;
  for (const QString& f : files) {
    auto* b = new QPushButton(dlg);
    b->setFixedSize(40, 40);
    b->setIcon(QIcon(dir + QLatin1Char('/') + f));
    b->setIconSize(QSize(36, 36));
    const QString path = dir + QLatin1Char('/') + f;
    connect(b, &QPushButton::clicked, this, [this, path, dlg] {
      dlg->close();
      if (current_peer_.isEmpty()) {
        show_status(QStringLiteral("先选择会话再发送自定义表情"));
        return;
      }
      if (current_kind_ == QStringLiteral("group") ||
          current_kind_ == QStringLiteral("dgroup")) {
        show_status(QStringLiteral("群会话暂不支持自定义表情（走文件单聊）"));
        return;
      }
      const std::string tid =
          direct_engine_.send_file(current_peer_.toStdString(), path);
      if (tid.empty()) {
        show_status(QStringLiteral("自定义表情发送失败"));
        return;
      }
      append_system_line(QStringLiteral("[表情] %1")
                             .arg(esc(QFileInfo(path).fileName())));
    });
    grid->addWidget(b, row, col);
    if (++col >= 8) { col = 0; ++row; }
  }
  auto* imp = new QPushButton(QStringLiteral("导入自定义表情…"), dlg);
  connect(imp, &QPushButton::clicked, this, [this, dlg] {
    const QString src = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择表情图片"), QString(),
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.gif)"));
    if (src.isEmpty()) return;
    if (import_emoji(src)) dlg->close(); // 导入失败留下面板＋状态栏报因
  });
  grid->addWidget(imp, row + 1, 0, 1, 4);
  // 需求批② 表情包：云素材管理（上传/清单/删除/下载到本地表情目录）；
  // 收藏随账号走——换机登录即得，下载落 emoji 目录即可在面板发送。
  auto* cloud = new QPushButton(QStringLiteral("云表情包…"), dlg);
  connect(cloud, &QPushButton::clicked, this, [this, dlg] {
    dlg->close();
    EmojiPackDialog d(this, emoji_dir());
    d.exec();
  });
  grid->addWidget(cloud, row + 1, 4, 1, 4);
  // 锚定到「表情」按钮正下方并抬窗激活：Qt::Popup 默认位置交给 WM 摆，
  // 部分环境摆到屏外/父窗后面＝「点表情没反应」（BUG-003）
  if (emoji_btn_) {
    dlg->move(
        emoji_btn_->mapToGlobal(QPoint(0, emoji_btn_->height() + 4)));
  }
  dlg->show();
  dlg->raise();
  dlg->activateWindow();
}

void MainWindow::request_input_color() {
  if (!input_box_->hasSelectedText()) {
    show_status(QStringLiteral("先选中要着色的文字，再点「颜色」"));
    return;
  }
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  const QColor last = QColor(settings.value(QStringLiteral("input_color"))
                                 .toString());
  const QColor picked = QColorDialog::getColor(
      last.isValid() ? last : QColor(QStringLiteral("#e16531")), this,
      QStringLiteral("文字颜色"), QColorDialog::ShowAlphaChannel);
  if (!picked.isValid()) return; // 取消＝无动作
  settings.setValue(QStringLiteral("input_color"), picked.name());
  apply_input_color(picked);
}

void MainWindow::apply_input_color(const QColor& color) {
  if (!input_box_->hasSelectedText()) {
    show_status(QStringLiteral("先选中要着色的文字，再点「颜色」"));
    return;
  }
  const QString marked = QStringLiteral("〔%1〕%2〔/〕")
                             .arg(color.name(), input_box_->selectedText());
  input_box_->insert(marked); // 替换选区（QLineEdit::insert 即替换当前选区）
  input_box_->setFocus();
}

// —— T4.5 自定义表情：目录与导入 ——
QString MainWindow::emoji_dir() {
  // 测试覆盖：MEMEX_TEST_EMOJI_DIR 指向临时目录（与 MEMEX_TEST_AUTOSTART_DIR
  // 同口径），避免污染真实应用数据目录
  const QByteArray override_dir = qgetenv("MEMEX_TEST_EMOJI_DIR");
  if (!override_dir.isEmpty()) return QString::fromUtf8(override_dir);
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
         QStringLiteral("/emoji");
}

bool MainWindow::import_emoji(const QString& src) {
  static const QSet<QString> kExt{QStringLiteral("png"), QStringLiteral("jpg"),
                                  QStringLiteral("jpeg"),
                                  QStringLiteral("gif")};
  const QFileInfo si(src);
  if (!si.exists() || !si.isFile() || !kExt.contains(si.suffix().toLower())) {
    show_status(QStringLiteral("导入失败：不是支持的表情图片（png/jpg/jpeg/gif）"));
    return false;
  }
  const QString dir = emoji_dir();
  if (!QDir().mkpath(dir)) {
    show_status(QStringLiteral("导入失败：无法创建目录 %1").arg(dir));
    return false;
  }
  const QString dst = dir + QLatin1Char('/') + si.fileName();
  if (QFile::exists(dst) && !QFile::remove(dst)) {
    show_status(QStringLiteral("导入失败：无法覆盖同名文件 %1").arg(dst));
    return false;
  }
  if (!QFile::copy(src, dst)) {
    show_status(QStringLiteral("导入失败：无法写入 %1").arg(dir));
    return false;
  }
  show_status(QStringLiteral("已导入表情：%1（重新打开面板可见）")
                  .arg(si.fileName()));
  return true;
}

} // namespace memex::client
