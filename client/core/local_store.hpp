// 共享内核：本地消息库（SQLite）。
// 归档铁律在本地库的体现：source 字段区分「直连（仅存本机）」与「协作（另有服务端归档）」，
// 本地删除只影响本机，不涉及服务端归档。
#pragma once

#include <QList>
#include <QSqlRecord>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <memory>
#include <string>

namespace memex::client {

struct StoredMessage {
  qint64 id{0};
  std::uint64_t seq{0};
  std::string peer;   // 会话对端（查询维度）
  std::string from;
  std::string to;
  qint64 ts_ms{0};
  std::string text;
  std::string source; // "direct" 或 "collab"
  // 消息标识（协作态：服务端分配；直连态为空）。按其去重：离线补投重复
  // 投递、断线补传重传同一消息都不产生第二条本地记录。
  std::string msg_id;
  // 撤回标记（仅界面展示用，原文保留在本地与服务端归档）。
  bool recalled{false};
  // 平台-9 同步状态机（蓝图§二十三）：LOCAL（本地域，不进服务端同步，
  // 直连态恒此值）/ PENDING（待发送）/ SENDING（在途）/ SERVER_ACKED
  // （服务端受理，已归档待投递）/ ARCHIVED（服务端归档确认：接收方向
  // 落库即此值——唯一入口是服务端投递；发送方向经 DELIVER_NOTICE/
  // RECEIPT_DATA 触发面自 SERVER_ACKED 推进）/ FAILED（终态失败，不再
  // 补传）。空串＝历史行（状态机启用前的旧数据），不参与恢复补传。
  std::string sync_state;
  // 回执态（需求批⑦）：''=已发送（落库在途，未获对端确认）/ delivered=
  // 已送达对方客户端 / read=已读（终态）。仅我发出的消息有意义：协作域
  // 按 msg_id 命中（set_receipt），直连域按 (收件人, 发送序号) 命中
  // （set_receipt_for_seq，2026-10-14 已读灰勾单）。接收方向恒空。
  std::string receipt;
  // 模糊检索相关度（仅关键词查询时非 0；无关键词=0，不参与排序）：
  // Σ(命中次数×1000 + 首命中位置提前量封顶 999)。见 search_messages。
  std::int64_t score{0};
};

// —— R23-5 杀毒扫描白名单（本地侧）——
// 允许的扩展名/MIME 小写；max_file_size=0 表示不限制。
struct ScanWhitelist {
  QStringList allowed_extensions;
  QStringList allowed_mime_types;
  qint64 max_file_size{0};
};

// 扫描结论枚举（与 scan_results.result 列一一对应，勿改已落库值）。
enum class ScanResult { Unknown = 0, Clean, Quarantined, Error };

// —— 「盯」提醒（todo「盯」单·实现单修订 2026-10-14）——
// 本端对一条我发出消息的已读订阅：键与回执同口径（直连 d:<收件人>:<序号>
// ／协作 c:<msg_id>），落库持久（重启仍有效）。三通道解除：已读触发即解
// ／手动取消／超时（默认 24h）静默过期。只存本机，不对端发任何被盯信号。
struct WatchedRow {
  std::string key;    // d:<收件人>:<发送序号> ／ c:<协作 msg_id>
  std::string peer;   // 会话对端（提醒文案与列表高亮用）
  std::string source; // "direct" / "collab"
  std::string text;   // 盯时消息文本（提醒预览）
  qint64 created_ms{0};
  bool reminded{false}; // 未读超阈值提醒已发过（只一次，不循环）
};

class LocalStore {
public:
  LocalStore() = default;
  ~LocalStore();

  LocalStore(const LocalStore&) = delete;
  LocalStore& operator=(const LocalStore&) = delete;

  // path 支持 ":memory:"（测试）与普通文件路径。重复 open 为幂等失败。
  bool open(const QString& path);
  void close();
  bool is_open() const { return open_; }

  // 幂等插入（from+seq 与 msg_id 唯一约束去重）。
  // inserted 非空时回报是否真正插入（false=命中已有记录，重复消息）。
  bool append(const StoredMessage& msg, bool* inserted = nullptr);

