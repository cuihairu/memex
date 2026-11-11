// FileConn 类定义（LAY-003 拆分）：单连接 HTTP 会话骨架与方法声明。
// 成员函数按路由域分布在 files_server/ 各模块（类外定义）；骨架
// do_read/on_data/parse_head/respond_json 在 conn.cpp。
#pragma once

#include "files_server.hpp"


#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include <memex/protocol/messages.hpp>

#include "cred.hpp"

#include "files_server/common.hpp"
#include "files_server/impl.hpp"

namespace memex::server {

class FileConn : public std::enable_shared_from_this<FileConn> {
 public:
FileConn(tcp::socket sock, FileServer::Impl& impl)
    : sock_(std::move(sock)), impl_(impl) {}
void start() { do_read(); }
 private:

  // —— approval ——
  void route_approval_create(const std::string& body);
  void route_approvals_list();
  void route_approval_decide(const std::string& body);
  static json mailbox_row_json(const ServerStore::MailboxRow& r);
  void route_mailbox_list(const std::string& body);
  void route_mailbox_unread();
  void route_mailbox_read(const std::string& body);
  void route_mailbox_handle(const std::string& body);
  void route_mailbox_confirm(const std::string& body);
  void route_mailbox_confirmations(const std::string& body);
  void route_approval_withdraw(const std::string& body);
  static json report_row_json(const ServerStore::ReportRow& r);
  void route_report_save(const std::string& body);
  void route_reports_mine();
  void route_reports_read();
  void route_reports_team();

  // —— assets ——
  static std::pair<int, std::string> png_asset_check(
      const std::string& bytes);
  static std::pair<int, std::string> emoji_asset_check(
      const std::string& bytes);
  static const char* emoji_content_type(const std::string& bytes);
  void respond_png(int status, const std::vector<unsigned char>& bytes);
  void respond_image(int status, const char* content_type,
                     const std::vector<unsigned char>& bytes);
  void route_emoji_upload(const std::string& body);
  void route_emoji_list();
  void route_emoji_download();
  void route_emoji_delete(const std::string& body);
  void route_avatar_upload(const std::string& body);
  void route_avatar_download();
  void route_avatar_delete(const std::string& body);
  void route_branding_get();
  void route_branding_asset(const char* /*name*/, bool logo);
  void route_branding_set(const std::string& body);
  void route_branding_asset_upload(const std::string& body, bool logo);

  // —— assist ——
  void route_assist_request(const std::string& body);
  void route_assist_respond(const std::string& body);
  void route_assist_lifecycle(const std::string& body, bool start);
  void route_assist_sessions();
  void route_assist_audit(const std::string& body);
  std::string assist_media_gate(const std::string& id,
                                const std::string& account, bool as_target,
                                int need_bit);
  void route_assist_frame(const std::string& body, bool push);
  void route_assist_input(const std::string& body, bool send);

  // —— audit ——
  void route_audit_search(const std::string& body);
  void route_audit_reads();
  bool audit_gate_denied(const std::string& account,
                         const std::string& filters);
  void route_audit_context();
  void route_audit_recall_event();
  void route_audit_export(const std::string& body);
  static json seat_row_json(const ServerStore::SeatRow& s);
  bool office_can_manage(const std::string& account);
  bool branding_can_manage(const std::string& account);
  void route_office_map();
  void route_office_seat(const std::string& body);
  void route_office_bind(const std::string& body);

  // —— conn ——
  void do_read();
  void on_data();
  bool parse_head(std::size_t head_end);
  std::string account_or_respond();
  void respond_json(int status, const json& body);
  void write_raw(std::shared_ptr<const std::string> bytes,
                 bool close_when_done,
                 std::function<void(std::error_code)> done);

  // —— files ——
  void route_upload(const std::string& body);
  void route_download();
  void route_list();
  void route_quota_get();
  void route_delete();
  void route_pin();
  void route_quota_set(const std::string& body);

  // —— interact ——
  bool group_manage_allowed(std::uint64_t gid, const std::string& account,
                            const std::string& initiator);
  void route_group_poll_create(const std::string& body);
  void route_group_polls_list();
  void route_group_poll_vote(const std::string& body);
  std::string poll_card_text(const ServerStore::GroupPoll& p);
  std::string chain_card_text(const ServerStore::GroupChain& c);
  void route_group_poll_close(const std::string& body);
  void route_group_chain_create(const std::string& body);
  void route_group_chains_list();
  void route_group_chain_join(const std::string& body);
  void route_group_chain_close(const std::string& body);
  void route_group_task_create(const std::string& body);
  void route_group_tasks_list();
  void route_group_task_claim(const std::string& body);
  void route_group_task_done(const std::string& body);

