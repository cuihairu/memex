// Memex 主窗口（T1.4 直连态界面）：
// 局域网设备列表（替代联系人）+ 聊天窗 + 「未归档」本地态标记 + 空态引导。
// 直连引擎承载全部会话；协作引擎未登录只待机（登录界面按阶段 2 接入）。
#pragma once

#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMap>
#include <QPushButton>
#include <QTextBrowser>

#include <engine/direct/direct_engine.hpp>
#include <engine/collab/collab_engine.hpp>

namespace memex::client {

class MainWindow : public QMainWindow {
public:
  explicit MainWindow(QWidget* parent = nullptr);

private:
  void build_ui();
  void wire_engines();
  void refresh_devices();
  void open_peer(const QString& device_id);
  void append_message(const QString& from_id, const QString& text,
                      qint64 ts_ms, bool outgoing);
  void append_system_line(const QString& text);
  void show_guidance();
  void show_status(const QString& text);

  QString current_peer_; // 当前会话对端设备标识（空＝未选中）
  QMap<QString, quint64> file_sent_; // 文件名 → 最近一次进度字节（节流）
  bool chat_showing_guidance_{false}; // 聊天区当前是否为引导态
  QString status_hint_;               // 状态栏事件提示（引擎态前缀实时拼）

  DirectEngine direct_engine_;
  CollabEngine collab_engine_;

  QLabel* device_count_{nullptr};
  QLineEdit* search_box_{nullptr};
  QListWidget* device_list_{nullptr};
  QLabel* chat_title_{nullptr};
  QLabel* chat_meta_{nullptr};
  QTextBrowser* chat_view_{nullptr};
  QLineEdit* input_box_{nullptr};
  QPushButton* send_btn_{nullptr};
};

} // namespace memex::client
