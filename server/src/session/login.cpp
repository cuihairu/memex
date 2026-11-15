// session.cpp 拆分模块（LAY-015）：登录校验·互踢·登出（handle_login/handle_logout 逐字）；公共面见 common.hpp，声明在 session.hpp。
#include "common.hpp"

namespace memex::server {
// 登录校验 → 记录（成功失败都记）→ 原子化互踢 → 回结果。
void Session::handle_login(const memex::protocol::Message& msg) {
  if (!msg.has_login()) {
    log("登录载荷缺字段，拒绝");
    close();
    return;
  }
  const auto& in = msg.login();
  account_ = in.account();
  device_fingerprint_ = in.device_fingerprint();
  device_name_ = in.device_name();
  kind_ = in.device_kind().empty() ? "desktop" : in.device_kind();

  LoginRecord rec;
  rec.account = account_;
  rec.fingerprint = device_fingerprint_;
  rec.kind = kind_;
  rec.name = device_name_;
  rec.source_ip = remote_;
  rec.version = in.client_version();

  const auto row = server_.store().find_account(account_);
  // 判登路由（544 LDAP 企业身份）：绑定 issuer=ldap 的账号走目录判登
  // （fail-closed——目录不可达/未配目录一律拒，绝不回落本地口令＝回落
  // 即绕过目录）；未绑定账号走原本地判登（平台-2 credentials 表，路径
  // 不动）。设备台账闸（停用/新设备审批）对两条路径一视同仁。
  const auto route = route_authentication(server_.store(), server_.ldap(),
                                          account_, in.password());
  bool ok = false;
  std::string reason;
  bool pwd_ok = false;
  if (!row) {
    reason = "账号不存在";
    rec.result = "no_account";
  } else if (route.path == AuthRoute::Path::LdapUnavailable) {
    reason = "目录认证不可用（fail-closed，不回落本地口令）";
    rec.result = "directory_unavailable";
  } else if (route.path == AuthRoute::Path::LdapDisabled) {
    reason = "目录账号已禁用：请联系管理员";
    rec.result = "directory_disabled";
  } else if (route.path == AuthRoute::Path::LdapBad) {
    reason = "口令不符";
    rec.result = "bad_password";
  } else if (route.path == AuthRoute::Path::LdapOk) {
    pwd_ok = true;
    // 显式映射角色落库（幂等；granted_by=ldap-connector 台账可查来源）
    sync_ldap_roles(server_.store(), account_, route.roles, now_ms());
  } else {
    const auto cred = server_.store().find_credential(account_, "password");
    if (!cred) {
      reason = "账号不存在";
      rec.result = "no_account";
    } else if (pbkdf2_sha256_hex(in.password(), cred->salt_hex, 60000) ==
               cred->digest_hex) {
      pwd_ok = true;
    } else {
      reason = "口令不符";
      rec.result = "bad_password";
    }
  }
  if (pwd_ok) {
    // T3.3 设备台账：已停用设备拒绝登录（启停即时生效）
    const auto dev = server_.store().find_device(device_fingerprint_);
    if (dev && !dev->enabled) {
      reason = "设备已停用：请联系管理员";
      rec.result = "device_disabled";
    } else if (!dev && server_.store().resolve_policy(account_)
                           .new_device_approval) {
      // T3.4 新设备审批：首登即建档为停用（待审批），管理员 device approve 后放行
      server_.store().upsert_device(device_fingerprint_, kind_, device_name_,
                                    now_ms());
      server_.store().set_device_enabled(device_fingerprint_, false);
      reason = "新设备待审批：请联系管理员";
      rec.result = "pending_approval";
    } else {
      ok = true;
      // 审计口径「用什么方式」：目录判登标 ok_ldap（本地仍为 ok）——
      // 密码永不入 rec（LoginRecord 无密码字段，口径即结构）
      rec.result = route.path == AuthRoute::Path::LdapOk ? "ok_ldap" : "ok";
    }
  }
  server_.store().add_login_record(rec);
  // 邮箱·告警入箱（mailbox.md §I/P3：登录异常——连续 3 次口令不符入
  // 告警（受众=admin 账号集，§Ⅵ-2 默认口径）；恰在第 3 次入箱一次，
  // 第 4/5 次不重复，成功登录即断 streak，下次连败再报）
  if (rec.result == "bad_password") {
    int streak = 0;
    for (const auto& r : server_.store().login_records(account_, "", 10)) {
      if (r.result != "bad_password") break;
      ++streak;
    }
    if (streak == 3) {
      (void)server_.store().mailbox_add(
          "warning", "登录异常：账号 " + account_ + " 连续 3 次口令不符",
          "服务器事件 · 修复后由处理人标记已处理并留痕", "admin 事件源",
          "", "", server_.store().admin_accounts(), now_ms());
    }
  }

  memex::protocol::Message result;
  result.set_type(v1::LOGIN_RESULT);
  result.set_to(account_);
  result.set_ts_ms(now_ms());
  if (ok) {
    // 同类型单点在线：同账号桌面端互踢、移动端互踢，桌面与手机并存。
    // register_online 在 io 线程内完成顶替，登录回包与互踢不会交错
    const auto kicked =
        server_.register_online(account_, kind_, shared_from_this());
    (void)kicked;
    logged_in_ = true;
    // 首登建档、再登刷新活跃时间（T3.3 台账随使用自动生长）
    server_.store().upsert_device(device_fingerprint_, kind_, device_name_,
                                  now_ms());
    result.mutable_login_result()->set_ok(true);
    result.mutable_login_result()->set_display_name(
        row->display_name);
    log("登录成功（" + account_ + "，" + kind_ + "）");
  } else {
    result.mutable_login_result()->set_ok(false);
    result.mutable_login_result()->set_reason(reason);
    log("登录失败：" + reason);
  }
  send(memex::protocol::encode(result));
  if (ok) {
    // 离线消息补投：登录回执之后推未 ACK 的队列（重复投递由接收端 msg_id 去重）
    for (const auto& blob : server_.store().pending_offline(account_)) {
      deliver_frame(blob);
    }
  }
}

void Session::handle_logout() {
  log("登出（" + account_ + "）");
  server_.unregister_online(account_, kind_, this);
  logged_in_ = false;
  close();
}


} // namespace memex::server
