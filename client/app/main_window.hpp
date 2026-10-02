// Memex 主窗口（T0.4 骨架）：空窗 + 双引擎状态。
#pragma once

#include <QMainWindow>

#include <engine/direct/direct_engine.hpp>
#include <engine/collab/collab_engine.hpp>

namespace memex::client {

class MainWindow : public QMainWindow {
public:
  explicit MainWindow(QWidget* parent = nullptr);

private:
  DirectEngine direct_engine_;
  CollabEngine collab_engine_;
};

} // namespace memex::client
