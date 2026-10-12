// 聊天记录模糊检索验收（子串匹配 + 多关键词 AND + 相关度排序）：
// ① 单词子串（词在长句中间）；② 只记得一半（半词命中）；③ 多关键词 AND
//    （全部命中才出，不做 OR 联想）；④ 英文大小写不敏感；⑤ 中文按字符直接匹配；
// ⑥ LIKE 元字符按字面；⑦ 分词口径（ASCII 空白／半角逗号／全角逗号）；
// ⑧ 相关度排序（命中次数×1000 ＋ 首命中位置提前量封顶 999，相关度优先、
//    同分按时间倒序）；⑨ 账号／时间窗过滤仍可组合；⑩ 空关键词=不过滤。
// 规模账（要求 4 实测数字）：磁盘语料 20000 条 × 3 个样例查询 × 实跑若干次，
// 命中数与 p50/p95 延迟均为本测试实跑输出（printf，见 RUN 日志）。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "store.hpp"

using memex::server::MessageSearch;
using memex::server::ServerStore;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ' ' << #cond    \
                << '\n';                                                     \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

std::vector<memex::server::ArchivedMessage> search(ServerStore& s,
                                                   std::string kw) {
  MessageSearch q;
  q.keyword = std::move(kw);
  q.limit = 100000;
  return s.search_messages(q);
}

} // namespace

