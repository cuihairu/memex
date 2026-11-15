// admin_app 共性面实现（LAY-004 拆分自 admin_app.cpp 匿名空间，体逐字
// 保留——声明/签名见 common.hpp）。
#include "common.hpp"

#include <cstring>
#include <iostream>
#include <mutex>

namespace memex::admin {


// —— JSON 应答与请求体小工具 ——

drogon::HttpResponsePtr json_resp(drogon::HttpStatusCode code,
                                  const Json::Value& j) {
  auto resp = drogon::HttpResponse::newHttpJsonResponse(j);
  resp->setStatusCode(code);
  return resp;
}

Json::Value ok_json() {
  Json::Value j;
  j["ok"] = true;
  return j;
}

drogon::HttpResponsePtr err_resp(drogon::HttpStatusCode code,
                                 const std::string& msg) {
  Json::Value j;
  j["ok"] = false;
  j["error"] = msg;
  return json_resp(code, j);
}

// 请求体解析（坏体＝null 返回，调用方 400）
std::shared_ptr<Json::Value> body_of(const drogon::HttpRequestPtr& req) {
  return req->getJsonObject();
}

std::string str_or(const Json::Value& j, const char* key,
                   const std::string& dflt) {
  return j.isMember(key) && j[key].isString() ? j[key].asString() : dflt;
}

std::int64_t int_or(const Json::Value& j, const char* key,
                    std::int64_t dflt) {
  if (!j.isMember(key)) return dflt;
  if (j[key].isInt64()) return j[key].asInt64(); // 时间戳级数值超 int32
  return j[key].isConvertibleTo(Json::intValue) ? j[key].asInt64() : dflt;
}

bool bool_or(const Json::Value& j, const char* key, bool dflt) {
  return j.isMember(key) && j[key].isBool() ? j[key].asBool() : dflt;
}

// 群号解析（与 files 面 parse_gid_param 同口径：非空纯数字且 >0）
bool parse_gid_q(const std::string& s, std::uint64_t* out) {
  if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) {
    return false;
  }
  *out = std::strtoull(s.c_str(), nullptr, 10);
  return *out > 0;
}

// 远程协助权限位 → 逗号串（view/keyboard/mouse/clipboard/file 顺序固定，
// 与 files 面 assist_perm_mask 同词汇表）
std::string assist_mask_names(int mask) {
  static const struct {
    int bit;
    const char* name;
  } kBits[] = {{memex::server::ServerStore::kAssistView, "view"},
               {memex::server::ServerStore::kAssistKeyboard, "keyboard"},
               {memex::server::ServerStore::kAssistMouse, "mouse"},
               {memex::server::ServerStore::kAssistClipboard, "clipboard"},
               {memex::server::ServerStore::kAssistFileTransfer, "file"}};
  std::string out;
  for (const auto& b : kBits) {
    if ((mask & b.bit) == 0) continue;
    if (!out.empty()) out += ",";
    out += b.name;
  }
  return out;
}

// 品牌素材 PNG 校验（与 files 面 png_asset_check 同口径：magic 8 字节＋
// ≤512KiB＋IHDR 尺寸 ≤2048×2048；IHDR 宽/高在固定偏移 16/20 大端）。
// 回 {0,""}=过，否则 {status,error}。该校验在 files_server 是文件内 static，
// 管理面直连库、不复用服务端路由层——同口径重写一次（不动既有服务端）。
std::pair<int, std::string> png_asset_check(const std::string& b) {
  static const unsigned char kPngMagic[8] = {0x89, 0x50, 0x4E, 0x47,
                                             0x0D, 0x0A, 0x1A, 0x0A};
  if (b.size() < 8 || std::memcmp(b.data(), kPngMagic, 8) != 0) {
    return {415, "素材须为 PNG 格式"};
  }
  if (b.size() > 512 * 1024) {
    return {413, "素材超大小上限（512KiB）"};
  }
  if (b.size() >= 24) {
    const auto be32 = [&](std::size_t off) {
      return (static_cast<std::uint32_t>(
                  static_cast<unsigned char>(b[off]))
              << 24) |
             (static_cast<std::uint32_t>(
                  static_cast<unsigned char>(b[off + 1]))
              << 16) |
             (static_cast<std::uint32_t>(
                  static_cast<unsigned char>(b[off + 2]))
              << 8) |
             static_cast<std::uint32_t>(
                 static_cast<unsigned char>(b[off + 3]));
    };
    const std::uint32_t w = be32(16);
    const std::uint32_t h = be32(20);
    if (w == 0 || h == 0 || w > 2048 || h > 2048) {
      return {413, "素材尺寸超限（≤2048×2048）"};
    }
  }
  return {0, ""};
}

// 会话取当前账号（未登录＝空串）
std::string session_account(const drogon::HttpRequestPtr& req) {
  const auto acct = req->session()->getOptional<std::string>("account");
  return acct.value_or(std::string{}); // {} 推不出 value_or 模板参（CI GCC 拒，GCC 15 收）
}

// —— CSRF 轻闸：改写类请求须带自定义头（跨站表单无法携带，CORS 预检
//    也过不来）——部署面反代到公网时这是最低限度的防跨站写。如实口径：
//    不是完整 CSRF 方案，同站 XSS 不设防（后台工具先保内网可用）。
bool csrf_ok(const drogon::HttpRequestPtr& req) {
  return req->getHeader("X-Memex-Admin") == "1";
}


// 判权门：org-admin 面。拒绝留痕（与 files_server 同款 std::cout）。
std::optional<drogon::HttpResponsePtr> require_manage(
    AdminContext& ctx, const drogon::HttpRequestPtr& req,
    const std::string& action_detail) {
  const std::string account = session_account(req);
  const memex::server::Decision d = ctx.az.authorize(
      {account, "admin:manage", "admin", "admin-web:" + action_detail, ""});
  if (d.allowed) return std::nullopt;
  std::cout << "[MEMEX-ADMIN] admin.denied account=" << account
            << " action=" << action_detail << " reason=" << d.reason << "\n";
  return err_resp(drogon::k403Forbidden, "无权（" + d.reason + "）");
}


// 判权门：auditor 面（归档检索/导出/查阅日志）。拒绝同样留痕。
std::optional<drogon::HttpResponsePtr> require_audit(
    AdminContext& ctx, const drogon::HttpRequestPtr& req) {
  const std::string account = session_account(req);
  const memex::server::Decision d =
      ctx.az.authorize({account, "audit:read", "audit", "admin-web", ""});
  if (d.allowed) return std::nullopt;
  // 被拒尝试落查阅日志（action=audit.denied，与 CLI/服务端同口径对账）
  memex::server::AuditReadRow denied;
  denied.op_account = account;
  denied.action = "audit.denied";
  denied.filters = "admin-web";
  denied.ts_ms = now_ms();
  {
    std::lock_guard<std::mutex> lk(ctx.mu);
    ctx.store->add_audit_read(denied);
  }
  std::cout << "[MEMEX-ADMIN] audit.denied account=" << account
            << " reason=" << d.reason << "\n";
  return err_resp(drogon::k403Forbidden,
                  "无 auditor 有效角色（org-admin 不自动可读消息，须经"
                  "授权授予 auditor）；被拒尝试已留痕");
}

} // namespace memex::admin