  // 服务端起源消息（如通知，无发送方会话 seq）本地分配单调 seq——满足
  // UNIQUE(from_id, seq)；离线重投的去重仍走 msg_id 唯一索引。
  qint64 next_local_seq(const std::string& from_id);
  // 查 (from_id, seq) 槽位上已落库行的 msg_id（BUG-007 §4.2：接收方向据此
  // 分辨「同槽位重投」与「重排新消息」）。无该槽位行返回空串。
  std::string msg_id_at(const std::string& from_id, std::uint64_t seq);

  // 按 msg_id 置撤回标记（服务端撤回事件到达后调用）。
  bool mark_recalled(const std::string& msg_id);

  // 平台-9 同步状态机：按发送方唯一键 (from_id, seq) 推移状态；
  // pending_sync 取某账号尚在 PENDING/SENDING 的消息（重启恢复补传依据）。
  bool set_sync_state(const std::string& from_id, std::uint64_t seq,
                      const std::string& state);
  QList<StoredMessage> pending_sync(const std::string& from_id) const;

  // 平台-9 发送侧归档推进：接收方客户端 ACK（已收取落库）经服务端
  // DELIVER_NOTICE／RECEIPT_DATA 到达后，把发送侧 SERVER_ACKED 行推进
  // ARCHIVED（状态机收口时服务端尚无归档确认事件、发送侧以 SERVER_ACKED
  // 为终态的留尾，回执面补齐后按 msg_id 推进）。只从 SERVER_ACKED 推进
  //（接收方向行本就 ARCHIVED、PENDING/SENDING/FAILED 不动）；未知
  // msg_id 返回 false。
  bool set_sync_state_archived(const std::string& msg_id);

  // —— 需求批⑦ 消息回执 ——
  // 发出消息回填服务端标识（sha256(account:seq) 本地推定，只补空位）。
  bool set_msg_id(const std::string& from_id, std::uint64_t seq,
                  const std::string& msg_id);
  // 服务端回执回带最终 msg_id 时覆盖本地推定值（BUG-007 §4.3：撞 id 换盐
  // 重排后的新值）——与 set_msg_id 的「只补空位」相对，此法强制覆盖，此后
  // 送达/已读回执、撤回按新 id 命中。
  bool set_msg_id_final(const std::string& from_id, std::uint64_t seq,
                        const std::string& msg_id);
  // 我发出消息的回执态（receipt 列）：state=delivered/read，只升不降
  //（已读终态不被乱序送达通知倒退）。非我发出或未知 msg_id 返回 false。
  bool set_receipt(const std::string& msg_id, const std::string& state);
  // 直连域同口径（需求批⑦·已读灰勾单 2026-10-14）：直连消息无服务端
  // msg_id，我发出的行以 (收件人 to_id, 发送序号 seq) 唯一——对端已读
  // 上报按此键命中。仅 source='direct' 行参与，防与协作域序号串扰。
  bool set_receipt_for_seq(const std::string& to_id, std::uint64_t seq,
                           const std::string& state);
  // 回执态查询（「盯」巡检用）：按两域键读某条我发出消息的 receipt；
  // 无该行返回空串（与"已发送在途"不可区分——盯的解除只认非空 read）。
  std::string receipt_of_seq(const std::string& to_id,
                             std::uint64_t seq) const;
  std::string receipt_of_msg_id(const std::string& msg_id) const;

  // —— 「盯」提醒（todo「盯」单·实现单修订 2026-10-14）——
  // 盯住表 CRUD：add 幂等（重盯同键＝整行覆盖刷新）；remove 三通道解除
  // 共口；reminded 打点保未读提醒只一次；watched_all 启动装载与巡检用。
  bool add_watch(const WatchedRow& w);
  bool remove_watch(const std::string& key);
  bool mark_watch_reminded(const std::string& key);
  QList<WatchedRow> watched_all() const;

