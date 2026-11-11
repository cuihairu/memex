// 路由入口（LAY-003 拆分）：process 按 method+path 分派到各域
// 路由（会话/任务/审批/审计/素材/协助/文件/备忘录/群工具…）。

#include "files_server/conn.hpp"

namespace memex::server {

void FileConn::process(const std::string& body) {
  // —— 路由 ——
  // 健康探针（无鉴权）：容器 HEALTHCHECK／运维探活用；只回固定 ok，
  // 不泄账号/配置信息（uplink 面同款 /uplink/health，探活口径一致）
  if ((path_ == "/files/health" || path_ == "/uplink/health") &&
      method_ == "GET") {
    return respond_json(200, {{"ok", true}});
  }
  // R23-4 外网单向面：只挂写入端点，无任何下载/读取内网数据的路由
  //（404 兜底）。物理面分离＝独立监听口；scope 闸见各 route。
  if (impl_.uplink_mode) {
    if (path_ == "/uplink/session") {
      return route_session(body, /*uplink=*/true);
    }
    if (path_ == "/uplink/upload" && method_ == "POST") {
      return route_uplink_upload(body);
    }
    if (path_ == "/uplink/mine" && method_ == "GET") {
      return route_uplink_mine();
    }
    if (path_ == "/uplink/delete" && method_ == "POST") {
      return route_uplink_delete();
    }
    respond_json(404, {{"ok", false}, {"error", "路径不存在"}});
    return;
  }
  // 单向性第二道闸：uplink 令牌打到内网端口，读端点一律 403
  //（session/health 无需会话，豁免）
  if (path_.rfind("/files/", 0) == 0 && path_ != "/files/session" &&
      impl_.is_uplink_token(authorization_)) {
    respond_json(403, {{"ok", false},
                       {"error", "外网会话只许写入（单向 uplink）"}});
    return;
  }
  if (path_ == "/files/session") {
    return route_session(body);
  }
  if (path_ == "/files/logout" && method_ == "POST") {
    return route_logout();
  }
  if (path_ == "/files/upload" && method_ == "POST") {
    return route_upload(body);
  }
  if (path_ == "/files/download" && method_ == "GET") {
    return route_download();
  }
  if (path_ == "/files/list" && method_ == "GET") {
    return route_list();
  }
  if (path_ == "/files/quota" && method_ == "GET") {
    return route_quota_get();
  }
  if (path_ == "/files/manage/delete" && method_ == "POST") {
    return route_delete();
  }
  if (path_ == "/files/manage/pin" && method_ == "POST") {
    return route_pin();
  }
  if (path_ == "/files/manage/quota" && method_ == "POST") {
    return route_quota_set(body);
  }
  // R23-3 文件助手备忘录：POST 建/改、GET 单条/列表、DELETE 删
  if (path_ == "/files/memo" && method_ == "POST") {
    return route_memo_write(body);
  }
  if (path_ == "/files/memo" && method_ == "GET") {
    return route_memo();
  }
  if (path_ == "/files/memo" && method_ == "DELETE") {
    return route_memo_delete();
  }
  // R27-1 个人任务清单：建（分配=「谁能分配」判权）/我的清单＋我派出/
  // 完成（清单主人）/提醒回执（清单主人）/撤回（主人或分配人）
  if (path_ == "/files/tasks" && method_ == "POST") {
    return route_task_create(body);
  }
  if (path_ == "/files/tasks" && method_ == "GET") {
    return route_tasks_list();
  }
  if (path_ == "/files/tasks/done" && method_ == "POST") {
    return route_task_done(body);
  }
  // R27-6 状态切换（待办/进行中/完成——看板列语义；done 双写过渡）
  if (path_ == "/files/tasks/status" && method_ == "POST") {
    return route_task_status(body);
  }
  if (path_ == "/files/tasks/reminded" && method_ == "POST") {
    return route_task_reminded(body);
  }
  // R27-6 任务类型清单（服务端配置，admin 增删改——客户端拉取展示）
  if (path_ == "/files/task-types" && method_ == "GET") {
    return route_task_types();
  }
  if (path_ == "/files/tasks/delete" && method_ == "POST") {
    return route_task_delete(body);
  }
  // R27-5 任务源拉取面（默认关硬口径：admin 开关＋主密钥两道闸在拉取
  // 器；本面只管配置与凭据密封）：POST 设源（可选带凭据）、GET 掩码列
  // 表（只回本人行）、DELETE 撤源（配置＋凭据同撤）
  if (path_ == "/files/task-sources" && method_ == "POST") {
    return route_task_source_set(body);
  }
  if (path_ == "/files/task-sources" && method_ == "GET") {
    return route_task_sources_list();
  }
  if (path_ == "/files/task-sources" && method_ == "DELETE") {
    return route_task_source_delete();
  }
  // 邮箱·统一收件箱（mailbox.md：三类条目状态面——列表/未读计数/
  // 标记已读/告警处理留痕/公告确认台账；边界②任何操作只动邮箱两表，
  // 归档行一概不触）
  if (path_ == "/files/mailbox/list" && method_ == "POST") {
    return route_mailbox_list(body);
  }
  if (path_ == "/files/mailbox/unread" && method_ == "POST") {
    return route_mailbox_unread();
  }
  if (path_ == "/files/mailbox/read" && method_ == "POST") {
    return route_mailbox_read(body);
  }
  if (path_ == "/files/mailbox/handle" && method_ == "POST") {
    return route_mailbox_handle(body);
  }
  if (path_ == "/files/mailbox/confirm" && method_ == "POST") {
    return route_mailbox_confirm(body);
  }
  if (path_ == "/files/mailbox/confirmations" && method_ == "POST") {
    return route_mailbox_confirmations(body);
  }
  // 二期·审批（请假起步）：申请/双列表/决定/撤回；判权 az
  // approval-decide（直属上级现查或无上级 org-admin 兜底）
  if (path_ == "/files/approvals" && method_ == "POST") {
    return route_approval_create(body);
  }
  if (path_ == "/files/approvals" && method_ == "GET") {
    return route_approvals_list();
  }
  if (path_ == "/files/approvals/decide" && method_ == "POST") {
    return route_approval_decide(body);
  }
  if (path_ == "/files/approvals/withdraw" && method_ == "POST") {
    return route_approval_withdraw(body);
  }
  // 日报周报（二期）：写自己的（当日 upsert）/看自己/直属上级看下属
  if (path_ == "/files/reports" && method_ == "POST") {
    return route_report_save(body);
  }
  if (path_ == "/files/reports" && method_ == "GET") {
    return route_reports_mine();
  }
  if (path_ == "/files/reports/read" && method_ == "GET") {
    return route_reports_read();
  }
  if (path_ == "/files/reports/team" && method_ == "GET") {
    return route_reports_team();
  }
  if (path_ == "/files/tasks/team" && method_ == "GET") {
    return route_tasks_team();
  }
  // 会话审计（二期）：在线检索＋查阅日志（持 auditor 有效角色）
  if (path_ == "/files/audit/search" && method_ == "POST") {
    return route_audit_search(body);
  }
  if (path_ == "/files/audit/reads" && method_ == "GET") {
    return route_audit_reads();
  }
  // 管理台归档检索（desktop-search）三面：上下文／撤回事件／导出留证
  //（判权与留痕同检索面——auditor 有效角色，每次查阅落 audit_reads）
  if (path_ == "/files/audit/context" && method_ == "GET") {
    return route_audit_context();
  }
  if (path_ == "/files/audit/recall-event" && method_ == "GET") {
    return route_audit_recall_event();
  }
  if (path_ == "/files/audit/export" && method_ == "POST") {
    return route_audit_export(body);
  }
  // 办公室位置图（二期）：看自楼层（org-admin 可 ?floor= 指定层）/
  // 编辑工位/绑定占用者
  if (path_ == "/files/office-map" && method_ == "GET") {
    return route_office_map();
  }
  if (path_ == "/files/office-map/seat" && method_ == "POST") {
    return route_office_seat(body);
  }
  if (path_ == "/files/office-map/bind" && method_ == "POST") {
    return route_office_bind(body);
  }
  // 品牌物料（读三免鉴权＝不调 account_or_respond；写三判权在路由内）
  if (path_ == "/files/branding" && method_ == "GET") {
    return route_branding_get();
  }
  if (path_ == "/files/branding/logo" && method_ == "GET") {
    return route_branding_asset("logo", /*logo=*/true);
  }
  if (path_ == "/files/branding/splash" && method_ == "GET") {
    return route_branding_asset("splash", /*logo=*/false);
  }
  if (path_ == "/files/branding" && method_ == "POST") {
    return route_branding_set(body);
  }
  if (path_ == "/files/branding/logo" && method_ == "POST") {
    return route_branding_asset_upload(body, /*logo=*/true);
  }
  if (path_ == "/files/branding/splash" && method_ == "POST") {
    return route_branding_asset_upload(body, /*logo=*/false);
  }
  // 表情包素材（需求批②）：个人素材面——判权＝仅本人（收藏随账号走）
  if (path_ == "/files/emoji/upload" && method_ == "POST") {
    return route_emoji_upload(body);
  }
  if (path_ == "/files/emoji/list" && method_ == "GET") {
    return route_emoji_list();
  }
  if (path_ == "/files/emoji/download" && method_ == "GET") {
    return route_emoji_download();
  }
  if (path_ == "/files/emoji/delete" && method_ == "POST") {
    return route_emoji_delete(body);
  }
  // 用户头像（需求批⑫）：写两（上传/删除）本人属主裁决（他人 403），
  // 读面＝登录成员可读（组织内互见，同资料面口径）
  if (path_ == "/files/avatar/upload" && method_ == "POST") {
    return route_avatar_upload(body);
  }
  if (path_ == "/files/avatar/download" && method_ == "GET") {
    return route_avatar_download();
  }
  if (path_ == "/files/avatar/delete" && method_ == "POST") {
    return route_avatar_delete(body);
  }
  // 远程协助（二期）：生命周期＋台账＋媒体中继（模型层平台-11 在 store）
  if (path_ == "/files/assist/request" && method_ == "POST") {
    return route_assist_request(body);
  }
  if (path_ == "/files/assist/respond" && method_ == "POST") {
    return route_assist_respond(body);
  }
  if (path_ == "/files/assist/start" && method_ == "POST") {
    return route_assist_lifecycle(body, /*start=*/true);
  }
  if (path_ == "/files/assist/end" && method_ == "POST") {
    return route_assist_lifecycle(body, /*start=*/false);
  }
  if (path_ == "/files/assist/sessions" && method_ == "GET") {
    return route_assist_sessions();
  }
  if (path_ == "/files/assist/audit" && method_ == "POST") {
    return route_assist_audit(body);
  }
  if (path_ == "/files/assist/frame" && method_ == "POST") {
    return route_assist_frame(body, /*push=*/true);
  }
  if (path_ == "/files/assist/frame" && method_ == "GET") {
    return route_assist_frame(body, /*push=*/false);
  }
  if (path_ == "/files/assist/input" && method_ == "POST") {
    return route_assist_input(body, /*send=*/true);
  }
  if (path_ == "/files/assist/input" && method_ == "GET") {
    return route_assist_input(body, /*send=*/false);
  }
  // R24-2 群备忘录：群维度共享知识（管理员维护；开放编辑后成员可写，
  // 全部编辑逐笔留痕可回滚）。判权走 AuthorizationService 群规则。
  if (path_ == "/files/group-memo/list" && method_ == "GET") {
    return route_group_memo_list();
  }
  if (path_ == "/files/group-memo/save" && method_ == "POST") {
    return route_group_memo_save(body);
  }
  if (path_ == "/files/group-memo/delete" && method_ == "POST") {
    return route_group_memo_delete(body);
  }
  if (path_ == "/files/group-memo/history" && method_ == "GET") {
    return route_group_memo_history();
  }
  if (path_ == "/files/group-memo/rollback" && method_ == "POST") {
    return route_group_memo_rollback(body);
  }
  if (path_ == "/files/group-memo/open-edit" && method_ == "POST") {
    return route_group_memo_open_edit(body);
  }
  // R24-3 群密码箱：全程密文（服务端只存 b64 密文/包裹块）；解锁面在
  // 客户端本地派生 KEK；授权名单=route 叠加判定（空=全成员，群主可收窄）。
  if (path_ == "/files/group-vault/info" && method_ == "GET") {
    return route_group_vault_info();
  }
  if (path_ == "/files/group-vault/init" && method_ == "POST") {
    return route_group_vault_init(body);
  }
  if (path_ == "/files/group-vault/rekey" && method_ == "POST") {
    return route_group_vault_rekey(body);
  }
  if (path_ == "/files/group-vault/list" && method_ == "GET") {
    return route_group_vault_list();
  }
  if (path_ == "/files/group-vault/access" && method_ == "POST") {
    return route_group_vault_access(body);
  }
  if (path_ == "/files/group-vault/save" && method_ == "POST") {
    return route_group_vault_save(body);
  }
  if (path_ == "/files/group-vault/delete" && method_ == "POST") {
    return route_group_vault_delete(body);
  }
  if (path_ == "/files/group-vault/acl" && method_ == "POST") {
    return route_group_vault_acl(body);
  }
  if (path_ == "/files/group-vault/audit" && method_ == "GET") {
    return route_group_vault_audit();
  }
  if (path_ == "/files/group-tools/config" && method_ == "POST") {
    return route_group_tools_config(body);
  }
  if (path_ == "/files/group-tools/list" && method_ == "GET") {
    return route_group_tools_list();
  }
  if (path_ == "/files/group-tools/call" && method_ == "POST") {
    return route_group_tools_call(body);
  }
  if (path_ == "/files/group-tools/audit" && method_ == "GET") {
    return route_group_tools_audit();
  }
  if (path_ == "/files/group-ci/pipeline" && method_ == "POST") {
    return route_group_ci_pipeline(body);
  }
  if (path_ == "/files/group-ci/list" && method_ == "GET") {
    return route_group_ci_list();
  }
  if (path_ == "/files/group-ci/trigger" && method_ == "POST") {
    return route_group_ci_trigger(body);
  }
  if (path_ == "/files/group-ci/runs" && method_ == "GET") {
    return route_group_ci_runs();
  }
  if (path_ == "/files/group-pack/build" && method_ == "POST") {
    return route_group_pack_build(body);
  }
  if (path_ == "/files/group-pack/list" && method_ == "GET") {
    return route_group_pack_list();
  }
  if (path_ == "/files/group-pack/delete" && method_ == "POST") {
    return route_group_pack_delete(body);
  }
  if (path_ == "/files/group-export" && method_ == "GET") {
    return route_group_export();
  }
  // 群组-2 群记录导出（Markdown 三段：参与人/时间范围/聊天记录；
  // 判权＝本群成员，非群成员 403；一次导出落 audit_reads 一行）
  if (path_ == "/files/group-record-export" && method_ == "GET") {
    return route_group_record_export();
  }
  // R25-4 凭据面：写/删凭据（管理面 memo:config；回包绝不回显 value）
  // 与掩码元数据列表（客户端零凭据——明文只在服务端代理内存内）
  if (path_ == "/files/group-tools/credential" && method_ == "POST") {
    return route_group_tools_credential(body);
  }
  if (path_ == "/files/group-tools/credentials" && method_ == "GET") {
    return route_group_tools_credentials();
  }
  // R26-1 服务器 agent 面：登记（管理面）/心跳（agent 注册令牌，非
  // 人会话）/列表（群成员——入群即授权）
  if (path_ == "/files/group-servers/enroll" && method_ == "POST") {
    return route_group_servers_enroll(body);
  }
  if (path_ == "/files/group-servers/heartbeat" && method_ == "POST") {
    return route_group_servers_heartbeat(body);
  }
  if (path_ == "/files/group-servers/list" && method_ == "GET") {
    return route_group_servers_list();
  }
  // R26-3 远程会话（SSH 起步）：签一次性短票＋接入留痕＋短票兑现＋
  // 收尾（谁/何时/连哪台/时长全程可回溯）
  if (path_ == "/files/group-servers/session/request" &&
      method_ == "POST") {
    return route_group_servers_session_request(body);
  }
  if (path_ == "/files/group-servers/session/redeem" &&
      method_ == "POST") {
    return route_group_servers_session_redeem(body);
  }
  if (path_ == "/files/group-servers/session/close" &&
      method_ == "POST") {
    return route_group_servers_session_close(body);
  }
  if (path_ == "/files/group-servers/sessions" && method_ == "GET") {
    return route_group_servers_sessions();
  }
  // R26-4 服务器凭据：目标机凭据只存服务端（加密落库、回包只见掩码
  // 元数据——客户端零凭据，真用凭据的操作归 croupier）
  if (path_ == "/files/group-servers/credential" && method_ == "POST") {
    return route_group_servers_credential(body);
  }
  // 二期群工具三件（原生互动，不走 R25 外部工具代理）：判权全循
  // file:read 群继承（tool_call_allowed 同构），身份约束服务端逻辑判
  if (path_ == "/files/group-polls" && method_ == "POST") {
    return route_group_poll_create(body);
  }
  if (path_ == "/files/group-polls" && method_ == "GET") {
    return route_group_polls_list();
  }
  if (path_ == "/files/group-polls/vote" && method_ == "POST") {
    return route_group_poll_vote(body);
  }
  if (path_ == "/files/group-polls/close" && method_ == "POST") {
    return route_group_poll_close(body);
  }
  if (path_ == "/files/group-chains" && method_ == "POST") {
    return route_group_chain_create(body);
  }
  if (path_ == "/files/group-chains" && method_ == "GET") {
    return route_group_chains_list();
  }
  if (path_ == "/files/group-chains/join" && method_ == "POST") {
    return route_group_chain_join(body);
  }
  if (path_ == "/files/group-chains/close" && method_ == "POST") {
    return route_group_chain_close(body);
  }
  if (path_ == "/files/group-tasks" && method_ == "POST") {
    return route_group_task_create(body);
  }
  if (path_ == "/files/group-tasks" && method_ == "GET") {
    return route_group_tasks_list();
  }
  if (path_ == "/files/group-tasks/claim" && method_ == "POST") {
    return route_group_task_claim(body);
  }
  if (path_ == "/files/group-tasks/done" && method_ == "POST") {
    return route_group_task_done(body);
  }
  respond_json(404, {{"ok", false}, {"error", "路径不存在"}});
}

} // namespace memex::server
