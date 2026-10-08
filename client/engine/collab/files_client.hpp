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

  // —— 群服务器面（R26-1 /files/group-servers）——
  // 登记（仅群主/管理员；回包带一次性注册令牌——喂给服务器上的 agent；
  // 重登记=轮换令牌，旧 agent 立即失联）
  void server_enroll(quint64 gid, const QString& name, const QString& host);
  // 服务器列表（群成员入群即授权；红绿灯=服务端 online 键＋最近一拍指标）
  void server_list(quint64 gid);

  // —— 远程会话（R26-3 /files/group-servers/session[s]，SSH 起步）——
  // 发起会话：一次性短票出门（60s TTL；票明文只此一次，服务端只存摘要）
  void session_request(quint64 gid, qint64 server_id,
                       const QString& protocol = QStringLiteral("ssh"));
  // 即时兑现（票即凭据；本地拉起 ssh 前才兑现——重放 409）
  void session_redeem(const QString& ticket);
  // 收尾（本人＋进行中才可；落时长留痕）
  void session_close(quint64 gid, qint64 session_id);
  // 接入留痕列表（成员；短票摘要永不出现）
  void session_list(quint64 gid);

  // —— 服务器凭据面（R26-4 /files/group-servers/credential）——
  // 上/覆盖目标机凭据（仅群主/管理员；服务端加密落库，客户端零凭据：
  // 服务器列表只回掩码元数据，明文只此一次出门）
  void server_cred_set(quint64 gid, qint64 server_id, const QString& value);
  // 删凭据（仅群主/管理员）
  void server_cred_delete(quint64 gid, qint64 server_id);

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

  // —— 个人任务清单（R27-1 /files/tasks）——
  // 建（assignee 空=自建；非空=分配给他人，服务端按「同群/同部门」判权。
  // R27-2 外部任务登记：provider/ext_key 成对非空=外部引用条目，
  // 个人登记不转派，详情 URL 由客户端 provider SPI 解析）
  void create_task(const QString& title, const QString& note, qint64 due_ms,
                   const QString& assignee = QString(),
                   const QString& provider = QString(),
                   const QString& ext_key = QString());
  // 我的清单＋我派出的（一次双数组）
  void list_tasks();
  // 完成/回退（仅清单主人）
  void set_task_done(qint64 id, bool done);
  // 提醒回执（仅清单主人；服务端只落一次时刻）
  void mark_task_reminded(qint64 id);
  // 撤回（清单主人或分配人）
  void delete_task(qint64 id);

  // —— 审批（二期·请假起步 /files/approvals）——
  // 建申请（type 白名单：年假/事假/病假/调休；from/to/reason 可选）
  void create_approval(const QString& type, const QString& from,
                       const QString& to, const QString& reason);
  // 我申请的＋待我决（一次双数组；待我决按判权现裁后下发）
  void list_approvals();
  // 决定（直属上级或无上级 org-admin，服务端现裁）
  void decide_approval(qint64 id, bool approved, const QString& note);
  // 撤回（申请人专属且仅 pending）
  void withdraw_approval(qint64 id);

  // —— 日报周报（二期 /files/reports）——
  // 写日报（date=归属日 YYYY-MM-DD；当日重复提交=服务端 upsert 更新）
  void save_report(const QString& date, const QString& content);
  // 我的日报全部（date 倒序）
  void list_reports();
  // 团队聚合（直属上级=各直接下属的日报分组；判权服务端现裁）
  void fetch_team_reports();

  // —— 会话审计（二期 /files/audit；持 auditor 有效角色，被拒也留痕）——
  // 归档检索（条件全部可空=全量；每次检索服务端落查阅日志）
  void audit_search(const QString& account, const QString& keyword,
                    qint64 since_ms, qint64 until_ms);
  // 查阅日志（台账自阅，同样持证）
  void fetch_audit_reads();

  // —— 办公室位置图（二期 /files/office-map）——
  // 看平面（floor 空=自楼层；org-admin 可指定层；回包含 can_manage）
  void fetch_office_map(const QString& floor = QString());
  // 建/改位（拖拽落位即 upsert；坐标归一化 0~1，服务端夹越界）
  void save_office_seat(const QString& floor, const QString& label, double x,
                        double y);
  // 删工位（org-admin）
  void delete_office_seat(qint64 id);
  // 绑定/解绑占用者（account 空=解绑；一人一工位，换座先解绑）
  void bind_office_seat(qint64 id, const QString& account);

  // —— 远程协助（二期 /files/assist；模型层平台-11，consent/audit 红线
  //     与部门放行开关全在服务端）——
  // 发起协助（perms=view/keyboard/mouse/clipboard/file 非空子集）
  void assist_request(const QString& target, const QStringList& perms);
  // 受控方批/拒（approve=false 忽略 perms；实批 ⊆ 申请可缩不可扩）
  void assist_respond(const QString& id, bool approve,
                      const QStringList& perms);
  // 启动（approved→active；任一当事方）
  void assist_start(const QString& id);
  // 结束/撤权（受控方 active 期结束=撤权即时生效；任一当事方）
  void assist_end(const QString& id, const QString& reason);
  // 我的会话台账（发起或受控，倒序）＝过程持续可见面
  void fetch_assist_sessions();
  // 会话审计链（当事方可读）
  void fetch_assist_audit(const QString& id);
  // 受控方推帧（JPEG b64；服务端只存最新帧不落库，终态即擦）
  void push_assist_frame(const QString& id, qint64 seq,
                         const QString& jpeg_b64);
  // 发起方拉最新帧（无帧回 seq=0 空 b64）
  void pull_assist_frame(const QString& id);
  // 发起方发输入事件（kind=mouse_move|mouse_click|key；须对应权限位）
  void send_assist_input(const QString& id, const QString& kind, double x,
                         double y, const QString& key);
  // 受控方取走输入事件（FIFO 取走即清）
  void fetch_assist_input(const QString& id);

  // —— 群工具三件（二期·投票/接龙/群任务，原生互动不走 R25 代理）——
  // 判权与身份约束全在服务端（file:read 群继承、发起人或群主/管理员），
  // 客户端只提交与展示
  // 建投票（topic 非空、options 2~10；deadline_ms 0=不设截止；
  // anonymous=匿名（台账不回 voter）；multi=多选（choice 存位集））
  void create_poll(quint64 gid, const QString& topic,
                   const QStringList& options, qint64 deadline_ms,
                   bool anonymous = false, bool multi = false);
  // 投票列表（含 counts 票数统计与 votes 记名台账）
  void list_polls(quint64 gid);
  // 投/改票（choice=选项序号 1 起；改票=覆盖）
  void vote_poll(quint64 gid, qint64 poll_id, int choice);
  // 关票（发起人或群主/管理员；已关 409）
  void close_poll(quint64 gid, qint64 poll_id);
  // 建接龙（title 非空；format_hint 可空）
  void create_chain(quint64 gid, const QString& title,
                    const QString& format_hint);
  // 接龙列表（条目 ts ASC）
  void list_chains(quint64 gid);
  // 加入/更新自己条目（一人一条 upsert）
  void join_chain(quint64 gid, qint64 chain_id, const QString& content);
  // 关接龙（同关票口径）
  void close_chain(quint64 gid, qint64 chain_id);
  // 建群任务（assignee 可空=待认领；非空须群成员否则 404）
  void create_group_task(quint64 gid, const QString& title,
                         const QString& assignee);
  // 群任务列表（status/done_by/claimed_ms 留痕可见）
  void list_group_tasks(quint64 gid);
  // 认领（todo 且无人认领；已占 409）
  void claim_group_task(quint64 gid, qint64 task_id);
  // 完成（负责人/创建者/群主/管理员；非 todo 409）
  void done_group_task(quint64 gid, qint64 task_id);

  // —— 品牌物料（二期 /files/branding；读面免鉴权归 BrandKit，此处是
  //     设置页写面，判权 branding-manage=org-admin 全在服务端）——
  // 当前配置回显（登录后拉；展示区＋编辑区初始值）
  void fetch_branding();
  // 保存文本三件（显式空串=清空；一次保存=服务端一个版本）
  void save_branding(const QString& company_name, const QString& accent,
                     const QString& slogan);
  // 素材清除（clear_* 布尔走同一路由；一次动作=一个版本）
  void clear_brand_logo();
  void clear_brand_splash();
  // 素材上传（PNG 原始字节；类型/大小/尺寸校验在服务端，语义化
  // 413/415 经统一失败通道回状态行）
  void upload_brand_logo(const QString& file_path);
  void upload_brand_splash(const QString& file_path);

  // —— 表情包素材（需求批②：个人素材，仅本人读写；随账号走）——
  // 上传本地图片（png/jpg/jpeg/gif 按后缀定 Content-Type；magic 与
  // 1MiB 上限校验在服务端）
  void emoji_upload(const QString& file_path, const QString& name);
  // 我的素材清单（ts 倒序；[{id,name,size,ts_ms}]，不含字节）
  void emoji_list();
  // 下载到 save_dir（文件名用素材原名；重名自动加序号）
  void emoji_download(qint64 id, const QString& name,
                      const QString& save_dir);
  // 删除我的素材
  void emoji_delete(qint64 id);

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
  void server_enrolled(qint64 gid, qint64 server_id, const QString& token);
  void servers_listed(const QJsonArray& servers);
  void session_requested(qint64 gid, qint64 session_id, const QString& ticket,
                         qint64 expires_ms);
  void session_redeemed(qint64 session_id, qint64 gid,
                        const QString& server_name, const QString& host);
  void session_closed(qint64 session_id);
  void sessions_listed(const QJsonArray& sessions);
  void server_cred_saved(qint64 gid, qint64 server_id);
  void server_cred_removed(qint64 gid, qint64 server_id);
  void inbox_listed(const QJsonArray& items); // type=memo|file 混排条目
  void upload_finished(qint64 file_id, bool second_transfer);
  void download_finished(const QString& save_path);
  void file_deleted(qint64 file_id);
  void task_created(qint64 id);
  void tasks_listed(const QJsonArray& mine, const QJsonArray& assigned_by_me);
  void task_done_set(qint64 id);
  void task_reminded(qint64 id);
  void task_deleted(qint64 id);
  void approval_created(qint64 id);
  void approvals_listed(const QJsonArray& mine, const QJsonArray& pending);
  void approval_decided(qint64 id); // 同意/拒绝共用；终态以列表回查为准
  void approval_withdrawn(qint64 id);
  void report_saved(qint64 id);
  void reports_listed(const QJsonArray& reports);
  void team_reports_listed(const QJsonArray& team); // author 分组＋reports
  void audit_searched(const QJsonArray& messages);
  void audit_reads_listed(const QJsonArray& reads);
  void office_map_fetched(const QJsonObject& map); // floor/can_manage/seats
  void office_seat_saved(qint64 id);
  void office_seat_deleted();
  void office_seat_bound();
  void assist_requested(const QString& id);
  void assist_responded(const QString& id);
  void assist_started(const QString& id);
  void assist_ended(const QString& id);
  void assist_sessions_listed(const QJsonArray& sessions);
  void assist_audit_listed(const QJsonArray& audits);
  void assist_frame_pushed(qint64 seq);
  void assist_frame_pulled(qint64 seq, const QString& jpeg_b64);
  void assist_input_sent();
  void assist_inputs_listed(const QJsonArray& events);
  // —— 群工具三件 ——
  void poll_created(qint64 id);
  void polls_listed(const QJsonArray& polls); // 含 counts/votes 记名台账
  void poll_voted(qint64 id);
  void poll_closed(qint64 id);
  void chain_created(qint64 id);
  void chains_listed(const QJsonArray& chains); // 含 entries ts ASC
  void chain_joined(qint64 id);
  void chain_closed(qint64 id);
  void group_task_created(qint64 id);
  void group_tasks_listed(const QJsonArray& tasks);
  void group_task_claimed(qint64 id);
  void group_task_done(qint64 id);
  // —— 品牌物料 ——
  void branding_fetched(const QJsonObject& branding); // 全字段＋version
  void branding_saved(qint64 version);
  void brand_logo_cleared();
  void brand_splash_cleared();
  void brand_logo_uploaded(qint64 version);
  void brand_splash_uploaded(qint64 version);
  // —— 表情包素材（需求批②）——
  void emoji_uploaded(qint64 id);
  void emoji_listed(const QJsonArray& assets);
  void emoji_downloaded(const QString& save_path);
  void emoji_deleted(qint64 id);
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
