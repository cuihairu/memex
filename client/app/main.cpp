// Memex 客户端入口（T0.4 骨架）。
// --smoke：烟测模式，起窗后自动退出（ctest 用，offscreen 平台）。
#include <QApplication>
#include <QTimer>

#include <iostream>
#include <string_view>

#include "main_window.hpp"

#ifndef MEMEX_VERSION
#define MEMEX_VERSION "dev"
#endif

int main(int argc, char** argv) {
  if (argc > 1 && std::string_view(argv[1]) == "--version") {
    std::cout << "memex-client " << MEMEX_VERSION << std::endl;
    return 0;
  }

  QApplication app(argc, argv);
  QApplication::setApplicationName("Memex");
  QApplication::setOrganizationName("memex");

  memex::client::MainWindow window;
  window.show();

  if (argc > 1 && std::string_view(argv[1]) == "--smoke") {
    QTimer::singleShot(500, &app, &QApplication::quit);
  }

  return QApplication::exec();
}
