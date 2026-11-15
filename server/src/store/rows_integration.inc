// store.hpp 行类型片段（LAY-010 自 store.hpp 逐字搬移）：webhook/bot/模型网关行；由 store.hpp 按位包含，无包含卫、勿单独编译。
// webhook 接入台账（T4.10）：按群／个人独立——target="group:<群号>" 或账号。
// token 明文仅 create 时输出一次，库内存 sha256 摘要；revoked=1 即吊销拒收。
struct WebhookRow {
  std::int64_t id{0};
  std::string token_hash;
  std::string target;
  std::string name; // 备注（CLI --name，可空）
  std::int64_t created_ms{0};
  bool revoked{false};
};

// 机器人（bot）台账：name 不含 "bot:" 前缀（全名= "bot:"+name 伪账号，
// 直插 group_members/offline_messages——两表无外键，零迁移）。token 明文
// 仅 add 时输出一次，库存 sha256 摘要；disabled=1 即 403 拒收发。
struct BotRow {
  std::string name;
  std::string token_hash;
  std::string created_by;
  std::int64_t created_ms{0};
  bool disabled{false};
};

// 模型端点（平台三期·模型网关）：上游 OpenAI 兼容服务登记项。注册序
// 即路由优先序；is_local=归档数据红线专用（仅本地模型——规则写死在
// 网关代码不做成配置）；api_key 库存原文（须可逆代发上游，与验自身
// token 的摘要口径不同）。
struct ModelEndpointRow {
  std::int64_t id{0};
  std::string name;    // 路由名（请求 model 字段命中即提优先）
  std::string base_url; // http://host[:port][/prefix]（仅 http，内网口径）
  std::string api_key; // 上游 Bearer（可空=上游不鉴权）
  std::string model;   // 上游模型名（空=透传请求里的 model）
  bool is_local{false};
  bool enabled{true};
  std::string created_by;
  std::int64_t created_ms{0};
};

// 模型调用审计：一次上游尝试一行（降级即多行，可对账）。只记元数据
// 不记 prompt/completion 正文（正文敏感；status 0=连接失败/超时）。
struct ModelCallRow {
  std::int64_t id{0};
  std::string caller; // bot:<name>（网关 Bearer 主体）
  std::string endpoint;
  std::string model;
  bool archive_scope{false};
  int prompt_chars{0};
  int completion_chars{0};
  int status{0};
  std::int64_t latency_ms{0};
  std::int64_t created_ms{0};
};