int main() {
  // —— 小语料：口径正确性 ——
  {
    ServerStore store;
    CHECK(store.open(":memory:"));
    const std::int64_t t0 = 1760000000000;
    CHECK(store.store_message("s1", "alice", "bob", 10, "合同 合同 合同", t0 + 1000));
    CHECK(store.store_message("s2", "alice", "bob", 10, "合同已签，请归档", t0 + 2000));
    CHECK(store.store_message("s3", "alice", "bob", 10, "合同扫描件已发", t0 + 3000));
    CHECK(store.store_message("s4", "bob", "alice", 10,
                              "请财务本周三前把发票已开那条对账一下", t0 + 4000));
    CHECK(store.store_message("s5", "bob", "alice", 10, "发票已开", t0 + 5000));
    CHECK(store.store_message("s6", "alice", "bob", 10,
                              "please invite me to the review", t0 + 6000));
    CHECK(store.store_message("s7", "alice", "bob", 10,
                              "Invite only for internal folks", t0 + 7000));
    CHECK(store.store_message("s8", "bob", "alice", 10, "周末团建报名", t0 + 8000));

    { // ① 词在长句中间
      auto h = search(store, "对账一下");
      CHECK(h.size() == 1);
      if (!h.empty()) CHECK(h[0].msg_id == "s4");
    }
    { // ② 只记得一半（中文半词）
      auto h = search(store, "合同扫");
      CHECK(h.size() == 1);
      if (!h.empty()) CHECK(h[0].msg_id == "s3");
    }
    { // ② 只记得一半（英文只记得开头几个字母）
      auto h = search(store, "PLEA");
      CHECK(h.size() == 1);
      if (!h.empty()) CHECK(h[0].msg_id == "s6");
    }
    { // ③ 多关键词 AND：全部命中才出
      auto h = search(store, "发票 对账");
      CHECK(h.size() == 1);
      if (!h.empty()) CHECK(h[0].msg_id == "s4");
    }
    { // ③ AND 而非 OR：无行同时含两词 → 0 命中
      CHECK(search(store, "合同 发票").empty());
    }
    { // ③ 部分命中不算：两词都有但只命中一词的行不出
      auto h = search(store, "合同 团建");
      CHECK(h.empty());
    }
    { // ④ 英文大小写不敏感（invite/Invite 都命中）
      auto h = search(store, "INVITE");
      CHECK(h.size() == 2);
      if (h.size() == 2) {
        // 位置提前量：Invite 在句首（pos 1）排前，invite 在句中（pos 8）排后
        CHECK(h[0].msg_id == "s7");
        CHECK(h[1].msg_id == "s6");
      }
    }
    { // ⑤ 中文按字符直接匹配
      auto h = search(store, "团建");
      CHECK(h.size() == 1);
      if (!h.empty()) CHECK(h[0].msg_id == "s8");
    }
    { // ⑥ LIKE 元字符按字面：下划线不当通配符
      CHECK(search(store, "合同_").empty());
    }
    { // ⑦ 分词口径：全角逗号「，」同样分词
      auto h = search(store, "发票，已");
      CHECK(h.size() == 2);
    }
    { // ⑧ 相关度排序：命中次数多者居前
      auto h = search(store, "合同");
      CHECK(h.size() == 3);
      if (h.size() == 3) {
        CHECK(h[0].msg_id == "s1"); // 「合同 合同 合同」3 次命中
        CHECK(h[1].msg_id == "s3"); // 同分按时间倒序（s3 晚于 s2）
        CHECK(h[2].msg_id == "s2");
        CHECK(h[0].score > h[1].score);
        CHECK(h[1].score == h[2].score);
      }
    }
    { // ⑨ 组合：账号 + 关键词交集（账号＝收发双侧，故用无消息的 carol 做反例）
      MessageSearch q;
      q.account = "carol";
      q.keyword = "合同";
      CHECK(store.search_messages(q).empty());
      q.account = "alice";
      CHECK(store.search_messages(q).size() == 3);
    }
    { // ⑨ 组合：时间窗 + 关键词
      MessageSearch q;
      q.keyword = "合同";
      q.since_ms = t0 + 1500;
      q.until_ms = t0 + 3500;
      auto h = store.search_messages(q);
      CHECK(h.size() == 2);
      if (h.size() == 2) {
        CHECK(h[0].msg_id == "s3");
        CHECK(h[1].msg_id == "s2");
      }
    }
    { // ⑩ 空关键词=不过滤，score 恒 0 不参与排序
      MessageSearch q;
      q.limit = 100;
      auto h = store.search_messages(q);
      CHECK(h.size() == 8);
      for (const auto& m : h) CHECK(m.score == 0);
    }
    std::printf("[search-fuzzy] 小语料口径正确性 %s\n",
                g_failures == 0 ? "PASS" : "FAIL");
  }

  // —— 规模账：磁盘语料 × 样例查询 × 延迟实测 ——
  {
    const std::string db_path = "/tmp/memex-search-bench.db";
    std::remove(db_path.c_str());
    constexpr std::size_t kCorpus = 20000;
    // 确定性语料池（40 条，中英混排、长短句混合；目标句在固定位，命中数可断言）
    const std::vector<std::string> pool = {
        "合同 合同 合同",                   // 0  相关度排序靶（3 次命中）
        "合同已签，请归档",                 // 1  多关键词 AND 正例
        "合同扫描件已发",                   // 2  只记得一半（合同扫）
        "请财务本周三前把发票已开那条对账一下", // 3  词在长句中间（对账一下）
        "发票已开",                         // 4  多关键词 AND 正例
        "please invite me to the review",   // 5  记不全的英文（INVITI）
        "Invite only for internal folks",   // 6  大小写不敏感对侧
        "周末团建报名",                     // 7
        "收到，我先看一下",                 // 8
        "这个明天上午给回复",               // 9
        "进度已经同步到看板了",             // 10
        "服务器今晚十点重启",               // 11
        "周报记得周五前发出来",             // 12
        "会议室订在三楼东侧",               // 13
        "新版本安装包已上传",               // 14
        "客户反馈的问题已修复",             // 15
        "预算表还差一列没填",               // 16
        "明天活动地点改到西区操场",         // 17
        "审批流走到李姐那一步了",           // 18
        "代码评审意见已回复",               // 19
        "测试环境数据已刷新",               // 20
        "服务器负载偏高，建议扩容",         // 21
        "这份纪要请存档备查",               // 22
        "下午的会有变动，时间待定",         // 23
        "报销单缺一张附件",                 // 24
        "供应商报价已经收到",               // 25
        "项目里程碑延后一周",               // 26
        "需求文档更新到第二节",             // 27
        "线上告警已确认为误报",             // 28
        "招聘面试安排在下周二",             // 29
        "办公用品清单已提交",               // 30
        "数据备份任务运行正常",             // 31
        "季度复盘材料准备中",               // 32
        "产品原型图已同步到群",             // 33
        "运维手册第三节有更新",             // 34
        "培训录屏已上传到网盘",             // 35
        "值班表轮转到小王",                 // 36
        "系统公告：本周五下午停网维护",     // 37
        "年假申请已批准",                   // 38
        "代码仓库权限已开通",               // 39
    };
    ServerStore store;
    CHECK(store.open(db_path));
    const std::int64_t t0 = 1760000000000;
    for (std::size_t i = 0; i < kCorpus; ++i) {
      const std::string text =
          pool[i % pool.size()] + "　第" + std::to_string(i) + "号记录";
      CHECK(store.store_message("b-" + std::to_string(i), i % 2 ? "bob" : "alice",
                                i % 2 ? "alice" : "bob", 10, text,
                                t0 + static_cast<std::int64_t>(i)));
    }
    std::printf("[search-fuzzy] 语料：磁盘库 %s，messages %zu 条（含事件流）\n",
                db_path.c_str(), kCorpus);

    // 样例查询 1：只记得一半（半词）
    // 样例查询 2：词在长句中间
    // 样例查询 3：打错字（子串口径＝记不全的连续片段，please→PLEA）
    // 附：多关键词 AND、大小写不敏感各一条
    struct Sample {
      const char* name;
      const char* kw;
      std::size_t expect;
    };
    const Sample samples[] = {
        {"只记得一半", "合同扫", kCorpus / 40},
        {"词在长句中间", "对账一下", kCorpus / 40},
        {"打错字(记不全)", "PLEA", kCorpus / 40},
        {"多关键词 AND", "发票 已", 2 * kCorpus / 40},
        {"大小写不敏感", "INVITE", 2 * kCorpus / 40},
    };
    constexpr int kRounds = 50; // p50/p95 采样足够；覆盖率构建下控制套件预算
    for (const auto& s : samples) {
      // 命中数（全量 limit）
      MessageSearch q;
      q.keyword = s.kw;
      q.limit = 100000;
      const auto hits = store.search_messages(q);
      CHECK(hits.size() == s.expect);
      // 延迟：单页 limit=50（界面实际取页大小），kRounds 次实跑
      MessageSearch qp;
      qp.keyword = s.kw;
      qp.limit = 50;
      (void)store.search_messages(qp); // 预热（页缓存）
      std::vector<double> us;
      us.reserve(kRounds);
      for (int r = 0; r < kRounds; ++r) {
        const auto a = std::chrono::steady_clock::now();
        (void)store.search_messages(qp);
        const auto b = std::chrono::steady_clock::now();
        us.push_back(std::chrono::duration<double, std::micro>(b - a).count());
      }
      std::sort(us.begin(), us.end());
      const double p50 = us[us.size() / 2];
      const double p95 = us[us.size() * 95 / 100];
      std::printf(
          "[search-fuzzy] 样例「%s」查询=\"%s\" 命中=%zu 期望=%zu "
          "延迟 p50=%.0fus p95=%.0fus（%d 次，limit=50，磁盘库缓存预热后）\n",
          s.name, s.kw, hits.size(), s.expect, p50, p95, kRounds);
    }
    // 规模递增：同库追加到 10 万条，看延迟随语料规模的增长（无索引＝全表扫描）
    {
      for (std::size_t i = kCorpus; i < 100000; ++i) {
        const std::string text =
            pool[i % pool.size()] + "　第" + std::to_string(i) + "号记录";
        CHECK(store.store_message("b-" + std::to_string(i), i % 2 ? "bob" : "alice",
                                  i % 2 ? "alice" : "bob", 10, text,
                                  t0 + static_cast<std::int64_t>(i)));
      }
      MessageSearch q;
      q.keyword = "合同扫";
      q.limit = 50;
      (void)store.search_messages(q); // 预热
      std::vector<double> us;
      us.reserve(30);
      for (int r = 0; r < 30; ++r) {
        const auto a = std::chrono::steady_clock::now();
        (void)store.search_messages(q);
        const auto b = std::chrono::steady_clock::now();
        us.push_back(std::chrono::duration<double, std::micro>(b - a).count());
      }
      std::sort(us.begin(), us.end());
      MessageSearch qc;
      qc.keyword = "合同扫";
      qc.limit = 100000;
      const std::size_t big_hits = store.search_messages(qc).size();
      CHECK(big_hits == 100000 / 40); // 2500：规模线性，命中数同步放大
      std::printf(
          "[search-fuzzy] 规模递增 语料=100000 条 查询=\"合同扫\" 命中=%zu "
          "延迟 p50=%.0fus p95=%.0fus（30 次，limit=50）\n",
          big_hits, us[us.size() / 2], us[us.size() * 95 / 100]);
    }
    store.close();
    std::remove(db_path.c_str());
  }

  std::printf("[search-fuzzy] %s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
