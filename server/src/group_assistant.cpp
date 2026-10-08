#include "group_assistant.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include <memex/protocol/messages.hpp>

#include "model_gateway.hpp" // http_post_json（网关回环调用）

namespace memex::server {

namespace {

using json = nlohmann::json;

// —— 指令词表（前缀写死，中文产品口径）——
constexpr const char* kCmdAsk = "@助手";
constexpr const char* kCmdMinutes = "@纪要";
constexpr const char* kCmdSpec = "@整理";
constexpr const char* kCmdSearch = "@检索";

// 上下文/资料条数上限（写死：内网口径，防 prompt 失控）
constexpr int kAskContextCount = 20;
constexpr int kMinutesContextCount = 50;
constexpr int kSearchHitLimit = 10;

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 指令参数去首尾空白（"@检索 立项"的参数若带前导空格，LIKE 子串检索
// 会静默零命中）
std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// 简版阻塞 HTTP POST（回环 /bot/send 专用：本机 webhook 口，无前缀，
// Connection: close 读至 EOF）。带 5s 死线：serve 收尾期 webhook 的
// io 已停、acceptor 不再 accept——无死线会让 worker.stop() 卡死进程
// 退出。失败/超时返回 0。
int post_loopback(std::uint16_t port, const std::string& path,
                  const std::string& bearer_token, const std::string& body) {
  try {
    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    asio::steady_timer deadline(io);
    bool done = false;
    int status = 0;
    const auto finish = [&]() {
      if (done) return;
      done = true;
      deadline.cancel();
    };
    deadline.expires_after(std::chrono::seconds(5));
    deadline.async_wait([&](std::error_code ec) {
      if (ec) return;
      if (!done) {
        done = true;
        std::error_code ignore;
        sock.close(ignore);
      }
    });
    sock.async_connect(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port),
        [&](std::error_code ec) {
          if (done) return;
          if (ec) {
            finish();
            return;
          }
          std::ostringstream req;
          req << "POST " << path << " HTTP/1.1\r\n"
              << "Host: 127.0.0.1\r\n"
              << "Authorization: Bearer " << bearer_token << "\r\n"
              << "Content-Type: application/json\r\n"
              << "Content-Length: " << body.size() << "\r\n"
              << "Connection: close\r\n\r\n"
              << body;
          const auto bytes = std::make_shared<const std::string>(req.str());
          asio::async_write(
              sock, asio::buffer(*bytes),
              [&, bytes](std::error_code ec, std::size_t) {
                if (done) return;
                if (ec) {
                  finish();
                  return;
                }
                auto resp = std::make_shared<std::string>();
                asio::async_read(
                    sock, asio::dynamic_buffer(*resp),
                    [&, resp](std::error_code ec, std::size_t) {
                      if (done) return;
                      if (ec && ec != asio::error::eof) {
                        finish();
                        return;
                      }
                      if (resp->rfind("HTTP/1.1 ", 0) == 0) {
                        status =
                            std::atoi(resp->c_str() + strlen("HTTP/1.1 "));
                      }
                      finish();
                    });
              });
        });
    io.run();
    if (!done) return 0; // 死线触发路径
    return status;
  } catch (const std::exception&) {
    return 0;
  }
}

// 回群/回人（sender=bot:<name> 由 /bot/send 鉴权推导；target=群键或账号）
bool send_as_bot(std::uint16_t port, const std::string& bot_token,
                 const std::string& target, const std::string& text) {
  json j;
  j["target"] = target;
  j["text"] = text;
  const int status =
      post_loopback(port, "/bot/send", bot_token, j.dump());
  return status == 200;
}

// 所在群最近消息（归档检索的账号过滤天然含「所在群全部群消息」，按
// to_account 再筛出目标群；上限 200 条内取最近 limit 条）
struct CtxLine {
  std::string sender;
  std::string text;
};
std::vector<CtxLine> group_context(ServerStore& store,
                                   const std::string& group_key, int limit) {
  std::vector<CtxLine> out;
  const auto rows = store.search_messages(
      {"", "", 0, 0, 200}); // 空账号=全部——按群键过滤
  for (auto it = rows.rbegin(); it != rows.rend(); ++it) { // id 序=时序
    if (it->to_account != group_key) continue;
    if (it->recalled) continue;
    out.push_back({it->from_account, it->text});
    if (static_cast<int>(out.size()) >= limit) break;
  }
  return out;
}

std::string context_block(const std::vector<CtxLine>& lines) {
  if (lines.empty()) return "（该群暂无归档消息）";
  std::ostringstream os;
  for (const auto& l : lines) os << l.sender << ": " << l.text << "\n";
  return os.str();
}

// 网关对话调用（archive_scope=true——资料来自归档，红线生效）。
// 返回回包正文；失败回空串并带出错误文案。
std::string ask_gateway(const std::string& gateway_base,
                        const std::string& bot_token,
                        const std::string& system_prompt,
                        const std::string& user_prompt, std::string& why) {
  if (gateway_base.empty()) {
    why = "模型未启用（serve 须带 --model-port）";
    return "";
  }
  json req;
  req["memex_archive_scope"] = true;
  req["messages"] = json::array({
      json::object({{"role", "system"}, {"content", system_prompt}}),
      json::object({{"role", "user"}, {"content", user_prompt}}),
  });
  const UpstreamResult r = http_post_json(gateway_base, bot_token, req.dump());
  if (r.status >= 200 && r.status < 300) {
    json out = json::accept(r.body) ? json::parse(r.body) : json();
    if (out.is_object() && out.contains("choices") &&
        out["choices"].is_array() && !out["choices"].empty() &&
        out["choices"][0].is_object() &&
        out["choices"][0].contains("message") &&
        out["choices"][0]["message"].is_object() &&
        out["choices"][0]["message"].contains("content") &&
        out["choices"][0]["message"]["content"].is_string()) {
      return out["choices"][0]["message"]["content"].get<std::string>();
    }
    why = "模型回包形态异常";
    return "";
  }
  if (r.status == 503) {
    why = "模型暂不可用：" + (r.body.empty() ? r.error : r.body);
    // 红线拦截的 503 错误体带「本地」字样——原样透出即可读
    return "";
  }
  if (r.status == 502) {
    why = "模型端点全部不可用（已降级尝试）";
    return "";
  }
  why = "模型拒绝（上游 " + std::to_string(r.status) + "）" +
        (r.body.empty() ? "" : "：" + r.body.substr(0, 200));
  return "";
}

} // namespace

