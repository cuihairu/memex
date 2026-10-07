// R23-3 文件助手客户端面：FileServer HTTP API 的 Qt 封装。
// 会话令牌内存持有（Bearer，服务端 12h TTL）；所有字节面（上传/下载）
// 同面走 FileServer——客户端永不直连对象存储（设计铁律 3）。
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

#include <QtGlobal>

class QNetworkAccessManager;

namespace memex::client {

class FilesClient : public QObject {
  Q_OBJECT
 public:
  explicit FilesClient(QObject* parent = nullptr);
  ~FilesClient() override;

  bool is_logged_in() const { return !token_.isEmpty(); }
  const QString& account() const { return account_; }

  // 换令牌：POST /files/session（与协作面同源账号口令，服务端口独立）
  void login(const QString& host, quint16 files_port,
             const QString& account, const QString& password);
  void logout();

  // —— 备忘录（/files/memo）——
  void create_memo(const QString& content);
  void update_memo(qint64 id, const QString& content);
  void delete_memo(qint64 id);
  // 无 id 列表（updated_ms 倒序）；带 id 单条回 memo_fetched
  void list_memos();
  void fetch_memo(qint64 id);

  // —— 群备忘录（R24-2 /files/group-memo）——
  // 列表（updated_ms 倒序）；q 非空＝标题/正文子串搜索（URL 百分号编码）
  void list_group_memos(quint64 gid, const QString& q = QString());
  // id=0 新建；>0 修改既有条目（服务端落修订笔）
  void save_group_memo(quint64 gid, const QString& title,
                       const QString& content, qint64 id = 0);
  // 删除（恒归群主/管理员；连带修订史）
  void delete_group_memo(quint64 gid, qint64 id);
  // 修订史（倒序：最新笔在前）
  void group_memo_history(qint64 id);
  // 回滚＝一次编辑（取目标笔全文落新笔）
  void rollback_group_memo(qint64 id, qint64 revision_id);
  // 开放全员编辑开关（仅群主/管理员）
  void set_group_memo_open_edit(quint64 gid, bool open);

  // —— 群密码箱（R24-3 /files/group-vault）——
  // 客户端只见 b64 密文与包裹块（全程密文；派生/加解密在 group_vault_crypto）
  // 箱状态（exists=false=未建箱；exists=true 带 kdf_salt/kdf_iters/
  // wrapped_dek/acl）
  void group_vault_info(quint64 gid);
  // 建箱（仅群主/管理员；重复 409；iters 服务端校验 ≥10000）
  void init_group_vault(quint64 gid, const QString& kdf_salt, int kdf_iters,
                        const QString& wrapped_dek);
  // 换箱密码/重置箱：覆盖包裹块（客户端先重包 DEK 或换新 DEK 再调）
  void rekey_group_vault(quint64 gid, const QString& kdf_salt, int kdf_iters,
                         const QString& wrapped_dek);
  // 条目掩码列表（updated_ms 倒序；不带 secret_*——密文只经 access 留痕）
  void list_group_vault_entries(quint64 gid);
  // 取条目密文（action=reveal|copy；服务端每访落审计）
  void access_group_vault_entry(quint64 gid, qint64 id,
                                const QString& action);
  // 建改条目（id=0 新建；>0 改；密文已在客户端备好）
  void save_group_vault_entry(quint64 gid, const QString& name,
                              const QString& account_name,
                              const QString& secret_ct,
                              const QString& secret_nonce, qint64 id = 0);
  // 删条目（恒归群主/管理员）
  void delete_group_vault_entry(quint64 gid, qint64 id);
  // 授权名单（仅群主；空=恢复全成员）
  void set_group_vault_acl(quint64 gid, const QStringList& accounts);
  // 访问审计（仅群主/管理员；倒序）
  void group_vault_audit(quint64 gid);

  // —— 群工具白名单（R25-1 /files/group-tools/config；仅群主/管理员）——
  // upsert 工具动作清单（CI/CD 等具体工具的「开放哪些动作」面）
  void set_tool_actions(quint64 gid, const QString& tool,
                        const QStringList& actions);

  // —— 群 CI/CD 工具（R25-2 /files/group-ci，落在 R25-1 白名单框架上）——
  // 流水线定义 upsert／删（remove=true 走 op=delete；仅群主/管理员）
  void ci_set_pipeline(quint64 gid, const QString& name,
                       const QString& description, bool remove = false);
  // 红绿灯列表（成员；每流水线带 last_status/last_actor/last_ts_ms，
  // 无键=未跑过）
  void ci_list(quint64 gid);
  // 触发（成员；须 ci/trigger 在 R25-1 工具白名单；params 可选）
  void ci_trigger(quint64 gid, const QString& pipeline,
                  const QJsonObject& params = {});
  // run 历史（成员；id DESC；pipeline 空=该群全部流水线）
  void ci_runs(quint64 gid, const QString& pipeline = QString());

