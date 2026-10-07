// R24-3 群密码箱窗口：显式解锁（箱密码本地派生，永不触服务端）＋掩码
// 列表（只露名称/账号）＋查看/复制=显式动作且服务端留痕＋条目维护＋
// 箱管理（改箱密码=重包同 DEK/重置箱=新 DEK 全量重加密/授权名单/审计）。
// 独立文件面会话（与 R23-3 文件助手同构：协作面同源账号、文件面独立端口；
// 权限按服务端裁决——建箱/维护=群主/管理员、删恒归群主/管理员、名单仅群主）。
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <QtGlobal>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace memex::client {

class FilesClient;

class GroupVaultDialog : public QDialog {
  Q_OBJECT
 public:
  explicit GroupVaultDialog(QWidget* parent = nullptr);

  // 连接文件面（连接按钮与测试共用同一入口）
  void connect_to(const QString& host, quint16 files_port,
                  const QString& account, const QString& password);
  bool is_connected() const;
  // 切目标群（懒建复用同一窗口时换群：清列表、清钥匙——换群即锁，
  // 显式解锁语义不跨群残留）
  void set_group(quint64 gid, const QString& group_name);

  // —— 程序化入口（测试共用）——
  // 建箱（仅未建箱时；600k 轮＋随机盐/DEK，本地全派生）
  bool init_vault(const QString& password);
  // 显式解锁：本地派生 KEK 解包裹（错口令不发声——不发任何请求）
  bool unlock(const QString& password);
  // 新建/保存条目（editing_id_==0 新建、>0 保存修改；密文本地加密后上传）
  bool submit_entry(const QString& name, const QString& account_name,
                    const QString& password, const QString& url,
                    const QString& note);
  // 选中条目进编辑态（access 留痕取回明文预填；再进一次＝取消）
  bool edit_selected();
  // 查看（action=reveal 留痕→解密→详情区）
  bool reveal_selected();
  // 复制密码（action=copy 留痕→解密→系统剪贴板）
  bool copy_selected();
  bool delete_selected();
  // 重拉箱状态＋条目列表
  void refresh();
  // 改箱密码（同 DEK 重包：新盐+新包裹块覆盖，条目密文不动）
  bool change_vault_password(const QString& old_pass, const QString& new_pass);
  // 重置箱（新 DEK 全量重加密：逐条取回明文重加密落回，再覆盖包裹块）
  bool reset_vault(const QString& old_pass, const QString& new_pass);
  // 授权名单（仅群主；空=恢复全成员）
  bool set_acl(const QStringList& accounts);
  // 打开审计窗（非模态；倒序）
  void open_audit();

  // —— 走查/测试观察点 ——
  QString status_text() const;
  int stream_count() const;
  QListWidget* stream() const { return stream_; }
  bool vault_exists() const { return exists_; }
  bool unlocked() const { return state_ == State::kUnlocked; }
  // 查看详情区现文（reveal 落这里；测试断言解密结果）
  QString details_text() const;
  // 剪贴板现文（copy 落这里）
  QString clipboard_text() const;
  // 审计窗当前条数（未开过/已关＝-1）
  int audit_count() const;
  QListWidget* audit_list() const { return audit_list_; }

 private:
  enum class State { kUnknown, kNoVault, kLocked, kUnlocked };
  // 条目明文（JSON{password,url,note} 打包/解包）
  static QString pack_secret(const QString& password, const QString& url,
                             const QString& note);
  static bool unpack_secret(const QString& plaintext, QString* password,
                            QString* url, QString* note);

  void build_ui();
  void apply_state();
  void set_status(const QString& text, bool error = false);
  void end_edit();
  // 名称/账号/密码/URL/备注五字段编辑子对话框（手工新建/编辑共用）
  bool entry_dialog(QString* name, QString* account_name, QString* pass,
                    QString* url, QString* note, const QString& init_name,
                    const QString& init_account, const QString& init_pass,
                    const QString& init_url, const QString& init_note);

  FilesClient* client_;
  quint64 gid_{0};
  State state_{State::kUnknown};
  bool exists_{false};
  // 箱材料（info 回包缓存；unlock 后 KEK/DEK 只在内存）
  QString kdf_salt_;
  int kdf_iters_{0};
  QString wrapped_;
  QStringList acl_;
  QByteArray kek_;
  QByteArray dek_;
  QString unlocked_salt_; // 解锁时对应的盐（info 刷新后判断是否仍解锁）

  QLineEdit* host_;
  QLineEdit* port_;
  QLineEdit* account_;
  QLineEdit* password_;
  QLineEdit* vault_pass_;
  QLabel* status_;
  QLabel* details_;
  QListWidget* stream_;
  QPushButton* btn_connect_;
  QPushButton* btn_init_;
  QPushButton* btn_unlock_;
  QPushButton* btn_new_;
  QPushButton* btn_edit_;
  QPushButton* btn_reveal_;
  QPushButton* btn_copy_;
  QPushButton* btn_delete_;
  QPushButton* btn_refresh_;
  QPushButton* btn_pass_;
  QPushButton* btn_reset_;
  QPushButton* btn_acl_;
  QPushButton* btn_audit_;
  qint64 editing_id_{0}; // 0=新建；>0=正在改的条目 id
  QString pending_action_; // 在途 access 语义：reveal|copy|edit|reset
  bool edit_manual_{false}; // 手工编辑路径（btn_edit_ 取回明文后弹对话框）
  // 编辑态预填种子（access(edit) 回包解密后落这里，submit_entry 取走）
  QString seed_name_, seed_account_, seed_password_, seed_url_, seed_note_;
  bool reset_pending_{false}; // 重置箱全量重加密进行中
  QStringList reset_queue_;   // 待重加密条目 id（reset 模式）
  int reset_saved_{0};        // 已落回条数（计数满→覆盖包裹块）
  int reset_total_{0};
  QString new_salt_; // 重置/改密在途的新材料（rekey 回包后生效）
  int new_iters_{0};
  QByteArray new_dek_;
  QByteArray new_kek_;
  // 审计子对话框列表（非模态存活期观察点；关闭置空）
  QListWidget* audit_list_{nullptr};
};

} // namespace memex::client
