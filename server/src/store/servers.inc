// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：服务器 agent 域；由 store.hpp 按位包含，无包含卫、勿单独编译。
  // —— R26-1 服务器 agent 面：公用服务器登记（agent 凭注册令牌心跳，
  // 令牌只存 SHA-256 摘要——明文只在登记回包出现一次；指标=最近一拍，
  // 红绿灯=last_seen 新鲜度由路由层判）——
  struct GroupServerRow {
    std::uint64_t id{0};
    std::uint64_t group_id{0};
    std::string name;
    std::string host;
    std::string enrolled_by;
    std::int64_t created_ms{0};
    std::int64_t last_seen_ms{0};
    double cpu_percent{-1.0}; // -1=尚未上报过
    double mem_used_mb{0};
    double mem_total_mb{0};
    double disk_used_mb{0};
    double disk_total_mb{0};
    double load1{0};
    // R26-4 凭据掩码元数据（空/0=未配置；密文永不在此行）
    std::string cred_updated_by;
    std::int64_t cred_updated_ms{0};
  };
  // 登记（token_hash=SHA-256 hex；同 gid+name 幂等复用行并轮换令牌）；
  // 群须存在——0=群不存在/失败
  std::uint64_t server_enroll(std::uint64_t group_id, const std::string& name,
                              const std::string& host,
                              const std::string& token_hash,
                              const std::string& enrolled_by,
                              std::int64_t ts_ms);
  std::optional<GroupServerRow> server_by_token_hash(
      const std::string& token_hash);
  bool server_heartbeat(std::uint64_t id, double cpu_percent,
                        double mem_used_mb, double mem_total_mb,
                        double disk_used_mb, double disk_total_mb,
                        double load1, std::int64_t ts_ms);
  std::vector<GroupServerRow> server_list(std::uint64_t group_id);

  // —— R26-3 远程会话面：一次性短票＋接入留痕（谁/何时/连哪台/协议/
  // 是否兑现/何时结束——时长=closed_ms-opened_ms 由路由层算）——
  struct ServerSessionRow {
    std::uint64_t id{0};
    std::uint64_t group_id{0};
    std::uint64_t server_id{0};
    std::string server_name;
    std::string host;
    std::string actor;
    std::string protocol; // ssh 起步（RDP/VNC 随后）
    std::int64_t opened_ms{0};
    std::int64_t redeemed_ms{0}; // 0=短票未兑现
    std::int64_t closed_ms{0};   // 0=进行中
  };
  // 开会话（留痕起点；ticket_hash=SHA-256 hex）——server_id 须属该群，
  // 不属=0
  std::uint64_t server_session_open(std::uint64_t group_id,
                                    std::uint64_t server_id,
                                    const std::string& actor,
                                    const std::string& protocol,
                                    const std::string& ticket_hash,
                                    std::int64_t ts_ms);
  // 短票兑现（一次性）：回会话行；查无=行内已兑现=过期的按路由层校验
  std::optional<ServerSessionRow> server_session_by_ticket(
      const std::string& ticket_hash);
  bool server_session_mark_redeemed(std::uint64_t id, std::int64_t ts_ms);
  // 关会话（时长终点）：行须属该群该人且进行中
  bool server_session_close(std::uint64_t group_id, std::uint64_t id,
                            const std::string& actor, std::int64_t ts_ms);
  std::vector<ServerSessionRow> server_session_list(std::uint64_t group_id);

  // —— R26-4 服务器凭据面：目标机接入凭据只存服务端（密文=cred::gcm_seal
  // 的 hex 串；客户端零凭据——memex 只做「看+连」，真用凭据的操作归
  // croupier）——
  struct ServerCredentialMeta {
    std::uint64_t server_id{0};
    std::string updated_by;
    std::int64_t updated_ms{0};
  };
  // 上/覆盖（同 server_id）；服务器须属该群=false
  bool server_cred_set(std::uint64_t group_id, std::uint64_t server_id,
                       const std::string& sealed_hex,
                       const std::string& updated_by, std::int64_t ts_ms);
  // 删（服务器须属该群）；无行=false
  bool server_cred_delete(std::uint64_t group_id, std::uint64_t server_id);
  // 密文取回（仅供服务端代理调用内存内解密——不暴露给任何 HTTP 面）
  std::optional<std::string> server_cred_sealed(std::uint64_t server_id);
  // 掩码清单（按群走 JOIN scope；无密文字段）
  std::vector<ServerCredentialMeta> server_cred_list(std::uint64_t group_id);
