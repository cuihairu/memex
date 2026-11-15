// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：群工具框架与凭据域；由 store.hpp 按位包含，无包含卫、勿单独编译。
  // —— R25-1 群工具框架（白名单动作＋入群即授权＋动作留痕＋代理骨架）——
  // 群=授权域：管理员给群配工具白名单动作；成员 call 判定在路由层叠加
  //（群成员 file:read 继承——入群即有、退群即失），无独立授权体系。
  struct GroupToolConfig {
    std::uint64_t group_id{0};
    std::string tool;
    std::string actions_json; // JSON 数组字符串（服务端不解释动作语义）
    std::string updated_by;
    std::int64_t updated_ms{0};
  };
  // 动作留痕：谁/何时/哪个工具/什么动作/参数/结果（铁律：点击可回溯到人）
  struct GroupToolAudit {
    std::int64_t id{0};
    std::uint64_t group_id{0};
    std::string tool;
    std::string action;
    std::string actor;
    std::string params_json;
    std::string result_json; // 代理调用回包（R25-1=骨架 stub 回显）
    std::int64_t ts_ms{0};
  };
  // 工具白名单动作配置（upsert；群须存在=false）
  bool tool_set_actions(std::uint64_t group_id, const std::string& tool,
                        const std::string& actions_json,
                        const std::string& updated_by, std::int64_t ts_ms);
  // 单工具配置（无行=未配置 nullopt）
  std::optional<GroupToolConfig> tool_config(std::uint64_t group_id,
                                             const std::string& tool);
  std::vector<GroupToolConfig> tool_list(std::uint64_t group_id);
  void tool_audit_add(std::uint64_t group_id, const std::string& tool,
                      const std::string& action, const std::string& actor,
                      const std::string& params_json,
                      const std::string& result_json, std::int64_t ts_ms);
  std::vector<GroupToolAudit> tool_audit_list(std::uint64_t group_id,
                                              int limit = 200);

  // —— R25-2 CI/CD 工具（首个落在 R25-1 框架上的具体工具）——
  // 流水线定义（群内按名唯一）；红绿灯=每流水线最近一笔 run 的 status
  struct CiPipeline {
    std::uint64_t group_id{0};
    std::string name;
    std::string description;
    std::string updated_by;
    std::int64_t updated_ms{0};
  };
  // 触发留痕：谁触发/何时/哪条流水线/结果（谁触发可回溯=设计点名）
  struct CiRun {
    std::int64_t id{0};
    std::uint64_t group_id{0};
    std::string pipeline;
    std::string actor;
    std::string status; // success/failed（stub 执行器即时出结果）
    std::string params_json;
    std::string result_json;
    std::int64_t ts_ms{0};
  };
  // 流水线 upsert/删（群须存在=false；删=删到行才真）
  bool ci_pipeline_upsert(std::uint64_t group_id, const std::string& name,
                          const std::string& description,
                          const std::string& updated_by, std::int64_t ts_ms);
  bool ci_pipeline_delete(std::uint64_t group_id, const std::string& name);
  std::vector<CiPipeline> ci_pipeline_list(std::uint64_t group_id);
  // run 落账（status 即终态：stub 执行器同步完成）并回 run id
  std::int64_t ci_run_add(std::uint64_t group_id, const std::string& pipeline,
                          const std::string& actor, const std::string& status,
                          const std::string& params_json,
                          const std::string& result_json, std::int64_t ts_ms);
  // run 历史 id DESC（pipeline 空=该群全部流水线）
  std::vector<CiRun> ci_run_list(std::uint64_t group_id,
                                 const std::string& pipeline = {},
                                 int limit = 50);
  // 红绿灯面：每条流水线最近一笔 run（无 run 的流水线不出现在此）
  std::vector<CiRun> ci_status_list(std::uint64_t group_id);

  // —— R25-3 打包工具（首批动作类示例之二）——
  // 产物台账（stub 阶段=打包结果记录；真产物字节面归后续批次）
  struct PackArtifact {
    std::int64_t id{0};
    std::uint64_t group_id{0};
    std::string name;
    std::string version;
    std::string note;
    std::string created_by;
    std::int64_t created_ms{0};
  };
  // 产物落账（同 gid+name+version 重复=覆盖 note/created_by）；群须存在
  bool pack_artifact_upsert(std::uint64_t group_id, const std::string& name,
                            const std::string& version,
                            const std::string& note,
                            const std::string& created_by,
                            std::int64_t ts_ms);
  std::vector<PackArtifact> pack_artifact_list(std::uint64_t group_id);
  bool pack_artifact_delete(std::uint64_t group_id, const std::string& name,
                            const std::string& version);

  // —— R25-4 凭据面：工具外部凭据只存服务端（密文=cred::gcm_seal 的
  // hex 串，行内永不见明文；取回只给掩码元数据——客户端零凭据）——
  struct ToolCredentialMeta {
    std::uint64_t group_id{0};
    std::string tool;
    std::string updated_by;
    std::int64_t updated_ms{0};
  };
  // 上/覆盖（同 gid+tool）；群须存在=false
  bool tool_cred_set(std::uint64_t group_id, const std::string& tool,
                     const std::string& sealed_hex,
                     const std::string& updated_by, std::int64_t ts_ms);
  bool tool_cred_delete(std::uint64_t group_id, const std::string& tool);
  // 密文取回（仅供服务端代理调用内存内解密——不暴露给任何 HTTP 面）
  std::optional<std::string> tool_cred_sealed(std::uint64_t group_id,
                                              const std::string& tool);
  // 掩码清单（无密文字段）
  std::vector<ToolCredentialMeta> tool_cred_list(std::uint64_t group_id);
