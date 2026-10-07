// 二期·办公室位置图窗口：抽象平面画布（网格＋工位点）＋拖拽式点位
// 编辑（org-admin，松手即保存）＋搜索定位（按账号/工位号过滤高亮）。
// 楼层可见性=本人占用工位所在楼层（同层互见）；can_manage 由服务端
// az 现裁回带，客户端不自判。独立文件面会话（与 R23-3 同构）。
#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QString>

#include <QtGlobal>

class QLabel;
class QLineEdit;
class QPushButton;

namespace memex::client {

class FilesClient;
class OfficeCanvas;

class OfficeMapDialog : public QDialog {
  Q_OBJECT
 public:
  explicit OfficeMapDialog(QWidget* parent = nullptr);

  // 连接文件面（连接按钮与测试共用同一入口）
  void connect_to(const QString& host, quint16 files_port,
                  const QString& account, const QString& password);
  bool is_connected() const;

  // 拉平面（floor 空=重拉当前视图；org-admin 首次=自楼层。带层号=切到
  // 该层查看/编辑——服务端门：非 org-admin 传层被忽略回自楼层）
  void refresh(const QString& floor = QString());
  // —— 程序化入口（测试共用；编辑面仅 can_manage 时真发网）——
  bool add_seat(const QString& floor, const QString& label);
  bool delete_selected();
  // account 空=解绑
  bool bind_selected(const QString& account);
  // 拖拽模拟（选中点位移并保存；can_manage 门与鼠标路径同源）
  bool move_selected(double x, double y);
  // 程序化点选（与鼠标按压同源 hit-test；选中才能删/绑/移）
  bool select_at(double x, double y);
  // 搜索过滤（命中亮、其余淡；空串=全亮）
  void set_search(const QString& q);

  // —— 走查/测试观察点 ——
  QString status_text() const;
  int seat_count() const;
  int visible_count() const; // 当前过滤下的可见工位数（set_search 生效面）
  qint64 selected_id() const;
  QString floor_text() const;
  bool can_manage() const;
  OfficeCanvas* canvas() const { return canvas_; }

 private:
  void build_ui();
  void apply_map(const QJsonObject& map);
  void set_status(const QString& text, bool error = false);

  FilesClient* client_;
  OfficeCanvas* canvas_;
  QLineEdit* search_;
  QLineEdit* new_floor_;
  QLineEdit* new_label_;
  QLineEdit* bind_account_;
  QLabel* status_;
  QLabel* floor_label_;
  QPushButton* btn_connect_;
  QPushButton* btn_add_;
  QPushButton* btn_delete_;
  QPushButton* btn_bind_;
  QPushButton* btn_unbind_;
  QPushButton* btn_refresh_;
  bool can_manage_{false};
  QString view_floor_; // org-admin 的当前编辑视图层（空=自楼层）
};

} // namespace memex::client
