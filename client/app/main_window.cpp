#include "main_window.hpp"

#include <QLabel>
#include <QStatusBar>

namespace memex::client {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("Memex");
  resize(960, 640);

  auto* placeholder = new QLabel("Memex 内网办公即时通讯系统\n\n双引擎骨架：直连态（未登录）+ 协作态（登录）", this);
  placeholder->setAlignment(Qt::AlignCenter);
  setCentralWidget(placeholder);

  // 两套引擎同起：协作引擎未登录只待机，直连引擎默认可用
  direct_engine_.start();
  collab_engine_.start();

  statusBar()->showMessage(QString::fromStdString(
      direct_engine_.status_text() + "　|　" + collab_engine_.status_text()));
}

} // namespace memex::client