GroupAssistantWorker::GroupAssistantWorker(std::string bot_name,
                                           std::string bot_token,
                                           std::uint16_t webhook_port,
                                           std::string gateway_base,
                                           const std::string& db_path,
                                           int poll_ms)
    : bot_name_(std::move(bot_name)),
      self_("bot:" + bot_name_),
      bot_token_(std::move(bot_token)),
      webhook_port_(webhook_port),
      gateway_base_(std::move(gateway_base)),
      poll_ms_(poll_ms < 20 ? 20 : poll_ms) {
  if (!store_.open(db_path)) {
    throw std::runtime_error("群助手本地库打开失败：" + db_path);
  }
}

GroupAssistantWorker::~GroupAssistantWorker() { stop(); }

void GroupAssistantWorker::start() {
  thread_ = std::thread([this] { loop(); });
}

void GroupAssistantWorker::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

void GroupAssistantWorker::loop() {
  while (!stop_.load()) {
    int processed = 0;
    try {
      processed = poll_once();
    } catch (const std::exception& e) {
      std::cout << "[MEMEX] group-assistant 轮询异常：" << e.what()
                << std::endl;
    }
    (void)processed;
    // 粒度 sleep：stop 及时生效（20ms 步进至 poll_ms_）
    for (int waited = 0; waited < poll_ms_ && !stop_.load(); waited += 20) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
}

int GroupAssistantWorker::poll_once() {
  const auto blobs = store_.pending_offline(self_);
  int handled = 0;
  for (const auto& blob : blobs) {
    memex::protocol::Message m;
    if (!m.ParseFromString(blob) || m.type() != memex::protocol::v1::TEXT) {
      // 非 TEXT（同群其他 bot 经 deliver_notice 群发的 NOTICE 等；bot 面
      // 只承诺文本）：ack 丢弃——留行即永久积压（worker 只认 TEXT，行
      // 永不会被消费），丢弃无信息损失
      store_.ack_offline(m.msg_id(), self_);
      continue;
    }
    const std::string text = m.text().text();
    if (m.msg_id().empty()) {
      // 畸形信封（无 msg_id=ack 键缺失，正常链路由服务端派生必有）：
      // 处理也无法清队=每轮重处理刷群刷网关，只记日志不处理不回复
      std::cout << "[MEMEX] group-assistant 丢弃无 msg_id 信封（from="
                << m.from() << "）" << std::endl;
      continue;
    }
    const std::string from = m.from();
    const std::string to = m.to();
    const bool is_group = to.rfind("group:", 0) == 0;
    const std::string reply_target = is_group ? to : from;

    // 指令分发（前缀匹配；命中即处理，处理完 ack——至少一次语义下
    // 重复投递由 ack 幂等清队兜底）
    std::string reply;
    if (text.rfind(kCmdAsk, 0) == 0) {
      const std::string question = trim(text.substr(strlen(kCmdAsk)));
      const auto ctx = is_group
                           ? group_context(store_, to, kAskContextCount)
                           : std::vector<CtxLine>{};
      std::string why;
      const std::string answer = ask_gateway(
          gateway_base_, bot_token_,
          "你是内网群助手。仅依据「群聊资料」回答用户问题；资料不足以"
          "回答时如实说明，不要编造。回答用中文、简洁。",
          "群聊资料：\n" + context_block(ctx) + "\n问题：" + question, why);
      reply = answer.empty() ? "（助手）" + why : answer;
    } else if (text.rfind(kCmdMinutes, 0) == 0) {
      if (!is_group) {
        reply = "（助手）@纪要 请在群内使用（私聊无纪要对象）";
      } else {
        const auto ctx = group_context(store_, to, kMinutesContextCount);
        std::string why;
        const std::string answer = ask_gateway(
            gateway_base_, bot_token_,
            "你是会议记录员。把群聊记录整理成会议纪要：分「要点」「结论」"
            "「待办」三节；待办标注负责人（按发言归属推断）；中文输出。",
            "群聊记录：\n" + context_block(ctx), why);
        reply = answer.empty() ? "（助手）" + why : answer;
      }
    } else if (text.rfind(kCmdSpec, 0) == 0) {
      const std::string raw = trim(text.substr(strlen(kCmdSpec)));
      if (raw.empty()) {
        reply = "（助手）用法：@整理 <要整理的需求文本>";
      } else {
        std::string why;
        const std::string answer = ask_gateway(
            gateway_base_, bot_token_,
            "你是需求分析师。把给定内容整理成结构化需求：分「背景」「目标」"
            "「功能点」「验收标准」四节；功能点逐条编号；中文输出。",
            "待整理内容：\n" + raw, why);
        reply = answer.empty() ? "（助手）" + why : answer;
      }
    } else if (text.rfind(kCmdSearch, 0) == 0) {
      const std::string keyword = trim(text.substr(strlen(kCmdSearch)));
      if (keyword.empty()) {
        reply = "（助手）用法：@检索 <关键词>";
      } else {
        // 智能检索（检索增强）不走模型：归档命中直接回群
        const auto hits = store_.search_messages({"", keyword, 0, 0, 200});
        std::ostringstream os;
        os << "（检索）「" << keyword << "」命中 " << hits.size() << " 条";
        int shown = 0;
        for (auto it = hits.rbegin();
             it != hits.rend() && shown < kSearchHitLimit; ++it, ++shown) {
          os << "\n" << shown + 1 << ". " << it->from_account << " → "
             << it->to_account << "：" << it->text.substr(0, 80);
        }
        if (hits.size() > static_cast<std::size_t>(kSearchHitLimit)) {
          os << "\n（仅列最近 " << kSearchHitLimit << " 条）";
        }
        reply = os.str();
      }
    } else {
      // 非指令：ack 清队跳过（bot 不回声）
      store_.ack_offline(m.msg_id(), self_);
      continue;
    }

    if (send_as_bot(webhook_port_, bot_token_, reply_target, reply)) {
      std::cout << "[MEMEX] group-assistant 处理指令并回复 target="
                << reply_target << std::endl;
    } else {
      std::cout << "[MEMEX] group-assistant 回群失败（回环 /bot/send "
                   "不可达），消息已消费不重投" << std::endl;
    }
    store_.ack_offline(m.msg_id(), self_);
    ++handled;
    handled_.fetch_add(1);
  }
  return handled;
}

} // namespace memex::server