  // —— 打包工具＋配置导出（R25-3 /files/group-pack、/files/group-export）——
  // 一键出产物（成员；须 pack/build 在工具白名单；stub=台账记录）
  void pack_build(quint64 gid, const QString& name, const QString& version,
                  const QString& note);
  // 产物台账（成员；created_ms 倒序）
  void pack_list(quint64 gid);
  // 删产物（恒归群主/管理员）
  void pack_delete(quint64 gid, const QString& name, const QString& version);
  // 群配置快照导出（仅群主/管理员；密文面永不进导出）
  void group_export(quint64 gid);

  // —— 工具凭据面（R25-4 /files/group-tools/credential[s]）——
  // 上/覆盖凭据（仅群主/管理员；服务端加密落库，客户端零凭据：
  // 回包只有掩码元数据，明文只此一次出门）
  void tool_cred_set(quint64 gid, const QString& tool, const QString& value);
  // 删凭据（仅群主/管理员）
  void tool_cred_delete(quint64 gid, const QString& tool);
  // 掩码元数据列表（tool/updated_by/updated_ms；永不含凭据值）
  void tool_cred_list(quint64 gid);

  // —— 收件箱（R23-3 文件助手混排面）——
  // GET /files/list?target=inbox：备忘录+文件时间倒序混排条目
  void list_inbox();
  // 「手机发自己=文件传输」：POST /files/upload?target=inbox
  void upload_inbox(const QString& file_path);
  // 下载到 save_dir 目录（文件名用服务端原名；重名自动加序号）
  void download_file(qint64 file_id, const QString& file_name,
                     const QString& save_dir);
  // 收件箱文件删除（/files/manage/delete，personal-owner 仅本人）
  void delete_file(qint64 file_id);

 signals:
  void logged_in();
  void login_failed(const QString& reason);
  void memo_created(qint64 id);
  void memo_updated(qint64 id);
  void memo_deleted(qint64 id);
  void memo_listed(const QJsonArray& memos);
  void memo_fetched(qint64 id, const QString& content);
  void group_memo_listed(const QJsonArray& memos, bool open_edit);
  void group_memo_saved(qint64 id);
  void group_memo_deleted(qint64 id);
  void group_memo_history_fetched(const QJsonArray& revisions);
  void group_memo_rolled_back(qint64 id);
  void group_memo_open_edit_set(bool open);
  void group_vault_info_fetched(const QJsonObject& info); // 含 exists 键
  void group_vault_initialized();
  void group_vault_rekeyed();
  void group_vault_listed(const QJsonArray& entries);
  void group_vault_accessed(const QJsonObject& entry); // 含 secret_*
  void group_vault_entry_saved(qint64 id);
  void group_vault_entry_deleted(qint64 id);
  void group_vault_acl_set();
  void group_vault_audit_listed(const QJsonArray& rows);
  void tool_actions_set();
  void ci_pipeline_set();
  void ci_listed(const QJsonArray& pipelines); // 红绿灯面（last_status 键）
  void ci_triggered(qint64 run_id, const QString& status); // success|failed
  void ci_runs_listed(const QJsonArray& runs);
  void pack_built();
  void pack_listed(const QJsonArray& artifacts);
  void pack_deleted();
  void group_exported(const QJsonObject& snapshot);
  void tool_cred_saved(qint64 gid, const QString& tool);
  void tool_cred_removed(qint64 gid, const QString& tool);
  void tool_credentials_listed(const QJsonArray& credentials);
  void inbox_listed(const QJsonArray& items); // type=memo|file 混排条目
  void upload_finished(qint64 file_id, bool second_transfer);
  void download_finished(const QString& save_path);
  void file_deleted(qint64 file_id);
  // 统一失败通道：op=操作名（"memo.create"/"inbox.upload"/…）、
  // status=HTTP 状态码（0=网络层失败）、error=服务端 error 字段或网络串
  void request_failed(const QString& op, int status, const QString& error);

 private:
  using JsonHandler = std::function<void(bool ok, int status,
                                         const QJsonObject& body,
                                         const QString& error)>;
  // 通用 JSON 面：带 Bearer 头、发 body（可空）、回包解析 JSON 后交 handler
  void send_json(const QString& op, const QString& method,
                 const QString& path, const QJsonObject& body,
                 const JsonHandler& handler);
  QString base_url() const { return QStringLiteral("http://%1:%2").arg(host_).arg(port_); }
  void fail(const QString& op, int status, const QString& error);

  QNetworkAccessManager* nam_;
  QString host_;
  quint16 port_{0};
  QString token_;
  QString account_;
};

} // namespace memex::client