  // 某对端的本地历史：取最近 limit 条，按时间正序返回。
  QList<StoredMessage> history(const QString& peer, int limit = 200) const;
  // 按日期范围的历史（需求批⑧）：时间窗毫秒含端点（≤0=该侧不限），
  // 窗内最近 limit 条正序返回。走既有 idx_messages_peer_ts(peer, ts_ms)
  // 复合索引（本地索引面），与 history 同构。
  QList<StoredMessage> history_between(const QString& peer, qint64 from_ms,
                                       qint64 until_ms, int limit = 200) const;
  // 模糊检索（多词子串 AND + 相关度排序）：分词（ASCII 空白／半角逗号／
  // 全角逗号）后每词独立子串命中并 AND 组合（全部命中才出）；英文大小写
  // 不敏感、中文按字符直接匹配；命中行按相关度倒序、同分按时间倒序
  // （相关度优先）。口径与服务端 ServerStore::search_messages 一致。
  // keyword 为空=不过滤（返回空表）。
  QList<StoredMessage> search_messages(const QString& keyword,
                                       int limit = 200) const;
  // 有历史的对端列表（按最近消息时间倒序）。source 过滤：
  // 空=全部，"collab"=协作会话（T2.4 会话列表），"direct"=直连会话。
  QStringList peers(const QString& source = {}) const;

  // —— R23-1 存储抽象层（客户端元数据层）——
  // 文件增删改查与配额只落本地 SQLite；对象字节经 memex server 走，
  // 客户端永不直连对象存储。status：0=正常。
  bool add_file(const QString& owner, const QString& belong_gid,
                const QString& belong_uid, const QString& file_name,
                qint64 file_size, const QString& file_hash,
                const QString& object_key, const QString& source);
  // 某属主的文件列表（belong_gid 为空=全部归属；否则按群过滤）。
  QList<QSqlRecord> file_list(const QString& owner,
                              const QString& belong_gid = {}) const;
  // 配额用量写（无行则建行；覆盖式更新，调用方先读后算）。
  bool update_group_quota(const QString& gid, qint64 used_bytes);
  bool update_user_quota(const QString& uid, qint64 used_bytes);

  // —— R23-3 文件助手（备忘录+文件传输统一收件箱，仅本人可见）——
  // 外网来的文件需内网人工转发后方可记录；status：0=normal、1=read、
  // 2=deleted（mark_* 更新）。
  bool add_helper_record(const QString& owner, const QString& memo_text,
                         const QString& file_hash, qint64 file_size,
                         const QString& object_key);
  // 某属主的收件箱记录（按上传时间倒序）。
  QList<QSqlRecord> helper_list(const QString& owner) const;
  bool mark_helper_read(const QString& record_id);
  bool mark_helper_deleted(const QString& record_id);

  // —— R23-5 杀毒扫描勾子 + 白名单（本地侧）——
  // 白名单与扫描状态由服务端最终裁决（权限层），本地仅公开写入接口与
  // 查询历史（record_id 为表主键的字符串形式）。
  bool set_scan_whitelist(const ScanWhitelist& whitelist);
  // 空指针=无白名单记录。
  std::unique_ptr<ScanWhitelist> get_scan_whitelist();
  bool record_scan(const QString& file_hash, ScanResult result,
                   qint64 file_size);
  QList<QSqlRecord> scan_history(const QString& file_hash) const;

  // —— 平台-8 直连安全：设备身份与对端定针（TOFU）——
  // 身份种子只存本机库（不出日志不进网络，对外只有公钥）；
  // 定针＝首触记录对端身份公钥，此后不符即拒（换钥/冒充显式暴露）。
  // 读身份（seed/pub 均 64 hex）；无记录或损坏返回 false。
  bool read_identity(std::string* seed_hex, std::string* pub_hex);
  // 落身份（INSERT OR IGNORE——并发首跑先入者为准，读方回读实际行）。
  bool save_identity(const std::string& seed_hex, const std::string& pub_hex);
  // 对端定针查询：无记录返回空串。
  std::string peer_identity_pub(const std::string& device_id);
  // 首触定针（IGNORE：已有记录不覆盖——防身份静默漂移与改写）。
  bool pin_peer_identity(const std::string& device_id,
                         const std::string& pub_hex);
  // 清除定针（对端合法换钥时显式重置，重触再定针）。
  bool clear_peer_identity(const std::string& device_id);

private:
  bool ensure_schema();

  QString connection_name_;
  bool open_{false};
};

} // namespace memex::client
