// collab_engine 拆分共性面（LAY-013 自 collab_engine.cpp 头部逐字搬移）：
// include 块＋协议 using＋消息 id 本地助手（inline 化）供各域模块；
// 方法声明全部在 collab_engine.hpp。
#pragma once

#include "../collab_engine.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QSysInfo>

#include <algorithm>

#include <nlohmann/json.hpp>

#include <memex/protocol/messages.hpp>
#include <core/local_store.hpp>

#ifndef MEMEX_VERSION
#define MEMEX_VERSION "dev"
#endif

namespace memex::client {

using memex::protocol::Message;
using memex::protocol::MsgType;

// 客户端本地推定式（服务端常规同式 session.cpp）：sha256_hex(from + ":" + seq)。
// 发出即回填本地行；服务端撞 id 换盐重排时经回执回带最终值覆盖（BUG-007
// §4.3），送达/已读回执、撤回按本地行 msg_id 命中。
inline QString derive_msg_id(const QString& account, quint64 seq) {
  return CollabEngine::msg_id_for(account, seq);
}

// 发送方小整数计数器，补齐行序号从 2^62 起计——两段永不相交，杜绝
// 补齐行占位把同 (from_id, seq) 的后续实时投递行静默 IGNORE 丢行。
inline constexpr std::uint64_t kGapSeqBase = 1ULL << 62;

} // namespace memex::client
