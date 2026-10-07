// R23-3 文件助手客户端面：FileServer HTTP API 的 Qt 封装。
// 会话令牌内存持有（Bearer，服务端 12h TTL）；所有字节面（上传/下载）
// 同面走 FileServer——客户端永不直连对象存储（设计铁律 3）。
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

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
