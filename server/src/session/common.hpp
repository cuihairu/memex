// session.cpp 拆分共性面（LAY-015 自 session.cpp 逐字搬移）：include 块/协议别名/时间与 UTF-8 工具/is_recipient；各域 handler（Session::h_*）声明在 session.hpp、定义在各模块。
#pragma once

#include "session.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <optional>

#include "cred.hpp"
#include "ldap.hpp" // 544 企业身份判登路由（route_authentication）
#include "webhook.hpp" // R24-1 公告联动三级推送（deliver_notice）

namespace memex::server {

namespace v1 = memex::protocol::v1;
inline std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// UTF-8 码点计数（需求批⑪签名「120 字」门：按字数不按字节——
// 一个汉字 3 字节，字节口径会把 40 字汉字签名误判超限）
inline std::size_t utf8_len(const std::string& s) {
  std::size_t n = 0;
  for (const unsigned char ch : s) {
    if ((ch & 0xC0) != 0x80) ++n; // 非续字节＝一个码点起点
  }
  return n;
}

// BUG-007 撞 id 消歧（设计 docs/design/消息标识与归档去重.md §3）：按派生 id
// （sha256(from:seq)）落归档并检查返回值——首插即返回；撞键（false）时按
// msg_id 查既有行比对 (to_account,type,text,ts_ms)：全同＝真补传（维持现状
// 全链，补传幂等原样保留），有异＝重用死 seq 的新消息（换盐重排
// sha256(from:to:ts:随机盐)，NOTICE 同式零持久状态）；查无此行或重排落库
// 失败＝库错，返回 nullopt（评审 P0-2：归档失败不谎报成功——调用方不投递
// 不回 ACK，发送端在途行保持 SENDING 随断线补传重试）。返回最终 msg_id。
inline std::optional<std::string> archive_and_resolve(CollabServer& server,
                                               const memex::protocol::Message& msg,
                                               const std::string& text,
                                               const std::string& derived_id) {
  const std::int64_t ts = msg.ts_ms() > 0 ? msg.ts_ms() : now_ms();
  const std::string from = msg.from();
  const std::string to = msg.to();
  const int type = static_cast<int>(msg.type());
  if (server.store().store_message(derived_id, from, to, type, text, ts)) {
    return derived_id; // 首插：常规路径
  }
  const auto existing = server.store().message_by_id(derived_id);
  if (!existing) return std::nullopt; // 库错：归档失败不谎报成功
  const bool same = existing->to_account == to && existing->type == type &&
                    existing->text == text && existing->ts_ms == ts;
  if (same) return derived_id; // 真补传：维持现状全链（离线队列 OR-IGNORE）
  // 重用死 seq 的新消息：换盐重排（时间戳＋随机盐）
  const std::string reshuffled = sha256_hex(
      from + ":" + to + ":" + std::to_string(ts) + ":" + random_salt_hex());
  if (!server.store().store_message(reshuffled, from, to, type, text, ts)) {
    return std::nullopt; // 重排落库失败：同样不谎报
  }
  return reshuffled;
}

// 平台-1 伪造回执闸（授权盘点 P0-1 补漏）：delivered/read 事件仅消息
// 收件人可记——单聊＝收件人本人；群消息＝群成员。非收件人的 ACK/READ
// 属伪造回执，拒不留痕不通知（队列清退本就按收件人 SQL 限定，不受影响）
inline bool is_recipient(CollabServer& server, const std::string& to_account,
                  const std::string& account) {
  if (to_account == account) return true;
  if (to_account.rfind("group:", 0) == 0) {
    const auto gid = static_cast<std::uint64_t>(
        std::strtoull(to_account.c_str() + 6, nullptr, 10));
    return server.store().is_group_member(gid, account);
  }
  return false;
}

} // namespace memex::server