  // —— memo ——
  static std::string memo_resource(const std::string& owner,
                                   std::int64_t id);
  void route_memo_write(const std::string& body);
  void route_memo();
  void route_memo_delete();
  static std::string group_resource(std::uint64_t gid);
  static std::string group_memo_resource(std::uint64_t gid, std::int64_t id);
  static bool parse_gid_param(const std::string& s, std::uint64_t* out);
  bool group_memo_write_allowed(const std::string& account,
                                std::uint64_t gid,
                                const std::string& resource);
  void route_group_memo_list();
  void route_group_memo_save(const std::string& body);
  void mailbox_note_memo_change(std::uint64_t gid, std::int64_t memo_id,
                                const std::string& title,
                                const std::string& content,
                                const std::string& editor, bool created);
  void route_group_memo_delete(const std::string& body);
  void route_group_memo_history();
  void route_group_memo_rollback(const std::string& body);
  void route_group_memo_open_edit(const std::string& body);

  // —— router ——
  void process(const std::string& body);

  // —— servers ——
  void route_group_servers_enroll(const std::string& body);
  void route_group_servers_heartbeat(const std::string& body);
  void route_group_servers_list();
  void route_group_servers_session_request(const std::string& body);
  void route_group_servers_session_redeem(const std::string& body);
  void route_group_servers_session_close(const std::string& body);
  void route_group_servers_sessions();
  void route_group_servers_credential(const std::string& body);

  // —— session ——
  void route_session(const std::string& body, bool uplink = false);
  void route_logout();
  std::string uplink_account_or_respond();
  void route_uplink_upload(const std::string& body);
  void route_uplink_mine();
  void route_uplink_delete();

  // —— tasks ——
  void route_task_create(const std::string& body);
  json task_row_json(const ServerStore::TaskRow& t);
  void route_tasks_list();
  void route_tasks_team();
  void route_task_done(const std::string& body);
  void route_task_status(const std::string& body);
  void route_task_types();
  void route_task_reminded(const std::string& body);
  void route_task_delete(const std::string& body);
  void route_task_source_set(const std::string& body);
  void route_task_sources_list();
  void route_task_source_delete();

  // —— tools ——
  bool tool_call_allowed(const std::string& account, std::uint64_t gid);
  bool capability_gate(std::uint64_t gid, const char* cap,
                       const char* label);
  void route_group_tools_config(const std::string& body);
  void route_group_tools_list();
  void route_group_tools_call(const std::string& body);
  void route_group_tools_audit();
  void route_group_ci_pipeline(const std::string& body);
  void route_group_ci_list();
  void route_group_ci_trigger(const std::string& body);
  void route_group_ci_runs();
  void route_group_pack_build(const std::string& body);
  void route_group_pack_list();
  void route_group_pack_delete(const std::string& body);
  void route_group_export();
  void route_group_record_export();
  void route_group_tools_credential(const std::string& body);
  void route_group_tools_credentials();

  // —— vault ——
  bool vault_unlock_allowed(const std::string& account, std::uint64_t gid);
  static bool vault_wrap_fields(const json& j, std::uint64_t* gid,
                                std::string* salt, int* iters,
                                std::string* wrapped);
  void route_group_vault_info();
  void route_group_vault_init(const std::string& body);
  void route_group_vault_rekey(const std::string& body);
  void route_group_vault_list();
  void route_group_vault_access(const std::string& body);
  void route_group_vault_save(const std::string& body);
  void route_group_vault_delete(const std::string& body);
  void route_group_vault_acl(const std::string& body);
  void route_group_vault_audit();

  // —— 品牌物料（设计稿 docs/design/品牌物料.md）：读三免鉴权（登录窗
  // 要在鉴权前显示，物料非敏感）；写三判权 branding-manage（org-admin）——
  static constexpr std::size_t kBrandAssetMax = 512 * 1024; // 512KiB

  // 表情素材常量（需求批②）：GIF 动图上限 1MiB
  static constexpr std::size_t kEmojiAssetMax = 1024 * 1024;

  tcp::socket sock_;
  FileServer::Impl& impl_;
  std::array<char, 4096> chunk_{};
  std::string buf_;
  bool head_parsed_{false};
  bool expect_continue_{false};
  bool seen_content_length_{false};
  std::size_t body_cap_{kMaxJsonBody};
  std::string method_;
  std::string path_;
  std::string query_;
  std::string authorization_;
  std::string file_name_;
  std::size_t content_length_{0};
};

} // namespace memex::server
