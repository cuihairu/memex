// 服务端本地库（SQLite）：账号表、登录记录表与离线消息队列。
// 归档消息表在 T2.3 接入同一库文件；本层只做存储，不含业务策略。
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

typedef struct sqlite3 sqlite3;

namespace memex::server {

// —— 领域行类型（LAY-010 拆分，按位包含于本命名空间）——
#include "store/rows_account.inc"
#include "store/rows_message.inc"
#include "store/rows_group.inc"
#include "store/rows_org.inc"
#include "store/rows_policy.inc"
#include "store/rows_visibility.inc"
#include "store/rows_contact.inc"
#include "store/rows_personal.inc"
#include "store/rows_integration.inc"


class ServerStore {
#include "store/schema.inc"
#include "store/account.inc"
#include "store/message.inc"
#include "store/archive.inc"
#include "store/assist.inc"
#include "store/org.inc"
#include "store/policy.inc"
#include "store/visibility.inc"
#include "store/integration.inc"
#include "store/model.inc"
#include "store/group.inc"
#include "store/task.inc"
#include "store/workflow.inc"
#include "store/workspace.inc"
#include "store/personal.inc"
#include "store/receipts.inc"
#include "store/file.inc"
#include "store/memo.inc"
#include "store/vault.inc"
#include "store/grouptools.inc"
#include "store/taskpull.inc"
#include "store/servers.inc"
#include "store/polls.inc"

private:
  bool ensure_schema();
  // 白名单收紧扫尾：名单非空时退掉名单外全部群（见 bot_allow_group 注）
  void bot_prune_group_membership(const std::string& name);

  // 453 满块自动封的免查缓存：-1=未加载（首次 append 时读账本）。只决定
  // 「何时调 seal」，账本真值恒以库为准——CLI 另进程 seal 只会多封，缓存
  // 偏差最多推迟一次自动封；封账后置 -1 失忆（回滚安全，下次重读）。
  std::int64_t archive_last_sealed_cache_{-1};

  sqlite3* db_{nullptr};
  std::function<void(const std::string&)> enqueue_hook_;
  void* enqueue_hook_owner_{nullptr};
};

} // namespace memex::server
