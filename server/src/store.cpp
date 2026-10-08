#include "store.hpp"

#include "cred.hpp"

#include <nlohmann/json.hpp>

#include <sqlite3.h>

#include <chrono>
#include <algorithm>
#include <map>
#include <set>

namespace memex::server {

namespace {
std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
// 与登录校验共用同一迭代数（改值需同步重建账号表口径）
constexpr int kPbkdf2IterationsForStore = 60000;
// 部门层级：ancestor 是否为 path 本身或其上级（"公司/研发" ⊑ "公司/研发/客户端组"）
bool dept_covers(const std::string& ancestor, const std::string& path) {
  if (ancestor.empty() || path.empty()) return false;
  if (path == ancestor) return true;
  return path.size() > ancestor.size() &&
         path.compare(0, ancestor.size(), ancestor) == 0 &&
         path[ancestor.size()] == '/';
}
// 逗号分隔字段表是否含某字段（精确匹配，容忍空格）
bool csv_field_has(const std::string& csv, const std::string& field) {
  std::size_t start = 0;
  while (start <= csv.size()) {
    const std::size_t comma = csv.find(',', start);
    std::string item =
        csv.substr(start, comma == std::string::npos ? std::string::npos
                                                     : comma - start);
    const auto b = item.find_first_not_of(' ');
    const auto e = item.find_last_not_of(' ');
    if (b != std::string::npos && item.substr(b, e - b + 1) == field) return true;
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return false;
}
} // namespace

ServerStore::~ServerStore() { close(); }

bool ServerStore::open(const std::string& path) {
  close();
  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    if (db) sqlite3_close(db);
    return false;
  }
  db_ = db;
  // 模型网关（平台三期）以第二连接打开同库：写锁竞争时等 5s 而非立即 BUSY
  sqlite3_busy_timeout(db_, 5000);
  if (!ensure_schema()) {
    close();
    return false;
  }
  return true;
}

void ServerStore::close() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

bool ServerStore::ensure_schema() {
  const char* sql =
      "CREATE TABLE IF NOT EXISTS accounts ("
      "  account TEXT PRIMARY KEY,"
      "  display_name TEXT NOT NULL,"
      "  salt TEXT NOT NULL,"
      "  digest TEXT NOT NULL,"
      "  role TEXT NOT NULL DEFAULT 'member',"
      "  created_ms INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS login_records ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  account TEXT NOT NULL,"
      "  fingerprint TEXT NOT NULL,"
      "  kind TEXT NOT NULL,"
      "  name TEXT NOT NULL,"
      "  source_ip TEXT NOT NULL,"
      "  version TEXT NOT NULL,"
      "  result TEXT NOT NULL,"
      "  ts_ms INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_login_records_account"
      "  ON login_records(account, ts_ms);"
      // T3.3 设备台账：首登建档、责任人登记、启停（停用拒绝登录）
      "CREATE TABLE IF NOT EXISTS devices ("
      "  fingerprint TEXT PRIMARY KEY,"
      "  kind TEXT NOT NULL,"
      "  name TEXT NOT NULL,"
      "  owner_account TEXT NOT NULL DEFAULT '',"
      "  enabled INTEGER NOT NULL DEFAULT 1,"
      "  first_seen_ms INTEGER NOT NULL,"
      "  last_seen_ms INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS offline_messages ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  msg_id TEXT NOT NULL,"
      "  to_account TEXT NOT NULL,"
      "  envelope BLOB NOT NULL,"
      "  queued_ms INTEGER NOT NULL,"
      "  UNIQUE(msg_id, to_account));" // 群扇出一人多列（T4.1），按接收方 ACK
      "CREATE INDEX IF NOT EXISTS idx_offline_to"
      "  ON offline_messages(to_account, id);"
      "CREATE TABLE IF NOT EXISTS messages ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  msg_id TEXT NOT NULL UNIQUE,"
      "  from_account TEXT NOT NULL,"
      "  to_account TEXT NOT NULL,"
      "  type INTEGER NOT NULL,"
      "  text TEXT NOT NULL DEFAULT '',"
      "  ts_ms INTEGER NOT NULL,"
      "  recall INTEGER NOT NULL DEFAULT 0);"
      "CREATE INDEX IF NOT EXISTS idx_messages_to"
      "  ON messages(to_account, id);"
      "CREATE TABLE IF NOT EXISTS recall_events ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  msg_id TEXT NOT NULL,"
      "  by_account TEXT NOT NULL,"
      "  ts_ms INTEGER NOT NULL);"
      // 平台-4 归档事件溯源：append-only（无 UPDATE/DELETE 路径）；
      // 消息状态变迁全留痕，当前态可由本表重放重建
      "CREATE TABLE IF NOT EXISTS message_events ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  msg_id TEXT NOT NULL,"
      "  event TEXT NOT NULL,"
      "  by_account TEXT NOT NULL DEFAULT '',"
      "  payload TEXT NOT NULL DEFAULT '',"
      "  ts_ms INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_message_events_msg"
      "  ON message_events(msg_id, id);"
      // 平台-5 留存策略与清除台账（删除必须双人＋留痕，无痕删除被禁止）
      "CREATE TABLE IF NOT EXISTS retention_policies ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  department_path TEXT NOT NULL DEFAULT '' UNIQUE,"
      "  retention_days INTEGER NOT NULL,"
      "  updated_by TEXT NOT NULL DEFAULT '',"
      "  updated_ms INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS retention_purges ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  purged_by TEXT NOT NULL,"
      "  approved_by TEXT NOT NULL,"
      "  reason TEXT NOT NULL DEFAULT '',"
      "  policy_days INTEGER NOT NULL DEFAULT 0,"
      "  msg_count INTEGER NOT NULL DEFAULT 0,"
      "  before_ms INTEGER NOT NULL,"
      "  purged_ms INTEGER NOT NULL);"
      // 平台-11 远程协助：部门放行开关（默认禁）＋会话生命周期＋审计
      //（consent/audit 恒开不设开关，见 store.hpp 段注）
      "CREATE TABLE IF NOT EXISTS assist_policies ("
      "  department_path TEXT NOT NULL DEFAULT '' UNIQUE,"
      "  allow INTEGER NOT NULL DEFAULT 0,"
      "  updated_by TEXT NOT NULL DEFAULT '',"
      "  updated_ms INTEGER NOT NULL DEFAULT 0);"
      "CREATE TABLE IF NOT EXISTS assist_sessions ("
      "  id TEXT PRIMARY KEY,"
      "  requester TEXT NOT NULL,"
      "  target TEXT NOT NULL,"
      "  requested_mask INTEGER NOT NULL,"
      "  granted_mask INTEGER NOT NULL DEFAULT 0,"
      "  status TEXT NOT NULL,"
      "  requested_ms INTEGER NOT NULL,"
      "  approved_ms INTEGER NOT NULL DEFAULT 0,"
      "  started_ms INTEGER NOT NULL DEFAULT 0,"
      "  ended_ms INTEGER NOT NULL DEFAULT 0,"
      "  end_actor TEXT NOT NULL DEFAULT '',"
      "  end_reason TEXT NOT NULL DEFAULT '');"
      "CREATE INDEX IF NOT EXISTS idx_assist_sessions_member"
      "  ON assist_sessions(requester, target, requested_ms);"
      "CREATE TABLE IF NOT EXISTS assist_audits ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  session_id TEXT NOT NULL DEFAULT '',"
      "  actor TEXT NOT NULL,"
      "  action TEXT NOT NULL,"
      "  detail TEXT NOT NULL DEFAULT '',"
      "  ts_ms INTEGER NOT NULL);"
      // 平台-12 群能力开关（权限模型「群能力管理员配全」；未配置=允许）
      "CREATE TABLE IF NOT EXISTS group_capabilities ("
      "  gid INTEGER NOT NULL,"
      "  capability TEXT NOT NULL,"
      "  enabled INTEGER NOT NULL,"
      "  updated_by TEXT NOT NULL DEFAULT '',"
      "  updated_ms INTEGER NOT NULL,"
      "  PRIMARY KEY(gid, capability));"
      // R27-1 个人任务清单：分配=建到别人清单（owner≠creator），完成/
      // 提醒归 owner，撤回（删）owner 或 creator 皆可（判权在路由层）
      "CREATE TABLE IF NOT EXISTS tasks ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  owner TEXT NOT NULL,"
      "  creator TEXT NOT NULL,"
      "  title TEXT NOT NULL,"
      "  note TEXT NOT NULL DEFAULT '',"
      "  due_ms INTEGER NOT NULL DEFAULT 0,"
      "  reminded_ms INTEGER NOT NULL DEFAULT 0,"
      "  done INTEGER NOT NULL DEFAULT 0,"
      "  done_ms INTEGER NOT NULL DEFAULT 0,"
      "  created_ms INTEGER NOT NULL,"
      "  provider TEXT NOT NULL DEFAULT '',"
      "  ext_key TEXT NOT NULL DEFAULT '');"
      "CREATE INDEX IF NOT EXISTS idx_tasks_owner ON tasks(owner);"
      // 二期·审批（请假起步）：四态 pending|approved|rejected|withdrawn
      //（决定不删改全程留痕；判权在路由层 az——直属上级或无上级 admin 兜底）
      "CREATE TABLE IF NOT EXISTS approvals ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  applicant TEXT NOT NULL,"
      "  type TEXT NOT NULL,"
      "  leave_from TEXT NOT NULL DEFAULT '',"
      "  leave_to TEXT NOT NULL DEFAULT '',"
      "  reason TEXT NOT NULL DEFAULT '',"
      "  status TEXT NOT NULL DEFAULT 'pending',"
      "  decider TEXT NOT NULL DEFAULT '',"
      "  decision_note TEXT NOT NULL DEFAULT '',"
      "  created_ms INTEGER NOT NULL,"
      "  decided_ms INTEGER NOT NULL DEFAULT 0);"
      // 二期·日报周报：个人日报台账（当日重复=upsert 更新不留修订史；
      // 周报=按周聚合视图不单设表；直属上级可看下属，判权在路由层 az）
      "CREATE TABLE IF NOT EXISTS reports ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  author TEXT NOT NULL,"
      "  report_date TEXT NOT NULL,"
      "  content TEXT NOT NULL DEFAULT '',"
      "  created_ms INTEGER NOT NULL,"
      "  updated_ms INTEGER NOT NULL,"
      "  UNIQUE(author, report_date));"
      // 二期·办公室位置图：抽象平面工位点（归一化坐标；一人一工位=
      // 部分唯一索引；编辑权 org-admin 判权在路由层 az）
      "CREATE TABLE IF NOT EXISTS office_seats ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  floor TEXT NOT NULL,"
      "  label TEXT NOT NULL DEFAULT '',"
      "  x REAL NOT NULL DEFAULT 0,"
      "  y REAL NOT NULL DEFAULT 0,"
      "  account TEXT NOT NULL DEFAULT '');"
      "CREATE UNIQUE INDEX IF NOT EXISTS idx_office_seats_floor_label"
      "  ON office_seats(floor, label);"
      "CREATE UNIQUE INDEX IF NOT EXISTS idx_office_seats_account"
      "  ON office_seats(account) WHERE account != '';"
      // T2.6 组织架构：部门树（parent_id 成树）＋成员资料
      //（直属上级为独立单列——每人至多一名，结构性约束）
      "CREATE TABLE IF NOT EXISTS departments ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  name TEXT NOT NULL,"
      "  parent_id INTEGER,"
      "  created_ms INTEGER NOT NULL);"
      "CREATE UNIQUE INDEX IF NOT EXISTS idx_departments_parent_name"
      "  ON departments(parent_id, name);"
      "CREATE TABLE IF NOT EXISTS member_profiles ("
      "  account TEXT PRIMARY KEY,"
      "  department_id INTEGER,"
      "  title TEXT NOT NULL DEFAULT '',"
      "  manager TEXT NOT NULL DEFAULT '',"
      "  updated_ms INTEGER NOT NULL);"
      // T3.2 查阅日志：检索／导出动作逐次落一条（只附加，不删改）
      "CREATE TABLE IF NOT EXISTS audit_reads ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  op_account TEXT NOT NULL,"
      "  action TEXT NOT NULL,"
      "  filters TEXT NOT NULL DEFAULT '',"
      "  result_count INTEGER NOT NULL,"
      "  ts_ms INTEGER NOT NULL);"
      // T3.4 策略开关：按部门配置（department_path 空=全局兜底行）
      "CREATE TABLE IF NOT EXISTS policies ("
      "  department_path TEXT PRIMARY KEY,"
      "  allow_anonymous INTEGER NOT NULL,"
      "  allow_cross_state INTEGER NOT NULL,"
      "  new_device_approval INTEGER NOT NULL,"
      "  allow_cross_dept_file INTEGER NOT NULL DEFAULT 0,"
      "  allow_forward_file INTEGER NOT NULL DEFAULT 1);"
      // T4.1 群聊：群表＋成员表（群主退群=解散，成员记录清除、群号与归档保留）
      "CREATE TABLE IF NOT EXISTS groups ("
      "  group_id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  name TEXT NOT NULL,"
      "  owner TEXT NOT NULL,"
      "  announcement TEXT NOT NULL DEFAULT '',"
      "  created_ms INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS group_members ("
      "  group_id INTEGER NOT NULL,"
      "  account TEXT NOT NULL,"
      "  joined_ms INTEGER NOT NULL,"
      "  PRIMARY KEY(group_id, account));"
      // T4.3 已读回执：(msg_id, 已读方) 复合主键天然幂等
      "CREATE TABLE IF NOT EXISTS message_reads ("
      "  msg_id TEXT NOT NULL,"
      "  reader TEXT NOT NULL,"
      "  read_ms INTEGER NOT NULL,"
      "  PRIMARY KEY(msg_id, reader));"
      // T4.2 跨态会话日志：时间/双方/时长，不含内容（ended_ms=0 进行中）
      "CREATE TABLE IF NOT EXISTS cross_state_logs ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  account TEXT NOT NULL,"
      "  peer_device TEXT NOT NULL,"
      "  peer_name TEXT NOT NULL,"
      "  started_ms INTEGER NOT NULL,"
      "  ended_ms INTEGER NOT NULL DEFAULT 0,"
      "  duration_ms INTEGER NOT NULL DEFAULT 0);"
      // T4.5 常用联系人：(账号, 对端) 联合主键 UPSERT
      "CREATE TABLE IF NOT EXISTS favorites ("
      "  account TEXT NOT NULL,"
      "  peer TEXT NOT NULL,"
      "  starred INTEGER NOT NULL DEFAULT 0,"
      "  last_ms INTEGER NOT NULL DEFAULT 0,"
      "  PRIMARY KEY(account, peer));"
      // T4.6 通讯录可见性：按成员／部门的隐藏、限看与敏感字段（UPSERT 单行）
      "CREATE TABLE IF NOT EXISTS org_visibility ("
      "  scope TEXT NOT NULL," // member（key=账号）| dept（key=部门全路径）
      "  target_key TEXT NOT NULL,"
      "  hidden INTEGER NOT NULL DEFAULT 0,"
      "  restrict_scope INTEGER NOT NULL DEFAULT 0,"
      "  hide_fields TEXT NOT NULL DEFAULT '',"
      "  PRIMARY KEY(scope, target_key));"
      // 白名单例外：viewer 可见 target（成员账号或部门全路径，整树豁免）
      "CREATE TABLE IF NOT EXISTS org_visibility_allow ("
      "  viewer TEXT NOT NULL,"
      "  target TEXT NOT NULL,"
      "  PRIMARY KEY(viewer, target));"
      // 通知子系统（T4.10）：webhook 台账——按群／个人独立；token 只存
      // sha256 摘要（明文仅 create 时输出一次）；revoked=1 吊销即拒收。
      "CREATE TABLE IF NOT EXISTS webhooks ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  token_hash TEXT NOT NULL UNIQUE,"
      "  target TEXT NOT NULL,"
      "  name TEXT NOT NULL DEFAULT '',"
      "  created_ms INTEGER NOT NULL,"
      "  revoked INTEGER NOT NULL DEFAULT 0);"
      // 机器人（bot）：name 不含 "bot:" 前缀（伪账号="bot:"+name，直插
      // group_members/offline_messages——两表无外键，零迁移）；token 只存
      // sha256 摘要（明文仅 add 时输出一次）；disabled=1 即拒收发。
      "CREATE TABLE IF NOT EXISTS bots ("
      "  name TEXT PRIMARY KEY,"
      "  token_hash TEXT NOT NULL UNIQUE,"
      "  created_by TEXT NOT NULL,"
      "  created_ms INTEGER NOT NULL,"
      "  disabled INTEGER NOT NULL DEFAULT 0);"
      // 模型网关（平台三期）：上游端点登记——注册序即路由优先序；
      // is_local=归档数据红线专用位（仅本地模型规则写死在网关代码）；
      // api_key 库存原文（须可逆代发上游）。
      "CREATE TABLE IF NOT EXISTS model_endpoints ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  name TEXT NOT NULL UNIQUE,"
      "  base_url TEXT NOT NULL,"
      "  api_key TEXT NOT NULL DEFAULT '',"
      "  model TEXT NOT NULL DEFAULT '',"
      "  is_local INTEGER NOT NULL DEFAULT 0,"
      "  enabled INTEGER NOT NULL DEFAULT 1,"
      "  created_by TEXT NOT NULL,"
      "  created_ms INTEGER NOT NULL);"
      // 调用审计：一次上游尝试一行（降级即多行），只记元数据不记正文
      "CREATE TABLE IF NOT EXISTS model_calls ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  caller TEXT NOT NULL,"
      "  endpoint TEXT NOT NULL DEFAULT '',"
      "  model TEXT NOT NULL DEFAULT '',"
      "  archive_scope INTEGER NOT NULL DEFAULT 0,"
      "  prompt_chars INTEGER NOT NULL DEFAULT 0,"
      "  completion_chars INTEGER NOT NULL DEFAULT 0,"
      "  status INTEGER NOT NULL DEFAULT 0,"
      "  latency_ms INTEGER NOT NULL DEFAULT 0,"
      "  created_ms INTEGER NOT NULL);"
      // R23-1 文件存储元数据：文件表（秒传键 file_hash、归属、对象键、来源、状态）
      // R23-2 起秒传键含归属，R23-3 起再含类目 kind：
      // UNIQUE(file_hash, owner, belong_gid, belong_uid, kind)——同属主同
      // 哈希在「同一目标同一空间」内复用对象；换群/换人/换类目是不同文件
      // 行（对象前缀 groups/{gid}/ 与 users/{uid}/ 本就隔离，见设计铁律 3）。
      // pin=置顶（群主/管理员管群文件、本人管个人文件）。
      "CREATE TABLE IF NOT EXISTS files ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  owner TEXT NOT NULL,"
      "  belong_gid TEXT NOT NULL DEFAULT '',"
      "  belong_uid TEXT NOT NULL DEFAULT '',"
      "  file_name TEXT NOT NULL,"
      "  file_size INTEGER NOT NULL DEFAULT 0,"
      "  file_hash TEXT NOT NULL DEFAULT '',"
      "  object_key TEXT NOT NULL DEFAULT '',"
      "  source INTEGER NOT NULL DEFAULT 0,"  // 0=internal, 1=uplink
      "  upload_ts INTEGER NOT NULL DEFAULT 0,"
      "  status INTEGER NOT NULL DEFAULT 0,"  // 0=normal, 1=quarantine, 2=expired
      "  pin INTEGER NOT NULL DEFAULT 0,"
      "  kind INTEGER NOT NULL DEFAULT 0,"    // 0=personal, 1=inbox（R23-3）
      "  UNIQUE(file_hash, owner, belong_gid, belong_uid, kind));"
      "CREATE INDEX IF NOT EXISTS idx_files_owner ON files(owner);"
      "CREATE INDEX IF NOT EXISTS idx_files_belong_gid ON files(belong_gid);"
      "CREATE INDEX IF NOT EXISTS idx_files_belong_uid ON files(belong_uid);"
      // R23-3 文件助手备忘录：本人文本条目（纯元数据，不入对象存储）；
      // 自备忘录不留修订史（修订历史属 R24-2 群备忘录，另表另设计）。
      "CREATE TABLE IF NOT EXISTS memos ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  owner TEXT NOT NULL,"
      "  content TEXT NOT NULL,"
      "  created_ms INTEGER NOT NULL,"
      "  updated_ms INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_memos_owner ON memos(owner);"
      // 群配额：每个群的使用字节数与上限
      "CREATE TABLE IF NOT EXISTS group_quota ("
      "  gid TEXT PRIMARY KEY,"
      "  used_bytes INTEGER NOT NULL DEFAULT 0,"
      "  limit_bytes INTEGER NOT NULL DEFAULT 0);"
      // 用户配额：每个人的使用字节数与上限
      "CREATE TABLE IF NOT EXISTS user_quota ("
      "  uid TEXT PRIMARY KEY,"
      "  used_bytes INTEGER NOT NULL DEFAULT 0,"
      "  limit_bytes INTEGER NOT NULL DEFAULT 0);"
      // 外网上传流水（审计）：谁/何时/什么/落点
      "CREATE TABLE IF NOT EXISTS uplink_logs ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  uploader TEXT NOT NULL,"
      "  file_name TEXT NOT NULL,"
      "  file_size INTEGER NOT NULL DEFAULT 0,"
      "  file_hash TEXT NOT NULL DEFAULT '',"
      "  object_key TEXT NOT NULL DEFAULT '',"
      "  upload_ts INTEGER NOT NULL DEFAULT 0);"
      "CREATE INDEX IF NOT EXISTS idx_uplink_logs_uploader ON uplink_logs(uploader);"
      // 群公告编辑历史（R24-1）：只附加——每次成功设置（含清除）落一条
      // 谁/何时/改成了什么；现行公告全文仍在 groups.announcement。
      "CREATE TABLE IF NOT EXISTS group_announcement_log ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  group_id INTEGER NOT NULL,"
      "  editor TEXT NOT NULL,"
      "  content TEXT NOT NULL,"
      "  ts_ms INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_gal_gid ON group_announcement_log(group_id);"
      // 群备忘录（R24-2）：群维度共享知识条目（标题+正文，明文域——
      // 禁放密码的提示在客户端 UI 层）；修订史逐笔全文快照可回滚。
      "CREATE TABLE IF NOT EXISTS group_memos ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  group_id INTEGER NOT NULL,"
      "  title TEXT NOT NULL,"
      "  content TEXT NOT NULL,"
      "  author TEXT NOT NULL,"
      "  created_ms INTEGER NOT NULL,"
      "  updated_ms INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_group_memos_gid ON group_memos(group_id);"
      "CREATE TABLE IF NOT EXISTS group_memo_revisions ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  memo_id INTEGER NOT NULL,"
      "  title TEXT NOT NULL,"
      "  content TEXT NOT NULL,"
      "  editor TEXT NOT NULL,"
      "  ts_ms INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_gmr_memo ON group_memo_revisions(memo_id);"
      // —— R24-3 群密码箱：每群一箱（盐/迭代数/包裹块，服务端不碰明文）——
      "CREATE TABLE IF NOT EXISTS group_vaults ("
      "  group_id INTEGER PRIMARY KEY,"
      "  kdf_salt TEXT NOT NULL,"
      "  kdf_iters INTEGER NOT NULL,"
      "  wrapped_dek TEXT NOT NULL,"
      "  created_ms INTEGER NOT NULL,"
      "  updated_ms INTEGER NOT NULL);"
      // 条目：name/account_name 明文（掩码面），secret_* 密文 b64
      "CREATE TABLE IF NOT EXISTS group_vault_entries ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  group_id INTEGER NOT NULL,"
      "  name TEXT NOT NULL,"
      "  account_name TEXT NOT NULL,"
      "  secret_ct TEXT NOT NULL,"
      "  secret_nonce TEXT NOT NULL,"
      "  created_by TEXT NOT NULL,"
      "  created_ms INTEGER NOT NULL,"
      "  updated_ms INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_gve_gid"
      " ON group_vault_entries(group_id);"
      // 授权名单：无行=全成员可解锁（共享本意），群主收窄；owner/admin 恒可
      "CREATE TABLE IF NOT EXISTS group_vault_acl ("
      "  group_id INTEGER NOT NULL,"
      "  account TEXT NOT NULL,"
      "  added_ms INTEGER NOT NULL,"
      "  PRIMARY KEY (group_id, account));"
      // 查看/复制留痕（谁/何时/哪条/何动作）
      "CREATE TABLE IF NOT EXISTS group_vault_audit ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  group_id INTEGER NOT NULL,"
      "  entry_id INTEGER NOT NULL,"
      "  actor TEXT NOT NULL,"
      "  action TEXT NOT NULL,"
      "  ts_ms INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_gva_gid"
      " ON group_vault_audit(group_id);"
      // —— R25-1 群工具：白名单动作配置（gid+tool 主键 upsert；服务端不
      // 解释动作语义，路由层按白名单放行——无自由动作）——
      "CREATE TABLE IF NOT EXISTS group_tools ("
      "  group_id INTEGER NOT NULL,"
      "  tool TEXT NOT NULL,"
      "  actions_json TEXT NOT NULL,"
      "  updated_by TEXT NOT NULL,"
      "  updated_ms INTEGER NOT NULL,"
      "  PRIMARY KEY (group_id, tool));"
      // 动作留痕：谁/何时/哪个工具/什么动作/参数/结果（与审计面同源）
      "CREATE TABLE IF NOT EXISTS group_tool_audit ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  group_id INTEGER NOT NULL,"
      "  tool TEXT NOT NULL,"
      "  action TEXT NOT NULL,"
      "  actor TEXT NOT NULL,"
      "  params_json TEXT NOT NULL,"
      "  result_json TEXT NOT NULL,"
      "  ts_ms INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_gta_gid"
      " ON group_tool_audit(group_id);"
      // —— R25-2 CI/CD 工具：流水线定义（gid+name 主键 upsert）＋触发
      // 留痕（谁触发可回溯=设计点名；stub 执行器即时出终态）——
      "CREATE TABLE IF NOT EXISTS group_ci_pipelines ("
      "  group_id INTEGER NOT NULL,"
      "  name TEXT NOT NULL,"
      "  description TEXT NOT NULL DEFAULT '',"
      "  updated_by TEXT NOT NULL,"
      "  updated_ms INTEGER NOT NULL,"
      "  PRIMARY KEY (group_id, name));"
      "CREATE TABLE IF NOT EXISTS group_ci_runs ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  group_id INTEGER NOT NULL,"
      "  pipeline TEXT NOT NULL,"
      "  actor TEXT NOT NULL,"
      "  status TEXT NOT NULL,"
      "  params_json TEXT NOT NULL DEFAULT '{}',"
      "  result_json TEXT NOT NULL DEFAULT '{}',"
      "  ts_ms INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_gcr_gid"
      " ON group_ci_runs(group_id, pipeline, id);"
      // —— R25-3 打包工具：产物台账（同 gid+name+version upsert 覆盖）——
      "CREATE TABLE IF NOT EXISTS group_pack_artifacts ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  group_id INTEGER NOT NULL,"
      "  name TEXT NOT NULL,"
      "  version TEXT NOT NULL,"
      "  note TEXT NOT NULL DEFAULT '',"
      "  created_by TEXT NOT NULL,"
      "  created_ms INTEGER NOT NULL,"
      "  UNIQUE (group_id, name, version));"
      "CREATE INDEX IF NOT EXISTS idx_gpa_gid"
      " ON group_pack_artifacts(group_id);"
      // —— R25-4 凭据面：工具外部凭据（密文 hex=cred::gcm_seal；客户端
      // 永不取回，代理调用时内存内解密；同 gid+tool 覆盖）——
      "CREATE TABLE IF NOT EXISTS group_tool_credentials ("
      "  group_id INTEGER NOT NULL,"
      "  tool TEXT NOT NULL,"
      "  sealed_hex TEXT NOT NULL,"
      "  updated_by TEXT NOT NULL,"
      "  updated_ms INTEGER NOT NULL,"
      "  PRIMARY KEY (group_id, tool));"
      // —— R26-1 服务器 agent 面：登记行（agent 令牌只存 SHA-256 摘要，
      // 明文只在登记回包出现一次）＋最近一拍指标（红绿灯=last_seen 新鲜
      // 度，路由层判）；同 gid+name 复用一行（UNIQUE）——
      "CREATE TABLE IF NOT EXISTS group_servers ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  group_id INTEGER NOT NULL,"
      "  name TEXT NOT NULL,"
      "  host TEXT NOT NULL,"
      "  token_hash TEXT NOT NULL UNIQUE,"
      "  enrolled_by TEXT NOT NULL,"
      "  created_ms INTEGER NOT NULL,"
      "  last_seen_ms INTEGER NOT NULL DEFAULT 0,"
      "  cpu_percent REAL NOT NULL DEFAULT -1,"
      "  mem_used_mb REAL NOT NULL DEFAULT 0,"
      "  mem_total_mb REAL NOT NULL DEFAULT 0,"
      "  disk_used_mb REAL NOT NULL DEFAULT 0,"
      "  disk_total_mb REAL NOT NULL DEFAULT 0,"
      "  load1 REAL NOT NULL DEFAULT 0,"
      "  UNIQUE (group_id, name));"
      "CREATE INDEX IF NOT EXISTS idx_gs_gid"
      " ON group_servers(group_id);"
      // —— R26-3 远程会话：接入留痕（谁/何时/连哪台/协议/短票是否兑现/
      // 何时结束）；短票只存 SHA-256 摘要，明文只在签发回包出现一次——
      "CREATE TABLE IF NOT EXISTS group_server_sessions ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  group_id INTEGER NOT NULL,"
      "  server_id INTEGER NOT NULL,"
      "  actor TEXT NOT NULL,"
      "  protocol TEXT NOT NULL DEFAULT 'ssh',"
      "  ticket_hash TEXT NOT NULL UNIQUE,"
      "  opened_ms INTEGER NOT NULL,"
      "  redeemed_ms INTEGER NOT NULL DEFAULT 0,"
      "  closed_ms INTEGER NOT NULL DEFAULT 0);"
      "CREATE INDEX IF NOT EXISTS idx_gss_gid"
      " ON group_server_sessions(group_id, id);"
      // —— R26-4 服务器凭据：密文落库（cred::gcm_seal hex）；行内永不见
      // 明文，掩码元数据经 server_list JOIN 出——
      "CREATE TABLE IF NOT EXISTS group_server_credentials ("
      "  server_id INTEGER PRIMARY KEY,"
      "  sealed_hex TEXT NOT NULL,"
      "  updated_by TEXT NOT NULL,"
      "  updated_ms INTEGER NOT NULL);"
      // —— 二期群工具三件（原生互动，不走 R25 外部工具代理）：投票
      // （记名单选，改票=覆盖；匿名=展示不回 voter、多选=choice 存位集）
      // ＋接龙（一人一条 upsert）＋群任务（认领制，done 终态留痕不删行）——
      "CREATE TABLE IF NOT EXISTS group_polls ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  group_id INTEGER NOT NULL,"
      "  topic TEXT NOT NULL,"
      "  options_json TEXT NOT NULL,"
      "  deadline_ms INTEGER NOT NULL DEFAULT 0,"
      "  closed INTEGER NOT NULL DEFAULT 0,"
      "  anonymous INTEGER NOT NULL DEFAULT 0,"
      "  multi INTEGER NOT NULL DEFAULT 0,"
      "  created_by TEXT NOT NULL,"
      "  created_ms INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_gpo_gid"
      " ON group_polls(group_id, id);"
      "CREATE TABLE IF NOT EXISTS group_poll_votes ("
      "  poll_id INTEGER NOT NULL,"
      "  account TEXT NOT NULL,"
      "  choice INTEGER NOT NULL,"
      "  ts_ms INTEGER NOT NULL,"
      "  PRIMARY KEY (poll_id, account));"
      "CREATE TABLE IF NOT EXISTS group_chains ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  group_id INTEGER NOT NULL,"
      "  title TEXT NOT NULL,"
      "  format_hint TEXT NOT NULL DEFAULT '',"
      "  closed INTEGER NOT NULL DEFAULT 0,"
      "  created_by TEXT NOT NULL,"
      "  created_ms INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_gch_gid"
      " ON group_chains(group_id, id);"
      "CREATE TABLE IF NOT EXISTS group_chain_entries ("
      "  chain_id INTEGER NOT NULL,"
      "  account TEXT NOT NULL,"
      "  content TEXT NOT NULL,"
      "  ts_ms INTEGER NOT NULL,"
      "  PRIMARY KEY (chain_id, account));"
      "CREATE TABLE IF NOT EXISTS group_tasks ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  group_id INTEGER NOT NULL,"
      "  title TEXT NOT NULL,"
      "  assignee TEXT NOT NULL DEFAULT '',"
      "  due_ms INTEGER NOT NULL DEFAULT 0,"
      "  claimed_ms INTEGER NOT NULL DEFAULT 0,"
      "  status TEXT NOT NULL DEFAULT 'todo',"
      "  created_by TEXT NOT NULL,"
      "  created_ms INTEGER NOT NULL,"
      "  done_by TEXT NOT NULL DEFAULT '',"
      "  done_ms INTEGER NOT NULL DEFAULT 0);"
      "CREATE INDEX IF NOT EXISTS idx_gtk_gid"
      " ON group_tasks(group_id, id);"
      // —— 平台-2 Identity 模型补全：Credential 独立（口令迁出 accounts
      // 行；type 枚举预留 token/certificate/sso/device）＋IdentityBinding
      // （外部身份↔本地账号，issuer+subject 唯一）＋Session 持久面
      // （签发落行、登出落理由，热路径裁决仍在内存）——
      "CREATE TABLE IF NOT EXISTS credentials ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  account TEXT NOT NULL,"
      "  type TEXT NOT NULL DEFAULT 'password',"
      "  salt TEXT NOT NULL DEFAULT '',"
      "  digest TEXT NOT NULL,"
      "  created_ms INTEGER NOT NULL,"
      "  disabled INTEGER NOT NULL DEFAULT 0,"
      "  UNIQUE(account, type, digest));"
      "CREATE INDEX IF NOT EXISTS idx_credentials_account"
      " ON credentials(account, type);"
      "CREATE TABLE IF NOT EXISTS identity_bindings ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  account TEXT NOT NULL,"
      "  issuer TEXT NOT NULL,"
      "  subject TEXT NOT NULL,"
      "  created_ms INTEGER NOT NULL,"
      "  UNIQUE(issuer, subject));"
      "CREATE TABLE IF NOT EXISTS sessions ("
      "  token_hash TEXT PRIMARY KEY,"
      "  account TEXT NOT NULL,"
      "  scope TEXT NOT NULL DEFAULT 'internal',"
      "  device_id TEXT NOT NULL DEFAULT '',"
      "  created_ms INTEGER NOT NULL,"
      "  expires_ms INTEGER NOT NULL,"
      "  logged_out_ms INTEGER NOT NULL DEFAULT 0,"
      "  logout_reason TEXT NOT NULL DEFAULT '');"
      "CREATE INDEX IF NOT EXISTS idx_sessions_account"
      " ON sessions(account, created_ms);"
      // —— 平台-3 三关联（Membership/RoleAssignment/ReportingLine）：
      //     一人多部门、多角色（临时代理=带时间窗的授权）、直属上级
      //     独立成表（沿用每人至多一名）——
      "CREATE TABLE IF NOT EXISTS org_memberships ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  account TEXT NOT NULL,"
      "  department_id INTEGER NOT NULL,"
      "  created_ms INTEGER NOT NULL,"
      "  UNIQUE(account, department_id));"
      "CREATE INDEX IF NOT EXISTS idx_org_memberships_dept"
      " ON org_memberships(department_id);"
      "CREATE TABLE IF NOT EXISTS org_role_assignments ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  account TEXT NOT NULL,"
      "  role TEXT NOT NULL,"
      "  scope TEXT NOT NULL DEFAULT '',"
      "  valid_from_ms INTEGER NOT NULL DEFAULT 0,"
      "  valid_until_ms INTEGER NOT NULL DEFAULT 0,"
      "  granted_by TEXT NOT NULL DEFAULT '',"
      "  created_ms INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_org_roles_account"
      " ON org_role_assignments(account, role);"
      "CREATE TABLE IF NOT EXISTS org_reporting_lines ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  account TEXT NOT NULL UNIQUE,"
      "  manager_account TEXT NOT NULL,"
      "  created_ms INTEGER NOT NULL);"
      // 老库迁移（幂等）：member_profiles 单列部门/上级 → 三关联
      "INSERT INTO org_memberships(account, department_id, created_ms)"
      " SELECT account, department_id, updated_ms FROM member_profiles"
      " WHERE department_id IS NOT NULL AND NOT EXISTS"
      " (SELECT 1 FROM org_memberships m"
      "  WHERE m.account = member_profiles.account"
      "  AND m.department_id = member_profiles.department_id);"
      "INSERT INTO org_reporting_lines(account, manager_account, created_ms)"
      " SELECT account, manager, updated_ms FROM member_profiles"
      " WHERE manager != '' AND NOT EXISTS"
      " (SELECT 1 FROM org_reporting_lines l"
      "  WHERE l.account = member_profiles.account);"
      // 老库迁移：accounts 内联口令 → credentials（幂等：已有行不重迁）
      "INSERT INTO credentials(account, type, salt, digest, created_ms)"
      " SELECT account, 'password', salt, digest, created_ms FROM accounts"
      " WHERE NOT EXISTS (SELECT 1 FROM credentials c"
      " WHERE c.account = accounts.account AND c.type = 'password');"; // 本段为 schema 字符串最后一段
  char* err = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    sqlite3_free(err);
    return false;
  }
  // 旧库迁移：补 role 列（已存在则忽略失败）
  sqlite3_exec(db_, "ALTER TABLE accounts ADD COLUMN"
                    " role TEXT NOT NULL DEFAULT 'member'",
              nullptr, nullptr, nullptr);
  // 旧库迁移（R24-2）：群备忘录开放编辑开关（默认关=管理员维护）
  sqlite3_exec(db_, "ALTER TABLE groups ADD COLUMN"
                    " open_memo_edit INTEGER NOT NULL DEFAULT 0",
              nullptr, nullptr, nullptr);
  // 旧库迁移（R27-2）：任务表补外部任务引用列（provider/ext_key 成对，
  // 空=本地任务；R27-1 建的库无此两列）
  sqlite3_exec(db_, "ALTER TABLE tasks ADD COLUMN"
                    " provider TEXT NOT NULL DEFAULT ''",
              nullptr, nullptr, nullptr);
  sqlite3_exec(db_, "ALTER TABLE tasks ADD COLUMN"
                    " ext_key TEXT NOT NULL DEFAULT ''",
              nullptr, nullptr, nullptr);
  // 旧库迁移（第七笔）：投票表补匿名/多选两列（默认 0=记名单选不变）
  sqlite3_exec(db_, "ALTER TABLE group_polls ADD COLUMN"
                    " anonymous INTEGER NOT NULL DEFAULT 0",
              nullptr, nullptr, nullptr);
  sqlite3_exec(db_, "ALTER TABLE group_polls ADD COLUMN"
                    " multi INTEGER NOT NULL DEFAULT 0",
              nullptr, nullptr, nullptr);
  // 旧库迁移（T4.1）：offline_messages 单列 UNIQUE(msg_id) →
  // 复合 UNIQUE(msg_id, to_account)。旧表不重建则群扇出 INSERT OR IGNORE
  // 按 msg_id 把第 2..N 个离线接收方静默丢弃。事务化重建，失败回滚保数据。
  {
    bool legacy = false;
    sqlite3_stmt* st = nullptr;
    const char* probe =
        "SELECT sql FROM sqlite_master WHERE type='table'"
        " AND name='offline_messages';";
    if (sqlite3_prepare_v2(db_, probe, -1, &st, nullptr) == SQLITE_OK) {
      if (sqlite3_step(st) == SQLITE_ROW) {
        const char* ddl =
            reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        legacy = ddl && std::string(ddl).find("UNIQUE(msg_id, to_account)") ==
                           std::string::npos;
      }
      sqlite3_finalize(st);
    }
    if (legacy) {
      const char* steps[] = {
          "BEGIN;",
          "ALTER TABLE offline_messages RENAME TO offline_messages_legacy;",
          "CREATE TABLE offline_messages ("
          "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "  msg_id TEXT NOT NULL,"
          "  to_account TEXT NOT NULL,"
          "  envelope BLOB NOT NULL,"
          "  queued_ms INTEGER NOT NULL,"
          "  UNIQUE(msg_id, to_account));",
          "INSERT INTO offline_messages(msg_id, to_account, envelope,"
          " queued_ms) SELECT msg_id, to_account, envelope, queued_ms"
          " FROM offline_messages_legacy;",
          "DROP TABLE offline_messages_legacy;",
          "CREATE INDEX IF NOT EXISTS idx_offline_to"
          " ON offline_messages(to_account, id);",
          "COMMIT;",
      };
      bool migrated = true;
      for (const char* s : steps) {
        if (sqlite3_exec(db_, s, nullptr, nullptr, nullptr) != SQLITE_OK) {
          migrated = false;
          break;
        }
      }
      if (!migrated) sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
  }
  // 旧库迁移（R23-2）：files 秒传键 UNIQUE(file_hash, owner) →
  // UNIQUE(file_hash, owner, belong_gid, belong_uid)＋补 pin 列。旧键把
  // 「同内容传第二个群」误判成秒传（行永不落新归属）。事务化重建，失败
  // 回滚保数据；pin 缺列的中间态库单独 ALTER。
  // 探针不含收尾括号：R23-3 起新键是 UNIQUE(..., belong_uid, kind)，
  // 若按「…belong_uid）」整串找会把新形表误判成 legacy 而回退重建。
  {
    bool legacy = false;
    sqlite3_stmt* st = nullptr;
    const char* probe =
        "SELECT sql FROM sqlite_master WHERE type='table' AND name='files';";
    if (sqlite3_prepare_v2(db_, probe, -1, &st, nullptr) == SQLITE_OK) {
      if (sqlite3_step(st) == SQLITE_ROW) {
        const char* ddl =
            reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        legacy = ddl && std::string(ddl).find(
                              "UNIQUE(file_hash, owner, belong_gid,"
                              " belong_uid") == std::string::npos;
      }
      sqlite3_finalize(st);
    }
    if (legacy) {
      const char* steps[] = {
          "BEGIN;",
          "ALTER TABLE files RENAME TO files_legacy;",
          "CREATE TABLE files ("
          "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "  owner TEXT NOT NULL,"
          "  belong_gid TEXT NOT NULL DEFAULT '',"
          "  belong_uid TEXT NOT NULL DEFAULT '',"
          "  file_name TEXT NOT NULL,"
          "  file_size INTEGER NOT NULL DEFAULT 0,"
          "  file_hash TEXT NOT NULL DEFAULT '',"
          "  object_key TEXT NOT NULL DEFAULT '',"
          "  source INTEGER NOT NULL DEFAULT 0,"
          "  upload_ts INTEGER NOT NULL DEFAULT 0,"
          "  status INTEGER NOT NULL DEFAULT 0,"
          "  pin INTEGER NOT NULL DEFAULT 0,"
          "  UNIQUE(file_hash, owner, belong_gid, belong_uid));",
          "INSERT INTO files(id, owner, belong_gid, belong_uid, file_name,"
          " file_size, file_hash, object_key, source, upload_ts, status, pin)"
          " SELECT id, owner, belong_gid, belong_uid, file_name, file_size,"
          " file_hash, object_key, source, upload_ts, status, 0"
          " FROM files_legacy;",
          "DROP TABLE files_legacy;",
          "CREATE INDEX IF NOT EXISTS idx_files_owner ON files(owner);",
          "CREATE INDEX IF NOT EXISTS idx_files_belong_gid"
          " ON files(belong_gid);",
          "CREATE INDEX IF NOT EXISTS idx_files_belong_uid"
          " ON files(belong_uid);",
          "COMMIT;",
      };
      bool migrated = true;
      for (const char* s : steps) {
        if (sqlite3_exec(db_, s, nullptr, nullptr, nullptr) != SQLITE_OK) {
          migrated = false;
          break;
        }
      }
      if (!migrated) sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
  }
  // 旧库迁移（R23-3）：files 秒传键补类目 kind 列，键扩为
  // UNIQUE(file_hash, owner, belong_gid, belong_uid, kind)——收件箱与
  // 个人空间互不秒传串用。存量行全部 kind=0（personal）。事务化重建，
  // 失败回滚保数据；探针含「, kind）」整串：R23-2 形态库才会进来。
  {
    bool legacy = false;
    sqlite3_stmt* st = nullptr;
    const char* probe =
        "SELECT sql FROM sqlite_master WHERE type='table' AND name='files';";
    if (sqlite3_prepare_v2(db_, probe, -1, &st, nullptr) == SQLITE_OK) {
      if (sqlite3_step(st) == SQLITE_ROW) {
        const char* ddl =
            reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        legacy = ddl && std::string(ddl).find(
                              "UNIQUE(file_hash, owner, belong_gid,"
                              " belong_uid, kind)") == std::string::npos;
      }
      sqlite3_finalize(st);
    }
    if (legacy) {
      const char* steps[] = {
          "BEGIN;",
          "ALTER TABLE files RENAME TO files_legacy;",
          "CREATE TABLE files ("
          "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "  owner TEXT NOT NULL,"
          "  belong_gid TEXT NOT NULL DEFAULT '',"
          "  belong_uid TEXT NOT NULL DEFAULT '',"
          "  file_name TEXT NOT NULL,"
          "  file_size INTEGER NOT NULL DEFAULT 0,"
          "  file_hash TEXT NOT NULL DEFAULT '',"
          "  object_key TEXT NOT NULL DEFAULT '',"
          "  source INTEGER NOT NULL DEFAULT 0,"
          "  upload_ts INTEGER NOT NULL DEFAULT 0,"
          "  status INTEGER NOT NULL DEFAULT 0,"
          "  pin INTEGER NOT NULL DEFAULT 0,"
          "  kind INTEGER NOT NULL DEFAULT 0,"
          "  UNIQUE(file_hash, owner, belong_gid, belong_uid, kind));",
          "INSERT INTO files(id, owner, belong_gid, belong_uid, file_name,"
          " file_size, file_hash, object_key, source, upload_ts, status,"
          " pin, kind)"
          " SELECT id, owner, belong_gid, belong_uid, file_name, file_size,"
          " file_hash, object_key, source, upload_ts, status, pin, 0"
          " FROM files_legacy;",
          "DROP TABLE files_legacy;",
          "CREATE INDEX IF NOT EXISTS idx_files_owner ON files(owner);",
          "CREATE INDEX IF NOT EXISTS idx_files_belong_gid"
          " ON files(belong_gid);",
          "CREATE INDEX IF NOT EXISTS idx_files_belong_uid"
          " ON files(belong_uid);",
          "CREATE INDEX IF NOT EXISTS idx_files_kind ON files(kind);",
          "COMMIT;",
      };
      bool migrated = true;
      for (const char* s : steps) {
        if (sqlite3_exec(db_, s, nullptr, nullptr, nullptr) != SQLITE_OK) {
          migrated = false;
          break;
        }
      }
      if (!migrated) sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
  }
  // kind 索引收尾：主 schema 串不能含它——R23-2 形态库 kind 列尚未存在，
  // 放主串会让 open 直接失败（迁移失败时这里也静默，表已按新形重建则成）。
  sqlite3_exec(db_, "CREATE INDEX IF NOT EXISTS idx_files_kind"
                    " ON files(kind)",
              nullptr, nullptr, nullptr);
  // 旧库迁移：群成员补组内角色列（权限模型：群主/管理员/成员；缺省=成员）
  sqlite3_exec(db_, "ALTER TABLE group_members ADD COLUMN"
                    " role TEXT NOT NULL DEFAULT 'member'",
              nullptr, nullptr, nullptr);
  // 旧库迁移：策略行补直连文件旁路两开关（平台-10；缺省=跨部门禁/转发允）
  sqlite3_exec(db_, "ALTER TABLE policies ADD COLUMN"
                    " allow_cross_dept_file INTEGER NOT NULL DEFAULT 0",
              nullptr, nullptr, nullptr);
  sqlite3_exec(db_, "ALTER TABLE policies ADD COLUMN"
                    " allow_forward_file INTEGER NOT NULL DEFAULT 1",
              nullptr, nullptr, nullptr);
  return true;
}

bool ServerStore::create_account(const std::string& account,
                                 const std::string& password,
                                 const std::string& display_name,
                                 const std::string& role) {
  const std::string salt = random_salt_hex();
  if (salt.empty()) return false;
  const char* sql =
      "INSERT INTO accounts(account, display_name, salt, digest, role,"
      " created_ms) VALUES(?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  // 摘要计算独立于语句，失败即放弃本次插入（盐只存在内存，无残留）
  std::string digest;
  try {
    digest = pbkdf2_sha256_hex(password, salt, kPbkdf2IterationsForStore);
  } catch (...) {
    sqlite3_finalize(st);
    return false;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, display_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, salt.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, digest.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, (role == "admin") ? "admin" : "member", -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 6, now_ms());
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok) return false; // 唯一键冲突（账号已存在）→ false
  // 平台-2：口令凭据独立落 credentials（判登唯一来源；accounts 行内
  // salt/digest 仅作老列兼容保留，不再参与判登）
  return credential_insert(account, "password", salt, digest, now_ms());
}

// 凭据行插入（create_account 与未来面共用；UNIQUE 冲突=false）
bool ServerStore::credential_insert(const std::string& account,
                                    const std::string& type,
                                    const std::string& salt_hex,
                                    const std::string& digest_hex,
                                    std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO credentials(account, type, salt, digest, created_ms)"
      " VALUES(?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, salt_hex.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, digest_hex.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

// —— 平台-2 Identity 模型补全 ——

std::optional<ServerStore::CredentialRow> ServerStore::find_credential(
    const std::string& account, const std::string& type) {
  const char* sql =
      "SELECT id, account, type, salt, digest, created_ms, disabled"
      " FROM credentials WHERE account = ? AND type = ? AND disabled = 0"
      " ORDER BY id DESC LIMIT 1;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, type.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<CredentialRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    CredentialRow r;
    r.id = sqlite3_column_int64(st, 0);
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* d = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.account = a ? a : "";
    r.type = t ? t : "";
    r.salt_hex = s ? s : "";
    r.digest_hex = d ? d : "";
    r.created_ms = sqlite3_column_int64(st, 5);
    r.disabled = sqlite3_column_int(st, 6) != 0;
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

std::int64_t ServerStore::identity_bind(const std::string& account,
                                        const std::string& issuer,
                                        const std::string& subject,
                                        std::int64_t ts_ms) {
  if (!find_account(account).has_value()) return 0;
  if (issuer.empty() || subject.empty()) return 0;
  const char* sql =
      "INSERT INTO identity_bindings(account, issuer, subject, created_ms)"
      " VALUES(?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, issuer.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, subject.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok ? sqlite3_last_insert_rowid(db_) : 0; // 唯一冲突（已绑）→ 0
}

bool ServerStore::identity_unbind(std::int64_t id) {
  const char* sql = "DELETE FROM identity_bindings WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::optional<ServerStore::IdentityBinding> ServerStore::identity_find(
    const std::string& issuer, const std::string& subject) {
  const char* sql =
      "SELECT id, account, issuer, subject, created_ms"
      " FROM identity_bindings WHERE issuer = ? AND subject = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, issuer.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, subject.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<IdentityBinding> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    IdentityBinding r;
    r.id = sqlite3_column_int64(st, 0);
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* i = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.account = a ? a : "";
    r.issuer = i ? i : "";
    r.subject = s ? s : "";
    r.created_ms = sqlite3_column_int64(st, 4);
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::IdentityBinding> ServerStore::identity_list(
    const std::string& account) {
  std::vector<IdentityBinding> out;
  const char* sql =
      "SELECT id, account, issuer, subject, created_ms"
      " FROM identity_bindings WHERE (? = '' OR account = ?)"
      " ORDER BY id ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    IdentityBinding r;
    r.id = sqlite3_column_int64(st, 0);
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* i = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.account = a ? a : "";
    r.issuer = i ? i : "";
    r.subject = s ? s : "";
    r.created_ms = sqlite3_column_int64(st, 4);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::session_insert(const SessionRecord& rec) {
  const char* sql =
      "INSERT OR REPLACE INTO sessions(token_hash, account, scope, device_id,"
      " created_ms, expires_ms, logged_out_ms, logout_reason)"
      " VALUES(?,?,?,?,?,?,0,'');";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, rec.token_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, rec.account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, rec.scope.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, rec.device_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, rec.created_ms);
  sqlite3_bind_int64(st, 6, rec.expires_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::session_close(const std::string& token_hash,
                                const std::string& reason,
                                std::int64_t ts_ms) {
  const char* sql =
      "UPDATE sessions SET logged_out_ms = ?, logout_reason = ?"
      " WHERE token_hash = ? AND logged_out_ms = 0;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, ts_ms);
  sqlite3_bind_text(st, 2, reason.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, token_hash.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::SessionRecord> ServerStore::session_list(
    const std::string& account, int limit) {
  std::vector<SessionRecord> out;
  const char* sql =
      "SELECT token_hash, account, scope, device_id, created_ms, expires_ms,"
      " logged_out_ms, logout_reason FROM sessions"
      " WHERE (? = '' OR account = ?) ORDER BY created_ms DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, limit > 0 ? limit : 100);
  while (sqlite3_step(st) == SQLITE_ROW) {
    SessionRecord r;
    const char* h = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* d = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.token_hash = h ? h : "";
    r.account = a ? a : "";
    r.scope = s ? s : "";
    r.device_id = d ? d : "";
    r.created_ms = sqlite3_column_int64(st, 4);
    r.expires_ms = sqlite3_column_int64(st, 5);
    r.logged_out_ms = sqlite3_column_int64(st, 6);
    const char* lr =
        reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
    r.logout_reason = lr ? lr : "";
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

std::optional<AccountRow> ServerStore::find_account(const std::string& account) {
  const char* sql =
      "SELECT account, display_name, salt, digest, role FROM accounts"
      " WHERE account = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return std::nullopt;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<AccountRow> row;
  if (sqlite3_step(st) == SQLITE_ROW) {
    AccountRow r;
    r.account = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    r.display_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.salt_hex = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.digest_hex = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.role = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    row = r;
  }
  sqlite3_finalize(st);
  return row;
}

std::vector<std::pair<std::string, std::string>> ServerStore::account_list() {
  std::vector<std::pair<std::string, std::string>> out;
  const char* sql =
      "SELECT account, display_name || '（' || role || '）' FROM accounts"
      " ORDER BY account;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    out.emplace_back(
        reinterpret_cast<const char*>(sqlite3_column_text(st, 0)),
        reinterpret_cast<const char*>(sqlite3_column_text(st, 1)));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::add_login_record(const LoginRecord& rec) {
  const char* sql =
      "INSERT INTO login_records(account, fingerprint, kind, name, source_ip,"
      " version, result, ts_ms) VALUES(?, ?, ?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, rec.account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, rec.fingerprint.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, rec.kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, rec.name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, rec.source_ip.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, rec.version.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 7, rec.result.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 8, rec.ts_ms != 0 ? rec.ts_ms : now_ms());
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<LoginRecord> ServerStore::login_records(const std::string& account,
                                                    const std::string& fp_prefix,
                                                    int limit) {
  std::vector<LoginRecord> out;
  const char* sql =
      "SELECT id, account, fingerprint, kind, name, source_ip, version, result,"
      " ts_ms FROM login_records";
  std::string query = sql;
  std::string where;
  if (!account.empty()) where += "account = ?";
  if (!fp_prefix.empty()) {
    if (!where.empty()) where += " AND ";
    where += "fingerprint LIKE ? || '%'";
  }
  if (!where.empty()) query += " WHERE " + where;
  query += " ORDER BY ts_ms DESC, id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, query.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  int idx = 1;
  if (!account.empty()) {
    sqlite3_bind_text(st, idx++, account.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (!fp_prefix.empty()) {
    sqlite3_bind_text(st, idx++, fp_prefix.c_str(), -1, SQLITE_TRANSIENT);
  }
  sqlite3_bind_int(st, idx, limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    LoginRecord r;
    r.id = sqlite3_column_int64(st, 0);
    r.account = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.fingerprint = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.kind = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.source_ip = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    r.version = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.result = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
    r.ts_ms = sqlite3_column_int64(st, 8);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// —— T3.3 设备台账 ——

// 首登建档（INSERT OR IGNORE 保首见时间）、再登刷新 last_seen
bool ServerStore::upsert_device(const std::string& fingerprint,
                                const std::string& kind,
                                const std::string& name,
                                std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO devices(fingerprint, kind, name, first_seen_ms, last_seen_ms)"
      " VALUES(?, ?, ?, ?, ?)"
      " ON CONFLICT(fingerprint) DO UPDATE SET"
      " kind = excluded.kind, name = excluded.name,"
      " last_seen_ms = excluded.last_seen_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, ts_ms);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::optional<DeviceRow> ServerStore::find_device(
    const std::string& fingerprint) {
  const char* sql =
      "SELECT fingerprint, kind, name, owner_account, enabled,"
      " first_seen_ms, last_seen_ms FROM devices WHERE fingerprint = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return std::nullopt;
  sqlite3_bind_text(st, 1, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<DeviceRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    DeviceRow d;
    const auto text_of = [&st](int col) {
      const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
      return p ? std::string(p) : std::string{};
    };
    d.fingerprint = text_of(0);
    d.kind = text_of(1);
    d.name = text_of(2);
    d.owner_account = text_of(3);
    d.enabled = sqlite3_column_int(st, 4) != 0;
    d.first_seen_ms = sqlite3_column_int64(st, 5);
    d.last_seen_ms = sqlite3_column_int64(st, 6);
    out = std::move(d);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<DeviceRow> ServerStore::device_list() {
  std::vector<DeviceRow> out;
  const char* sql =
      "SELECT fingerprint, kind, name, owner_account, enabled,"
      " first_seen_ms, last_seen_ms FROM devices"
      " ORDER BY last_seen_ms DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    DeviceRow d;
    const auto text_of = [&st](int col) {
      const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
      return p ? std::string(p) : std::string{};
    };
    d.fingerprint = text_of(0);
    d.kind = text_of(1);
    d.name = text_of(2);
    d.owner_account = text_of(3);
    d.enabled = sqlite3_column_int(st, 4) != 0;
    d.first_seen_ms = sqlite3_column_int64(st, 5);
    d.last_seen_ms = sqlite3_column_int64(st, 6);
    out.push_back(std::move(d));
  }
  sqlite3_finalize(st);
  return out;
}

// 指纹前缀定位：≥8 位防误配；多义时 second=true（CLI 拒绝并提示补长）
std::pair<std::string, bool> ServerStore::device_by_prefix(
    const std::string& prefix) {
  if (prefix.size() < 8) return {"", false};
  std::vector<std::string> hits;
  for (const auto& d : device_list()) {
    if (d.fingerprint.compare(0, prefix.size(), prefix) == 0) {
      hits.push_back(d.fingerprint);
    }
  }
  if (hits.size() == 1) return {hits[0], false};
  return {"", hits.size() > 1};
}

// 责任人登记：账号须已存在（防拼错挂空名）
bool ServerStore::set_device_owner(const std::string& fingerprint,
                                   const std::string& owner_account) {
  if (!find_account(owner_account).has_value()) return false;
  const char* sql = "UPDATE devices SET owner_account = ? WHERE fingerprint = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, owner_account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::set_device_enabled(const std::string& fingerprint,
                                     bool enabled) {
  const char* sql = "UPDATE devices SET enabled = ? WHERE fingerprint = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(st, 1, enabled ? 1 : 0);
  sqlite3_bind_text(st, 2, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

// 解绑：清责任人并停用（该设备须重新启用并登记责任人才可用）
bool ServerStore::unbind_device(const std::string& fingerprint) {
  const char* sql =
      "UPDATE devices SET owner_account = '', enabled = 0"
      " WHERE fingerprint = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::queue_offline(const std::string& msg_id,
                                const std::string& to_account,
                                const std::string& envelope_blob) {
  const char* sql =
      "INSERT OR IGNORE INTO offline_messages(msg_id, to_account, envelope,"
      " queued_ms) VALUES(?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, to_account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(st, 3, envelope_blob.data(),
                    static_cast<int>(envelope_blob.size()), SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, now_ms());
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<std::string> ServerStore::pending_offline(const std::string& account) {
  std::vector<std::string> out;
  const char* sql =
      "SELECT envelope FROM offline_messages WHERE to_account = ?"
      " ORDER BY id ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    const auto* p = static_cast<const char*>(sqlite3_column_blob(st, 0));
    const int n = sqlite3_column_bytes(st, 0);
    out.emplace_back(p, static_cast<std::size_t>(n));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::ack_offline(const std::string& msg_id,
                              const std::string& account) {
  // 群扇出场景：同一条群消息对每名成员各有一行，仅清本接收方那份
  const char* sql =
      "DELETE FROM offline_messages WHERE msg_id = ? AND to_account = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::size_t ServerStore::offline_count(const std::string& account) {
  const char* sql =
      "SELECT COUNT(*) FROM offline_messages WHERE to_account = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  std::size_t n = 0;
  if (sqlite3_step(st) == SQLITE_ROW) {
    n = static_cast<std::size_t>(sqlite3_column_int64(st, 0));
  }
  sqlite3_finalize(st);
  return n;
}

// T2.3 消息归档：协作态消息全量落库
bool ServerStore::store_message(const std::string& msg_id, const std::string& from_account,
                                const std::string& to_account, int type,
                                const std::string& text, std::int64_t ts_ms) {
  // 平台-4：归档走 created 事件（payload 带全量字段，事件与投影同事务落）
  nlohmann::json p;
  p["from"] = from_account;
  p["to"] = to_account;
  p["type"] = type;
  p["text"] = text;
  p["ts_ms"] = ts_ms;
  return append_message_event(msg_id, "created", from_account, p.dump(),
                              ts_ms > 0 ? ts_ms : now_ms());
}

// —— 平台-4 归档事件溯源 ——

bool ServerStore::append_message_event(const std::string& msg_id,
                                       const std::string& event,
                                       const std::string& by_account,
                                       const std::string& payload,
                                       std::int64_t ts_ms) {
  if (msg_id.empty() || event.empty()) return false;
  char* tx = nullptr;
  if (sqlite3_exec(db_, "SAVEPOINT msg_evt;", nullptr, nullptr, &tx) !=
      SQLITE_OK) {
    sqlite3_free(tx);
    return false;
  }
  bool ok = true;
  if (event == "created") {
    // created 物化归档行：payload 带全量字段；重投去重（已有行不重放）
    nlohmann::json p = nlohmann::json::parse(payload, nullptr, false);
    if (p.is_discarded() || !p.contains("from") || !p.contains("to")) {
      sqlite3_exec(db_, "ROLLBACK TO msg_evt;", nullptr, nullptr, &tx);
      sqlite3_exec(db_, "RELEASE msg_evt;", nullptr, nullptr, &tx);
      sqlite3_free(tx);
      return false;
    }
    sqlite3_stmt* i = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "INSERT OR IGNORE INTO messages(msg_id, from_account, to_account,"
             " type, text, ts_ms) VALUES(?,?,?,?,?,?);",
             -1, &i, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(i, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(i, 2,
                        p.value("from", std::string{}).c_str(), -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(i, 3,
                        p.value("to", std::string{}).c_str(), -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_int(i, 4, p.value("type", 0));
      sqlite3_bind_text(i, 5,
                        p.value("text", std::string{}).c_str(), -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_int64(i, 6, p.value("ts_ms", std::int64_t{0}));
      ok = sqlite3_step(i) == SQLITE_DONE;
      if (ok) ok = sqlite3_changes(db_) > 0; // 已存在=重投，不记重复事件
    }
    sqlite3_finalize(i);
  } else if (event == "recalled") {
    // 投影跟随事件：recall=1（正文不动——留痕纪律）
    sqlite3_stmt* u = nullptr;
    ok = sqlite3_prepare_v2(db_,
                            "UPDATE messages SET recall = 1 WHERE msg_id = ?;",
                            -1, &u, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(u, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(u) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    }
    sqlite3_finalize(u);
  } else if (event == "edited") {
    // 投影跟随事件：正文替换（原 created payload 里原文永在——可审计）
    sqlite3_stmt* u = nullptr;
    ok = sqlite3_prepare_v2(db_,
                            "UPDATE messages SET text = ? WHERE msg_id = ?;",
                            -1, &u, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(u, 1, payload.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(u, 2, msg_id.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(u) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    }
    sqlite3_finalize(u);
  } else if (event == "purged") {
    // 平台-5 留存清除：投影物理移除；事件表留痕（重建跳过——清除可重现）
    sqlite3_stmt* d = nullptr;
    ok = sqlite3_prepare_v2(db_, "DELETE FROM messages WHERE msg_id = ?;",
                            -1, &d, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(d, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(d) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    }
    sqlite3_finalize(d);
  }
  // delivered/read：只记事件，不动投影
  if (ok) {
    sqlite3_stmt* e = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "INSERT INTO message_events(msg_id, event, by_account, payload,"
             " ts_ms) VALUES(?,?,?,?,?);",
             -1, &e, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(e, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(e, 2, event.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(e, 3, by_account.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(e, 4, payload.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(e, 5, ts_ms);
      ok = sqlite3_step(e) == SQLITE_DONE;
    }
    sqlite3_finalize(e);
  }
  if (ok) {
    ok = sqlite3_exec(db_, "RELEASE msg_evt;", nullptr, nullptr, &tx) ==
         SQLITE_OK;
  } else {
    sqlite3_exec(db_, "ROLLBACK TO msg_evt;", nullptr, nullptr, &tx);
    sqlite3_exec(db_, "RELEASE msg_evt;", nullptr, nullptr, &tx);
  }
  sqlite3_free(tx);
  return ok;
}

std::vector<ServerStore::MessageEvent> ServerStore::message_events(
    const std::string& msg_id) {
  std::vector<MessageEvent> out;
  std::string sql =
      "SELECT id, msg_id, event, by_account, payload, ts_ms"
      " FROM message_events";
  if (!msg_id.empty()) sql += " WHERE msg_id = ?";
  sql += " ORDER BY id;"; // 发生序（append-only 单调）
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
    return out;
  if (!msg_id.empty())
    sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    MessageEvent e;
    e.id = sqlite3_column_int64(st, 0);
    e.msg_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    e.event = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    e.by_account = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    e.payload = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    e.ts_ms = sqlite3_column_int64(st, 5);
    out.push_back(std::move(e));
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ArchivedMessage> ServerStore::rebuild_messages_from_events() {
  // 重放事件重建当前态：created 建（按首现序）、recalled 置标记、
  // edited 替正文、purged 移除（留存清除可重现）；其余事件不变形状
  std::vector<ArchivedMessage> out;
  std::map<std::string, std::size_t> index; // msg_id → out 下标
  std::set<std::string> purged;
  for (const auto& e : message_events("")) {
    if (e.event == "created") {
      nlohmann::json p = nlohmann::json::parse(e.payload, nullptr, false);
      if (p.is_discarded()) continue;
      ArchivedMessage m;
      m.msg_id = e.msg_id;
      m.from_account = p.value("from", std::string{});
      m.to_account = p.value("to", std::string{});
      m.type = p.value("type", 0);
      m.text = p.value("text", std::string{});
      m.ts_ms = p.value("ts_ms", std::int64_t{0});
      index[e.msg_id] = out.size();
      out.push_back(std::move(m));
    } else if (e.event == "recalled") {
      const auto it = index.find(e.msg_id);
      if (it != index.end()) out[it->second].recalled = true;
    } else if (e.event == "edited") {
      const auto it = index.find(e.msg_id);
      if (it != index.end() && !e.payload.empty()) out[it->second].text = e.payload;
    } else if (e.event == "purged") {
      purged.insert(e.msg_id);
    }
  }
  if (!purged.empty()) {
    std::vector<ArchivedMessage> kept;
    kept.reserve(out.size());
    for (auto& m : out) {
      if (!purged.count(m.msg_id)) kept.push_back(std::move(m));
    }
    out = std::move(kept);
  }
  return out;
}

// —— 平台-5 留存策略与 Retention Purge ——

bool ServerStore::retention_set(int days, const std::string& department_path,
                                const std::string& updated_by,
                                std::int64_t ts_ms) {
  // 白名单枚举（蓝图§十四）：30 天/6 个月/1 年/3 年/Indefinite
  if (days != 0 && days != 30 && days != 180 && days != 365 && days != 1095)
    return false;
  if (!department_path.empty()) {
    // 部门行须挂已存在部门（与 set_policy 同口径，不顺手建部门）
    bool found = false;
    for (const auto& [id, path] : department_list()) {
      (void)id;
      if (path == department_path) {
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  const char* sql =
      "INSERT INTO retention_policies(department_path, retention_days,"
      " updated_by, updated_ms) VALUES(?,?,?,?)"
      " ON CONFLICT(department_path) DO UPDATE SET"
      " retention_days = excluded.retention_days,"
      " updated_by = excluded.updated_by,"
      " updated_ms = excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, department_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, days);
  sqlite3_bind_text(st, 3, updated_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::RetentionPolicy> ServerStore::retention_list() {
  std::vector<RetentionPolicy> out;
  const char* sql =
      "SELECT id, department_path, retention_days, updated_by, updated_ms"
      " FROM retention_policies ORDER BY department_path = '' DESC,"
      " department_path ASC;"; // 全局行在前
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    RetentionPolicy p;
    p.id = sqlite3_column_int64(st, 0);
    p.department_path = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    p.retention_days = sqlite3_column_int(st, 2);
    p.updated_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    p.updated_ms = sqlite3_column_int64(st, 4);
    out.push_back(std::move(p));
  }
  sqlite3_finalize(st);
  return out;
}

ServerStore::RetentionPolicy ServerStore::retention_resolve(
    const std::string& account) {
  // 生效留存期：本人部门链逐级上溯（与 resolve_policy 同走法）→ 全局行 →
  // 内置默认 Indefinite（0）
  std::map<std::string, RetentionPolicy> by_path;
  for (const auto& p : retention_list()) by_path[p.department_path] = p;
  std::string path;
  if (const auto prof = member_profile(account)) {
    path = prof->department_path;
  }
  while (!path.empty()) {
    const auto it = by_path.find(path);
    if (it != by_path.end()) return it->second;
    const auto pos = path.rfind('/');
    if (pos == std::string::npos) break;
    path.resize(pos);
  }
  const auto g = by_path.find("");
  if (g != by_path.end()) return g->second;
  RetentionPolicy fallback; // 内置默认：Indefinite（现状口径）
  return fallback;
}

std::int64_t ServerStore::retention_purge(std::int64_t before_ms,
                                          const std::string& reason,
                                          const std::string& purged_by,
                                          const std::string& approved_by,
                                          int policy_days,
                                          std::int64_t ts_ms) {
  // 高风险三道闸：双人（两账号存在且不同）＋理由必填＋时间线有效
  if (before_ms <= 0) return 0;
  if (reason.empty()) return 0;
  if (purged_by.empty() || approved_by.empty()) return 0;
  if (purged_by == approved_by) return 0; // 双人审批：一人不得自批自清
  if (!find_account(purged_by) || !find_account(approved_by)) return 0;
  const auto victims = [&] {
    std::vector<std::string> ids;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT msg_id FROM messages WHERE ts_ms < ?"
                           " ORDER BY id;",
                           -1, &st, nullptr) != SQLITE_OK)
      return ids;
    sqlite3_bind_int64(st, 1, before_ms);
    while (sqlite3_step(st) == SQLITE_ROW) {
      ids.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)));
    }
    sqlite3_finalize(st);
    return ids;
  }();
  char* tx = nullptr;
  if (sqlite3_exec(db_, "SAVEPOINT purge;", nullptr, nullptr, &tx) !=
      SQLITE_OK) {
    sqlite3_free(tx);
    return 0;
  }
  bool ok = true;
  for (const auto& id : victims) {
    // 逐条落 purged 事件（事件与清除同事务——无痕删除被禁止）
    ok = append_message_event(id, "purged", purged_by, reason, ts_ms);
    if (!ok) break;
  }
  std::int64_t ledger_id = 0;
  if (ok) {
    sqlite3_stmt* i = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "INSERT INTO retention_purges(purged_by, approved_by, reason,"
             " policy_days, msg_count, before_ms, purged_ms)"
             " VALUES(?,?,?,?,?,?,?);",
             -1, &i, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(i, 1, purged_by.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(i, 2, approved_by.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(i, 3, reason.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(i, 4, policy_days);
      sqlite3_bind_int(i, 5, static_cast<int>(victims.size()));
      sqlite3_bind_int64(i, 6, before_ms);
      sqlite3_bind_int64(i, 7, ts_ms);
      ok = sqlite3_step(i) == SQLITE_DONE;
      if (ok) ledger_id = sqlite3_last_insert_rowid(db_);
    }
    sqlite3_finalize(i);
  }
  if (ok) { // 审计留痕复用查阅台账（purge 属敏感动作，谁何时清了几条）
    AuditReadRow a;
    a.op_account = purged_by;
    a.action = "purge";
    a.filters = "before_ms=" + std::to_string(before_ms) +
                " 批准=" + approved_by +
                " 理由=" + reason;
    a.result_count = static_cast<int>(victims.size());
    a.ts_ms = ts_ms;
    add_audit_read(a);
  }
  if (ok && ledger_id > 0) {
    ok = sqlite3_exec(db_, "RELEASE purge;", nullptr, nullptr, &tx) ==
         SQLITE_OK;
  } else {
    ok = false;
    sqlite3_exec(db_, "ROLLBACK TO purge;", nullptr, nullptr, &tx);
    sqlite3_exec(db_, "RELEASE purge;", nullptr, nullptr, &tx);
  }
  sqlite3_free(tx);
  return ok ? ledger_id : 0;
}

std::vector<ServerStore::RetentionPurge> ServerStore::retention_purges(
    int limit) {
  std::vector<RetentionPurge> out;
  const char* sql =
      "SELECT id, purged_by, approved_by, reason, policy_days, msg_count,"
      " before_ms, purged_ms FROM retention_purges ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int(st, 1, limit > 0 ? limit : 50);
  while (sqlite3_step(st) == SQLITE_ROW) {
    RetentionPurge p;
    p.id = sqlite3_column_int64(st, 0);
    p.purged_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    p.approved_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    p.reason = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    p.policy_days = sqlite3_column_int(st, 4);
    p.msg_count = sqlite3_column_int(st, 5);
    p.before_ms = sqlite3_column_int64(st, 6);
    p.purged_ms = sqlite3_column_int64(st, 7);
    out.push_back(std::move(p));
  }
  sqlite3_finalize(st);
  return out;
}

// —— 平台-11 远程协助（Security Domain 模型）——

namespace {
// 审计留痕（红线：require_audit 恒开）——每次状态迁移/策略变更必经此处
void assist_audit_log(sqlite3* db, const std::string& session_id,
                      const std::string& actor, const std::string& action,
                      const std::string& detail, std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO assist_audits(session_id, actor, action, detail, ts_ms)"
      " VALUES(?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return;
  sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, actor.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, action.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, detail.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  sqlite3_step(st);
  sqlite3_finalize(st);
}

bool assist_mask_valid(int m) {
  const int all = ServerStore::kAssistView | ServerStore::kAssistKeyboard |
                  ServerStore::kAssistMouse | ServerStore::kAssistClipboard |
                  ServerStore::kAssistFileTransfer;
  return m != 0 && (m & ~all) == 0;
}

// 会话行整体读出（列序同 SELECT 辅助）
ServerStore::AssistSession assist_row_read(sqlite3_stmt* st) {
  ServerStore::AssistSession s;
  s.id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
  s.requester = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
  s.target = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
  s.requested_mask = sqlite3_column_int(st, 3);
  s.granted_mask = sqlite3_column_int(st, 4);
  s.status = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
  s.requested_ms = sqlite3_column_int64(st, 6);
  s.approved_ms = sqlite3_column_int64(st, 7);
  s.started_ms = sqlite3_column_int64(st, 8);
  s.ended_ms = sqlite3_column_int64(st, 9);
  s.end_actor = reinterpret_cast<const char*>(sqlite3_column_text(st, 10));
  s.end_reason = reinterpret_cast<const char*>(sqlite3_column_text(st, 11));
  return s;
}

const char* kAssistSessionCols =
    "id, requester, target, requested_mask, granted_mask, status,"
    " requested_ms, approved_ms, started_ms, ended_ms, end_actor, end_reason";
} // namespace

bool ServerStore::assist_policy_set(bool allow,
                                    const std::string& department_path,
                                    const std::string& updated_by,
                                    std::int64_t ts_ms) {
  if (!department_path.empty()) {
    // 部门行须挂已存在部门（与 retention_set 同口径，不顺手建部门）
    bool found = false;
    for (const auto& [id, path] : department_list()) {
      (void)id;
      if (path == department_path) {
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  const char* sql =
      "INSERT INTO assist_policies(department_path, allow, updated_by,"
      " updated_ms) VALUES(?,?,?,?) ON CONFLICT(department_path) DO UPDATE"
      " SET allow = excluded.allow, updated_by = excluded.updated_by,"
      " updated_ms = excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, department_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, allow ? 1 : 0);
  sqlite3_bind_text(st, 3, updated_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (ok) {
    // 策略面动作 session_id 记空（段注口径），部门路径随 detail 留痕
    assist_audit_log(db_, "", updated_by, "policy",
                     (department_path.empty() ? std::string("（全局）")
                                              : department_path) +
                         (allow ? " → 放行" : " → 禁止"),
                     ts_ms);
  }
  return ok;
}

std::vector<ServerStore::AssistPolicy> ServerStore::assist_policy_list() {
  std::vector<AssistPolicy> out;
  const char* sql =
      "SELECT rowid, department_path, allow, updated_by, updated_ms"
      " FROM assist_policies ORDER BY department_path = '' DESC,"
      " department_path ASC;"; // 全局行在前
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    AssistPolicy p;
    p.id = sqlite3_column_int64(st, 0);
    p.department_path = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    p.allow = sqlite3_column_int(st, 2) != 0;
    p.updated_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    p.updated_ms = sqlite3_column_int64(st, 4);
    out.push_back(std::move(p));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::assist_policy_resolve(const std::string& account) {
  // 生效口径：本人部门链逐级上溯（与 retention_resolve 同走法）→ 全局行
  // → 内置默认禁止（白名单口径，从严）
  std::map<std::string, bool> by_path;
  for (const auto& p : assist_policy_list())
    by_path[p.department_path] = p.allow;
  std::string path;
  if (const auto prof = member_profile(account)) path = prof->department_path;
  while (!path.empty()) {
    const auto it = by_path.find(path);
    if (it != by_path.end()) return it->second;
    const auto pos = path.rfind('/');
    if (pos == std::string::npos) break;
    path.resize(pos);
  }
  const auto g = by_path.find("");
  if (g != by_path.end()) return g->second;
  return false; // 内置默认：禁止
}

std::string ServerStore::assist_request(const std::string& requester,
                                        const std::string& target, int mask,
                                        std::int64_t ts_ms) {
  // 发起面四闸：两账号存在、非同一人、双方部门均放行、权限集合法非零
  if (requester.empty() || target.empty() || requester == target) return "";
  if (!find_account(requester).has_value()) return "";
  if (!find_account(target).has_value()) return "";
  if (!assist_mask_valid(mask)) return "";
  if (!assist_policy_resolve(requester) || !assist_policy_resolve(target))
    return "";
  // 会话 id：ra-<sqlite PRNG hex16>（库内生成，不依赖外部熵源）
  std::string id;
  {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT lower(hex(randomblob(16)));", -1, &st,
                           nullptr) != SQLITE_OK)
      return "";
    if (sqlite3_step(st) == SQLITE_ROW)
      id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    if (id.empty()) return "";
    id.insert(0, "ra-");
  }
  const char* sql =
      "INSERT INTO assist_sessions(id, requester, target, requested_mask,"
      " granted_mask, status, requested_ms) VALUES(?,?,?,?,0,'requested',?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return "";
  sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, requester.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, target.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 4, mask);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok) return "";
  assist_audit_log(db_, id, requester, "request", "申请协助", ts_ms);
  return id;
}

std::optional<ServerStore::AssistSession> ServerStore::assist_session(
    const std::string& id) {
  const std::string sql =
      std::string("SELECT ") + kAssistSessionCols +
      " FROM assist_sessions WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
    return std::nullopt;
  sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<AssistSession> out;
  if (sqlite3_step(st) == SQLITE_ROW) out = assist_row_read(st);
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::AssistSession> ServerStore::assist_sessions(
    const std::string& account) {
  std::vector<AssistSession> out;
  std::string sql = std::string("SELECT ") + kAssistSessionCols +
                    " FROM assist_sessions";
  if (!account.empty()) sql += " WHERE requester = ? OR target = ?";
  sql += " ORDER BY requested_ms DESC, id ASC LIMIT 200;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
    return out;
  if (!account.empty()) {
    sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  }
  while (sqlite3_step(st) == SQLITE_ROW) out.push_back(assist_row_read(st));
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::assist_approve(const std::string& id, const std::string& by,
                                 int granted_mask, std::int64_t ts_ms) {
  // consent 红线：只有受控方本人可批；实批集 ⊆ 申请集且非零（可缩不可扩）
  const auto s = assist_session(id);
  if (!s || s->status != "requested" || by.empty() || by != s->target)
    return false;
  if (!assist_mask_valid(granted_mask) ||
      (granted_mask & ~s->requested_mask) != 0)
    return false;
  const char* sql =
      "UPDATE assist_sessions SET granted_mask = ?, status = 'approved',"
      " approved_ms = ? WHERE id = ? AND status = 'requested';";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(st, 1, granted_mask);
  sqlite3_bind_int64(st, 2, ts_ms);
  sqlite3_bind_text(st, 3, id.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  if (ok)
    assist_audit_log(db_, id, by, "approve", "批准协助（授出权限）", ts_ms);
  return ok;
}

bool ServerStore::assist_deny(const std::string& id, const std::string& by,
                              std::int64_t ts_ms) {
  // consent 红线：拒绝同样只属受控方本人；requested→denied 终态
  const auto s = assist_session(id);
  if (!s || s->status != "requested" || by.empty() || by != s->target)
    return false;
  const char* sql =
      "UPDATE assist_sessions SET status = 'denied', ended_ms = ?,"
      " end_actor = ?, end_reason = 'denied'"
      " WHERE id = ? AND status = 'requested';";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, ts_ms);
  sqlite3_bind_text(st, 2, by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, id.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  if (ok) assist_audit_log(db_, id, by, "deny", "拒绝协助", ts_ms);
  return ok;
}

bool ServerStore::assist_start(const std::string& id, const std::string& by,
                               std::int64_t ts_ms) {
  const auto s = assist_session(id);
  if (!s || s->status != "approved" ||
      (by != s->requester && by != s->target))
    return false;
  const char* sql =
      "UPDATE assist_sessions SET status = 'active', started_ms = ?"
      " WHERE id = ? AND status = 'approved';";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, ts_ms);
  sqlite3_bind_text(st, 2, id.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  if (ok) assist_audit_log(db_, id, by, "start", "开始协助", ts_ms);
  return ok;
}

bool ServerStore::assist_end(const std::string& id, const std::string& by,
                             const std::string& reason, std::int64_t ts_ms) {
  // active（协助中受控方=撤权即时生效）或 approved（批了没用上）都可结；
  // 终态 closed/denied 再动拒
  const auto s = assist_session(id);
  if (!s || (s->status != "active" && s->status != "approved") ||
      by.empty() || (by != s->requester && by != s->target))
    return false;
  const char* sql =
      "UPDATE assist_sessions SET status = 'closed', ended_ms = ?,"
      " end_actor = ?, end_reason = ?"
      " WHERE id = ? AND status IN ('active','approved');";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, ts_ms);
  sqlite3_bind_text(st, 2, by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, reason.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, id.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  if (ok)
    assist_audit_log(db_, id, by,
                     by == s->target ? "end(revoke)" : "end",
                     reason, ts_ms);
  return ok;
}

std::vector<ServerStore::AssistAuditRow> ServerStore::assist_audits(
    const std::string& session_id) {
  std::vector<AssistAuditRow> out;
  std::string sql =
      "SELECT id, session_id, actor, action, detail, ts_ms"
      " FROM assist_audits";
  if (!session_id.empty()) sql += " WHERE session_id = ?";
  sql += " ORDER BY id ASC LIMIT 500;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
    return out;
  if (!session_id.empty())
    sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    AssistAuditRow a;
    a.id = sqlite3_column_int64(st, 0);
    a.session_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    a.actor = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    a.action = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    a.detail = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    a.ts_ms = sqlite3_column_int64(st, 5);
    out.push_back(std::move(a));
  }
  sqlite3_finalize(st);
  return out;
}

// 消息检索：账号为空=全部；非空=该账号收发两侧＋其所在群的群消息都命中
//（管理员检索面；T4.1 起群消息 to="group:<群号>" 联入成员检索面）
std::vector<ArchivedMessage> ServerStore::messages(const std::string& account,
                                                   int limit) {
  MessageSearch q;
  q.account = account;
  q.limit = limit;
  return search_messages(q);
}

// T3.2 条件检索：账号（收发双侧）／时间窗（含端点）／关键词（子串，LIKE 转义）AND 组合
std::vector<ArchivedMessage> ServerStore::search_messages(
    const MessageSearch& q) {
  std::vector<ArchivedMessage> out;
  std::string sql =
      "SELECT msg_id, from_account, to_account, type, text, ts_ms, recall"
      " FROM messages WHERE 1=1";
  if (!q.account.empty()) {
    // 收发双侧 ＋ 该账号所在群的群消息（to="group:<群号>"）
    sql += " AND (from_account = ? OR to_account = ? OR to_account IN"
           " (SELECT 'group:' || group_id FROM group_members"
           "  WHERE account = ?))";
  }
  // 关键词子串匹配：%／_／转义符先转义，避免用户输入被当通配符
  std::string like;
  if (!q.keyword.empty()) {
    like.reserve(q.keyword.size() + 8);
    for (const char c : q.keyword) {
      if (c == '%' || c == '_' || c == '\\') like += '\\';
      like += c;
    }
    sql += " AND text LIKE ? ESCAPE '\\'";
  }
  if (q.since_ms > 0) sql += " AND ts_ms >= ?";
  if (q.until_ms > 0) sql += " AND ts_ms <= ?";
  sql += " ORDER BY id DESC LIMIT ?;";

  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  int idx = 1;
  if (!q.account.empty()) {
    sqlite3_bind_text(st, idx++, q.account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, idx++, q.account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, idx++, q.account.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (!like.empty()) {
    const std::string pat = "%" + like + "%";
    sqlite3_bind_text(st, idx++, pat.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (q.since_ms > 0) sqlite3_bind_int64(st, idx++, q.since_ms);
  if (q.until_ms > 0) sqlite3_bind_int64(st, idx++, q.until_ms);
  sqlite3_bind_int(st, idx, q.limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    ArchivedMessage m;
    m.msg_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    m.from_account = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    m.to_account = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    m.type = sqlite3_column_int(st, 3);
    const char* text_ptr =
        reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    m.text = text_ptr ? text_ptr : "";
    m.ts_ms = sqlite3_column_int64(st, 5);
    m.recalled = sqlite3_column_int(st, 6) != 0;
    out.push_back(std::move(m));
  }
  sqlite3_finalize(st);
  return out;
}

// 查阅留痕：只附加
bool ServerStore::add_audit_read(const AuditReadRow& rec) {
  const char* sql =
      "INSERT INTO audit_reads(op_account, action, filters, result_count, ts_ms)"
      " VALUES(?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, rec.op_account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, rec.action.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, rec.filters.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 4, rec.result_count);
  sqlite3_bind_int64(st, 5, rec.ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<AuditReadRow> ServerStore::audit_reads(int limit) {
  std::vector<AuditReadRow> out;
  const char* sql =
      "SELECT id, op_account, action, filters, result_count, ts_ms"
      " FROM audit_reads ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int(st, 1, limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    AuditReadRow r;
    r.id = sqlite3_column_int64(st, 0);
    const auto text_of = [&st](int col) {
      const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
      return p ? std::string(p) : std::string{};
    };
    r.op_account = text_of(1);
    r.action = text_of(2);
    r.filters = text_of(3);
    r.result_count = sqlite3_column_int(st, 4);
    r.ts_ms = sqlite3_column_int64(st, 5);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// 消息撤回：平台-4 起走 recalled 事件（事件与投影同事务落），
// 并保 recall_events 对账行（既有对账口径不变）
bool ServerStore::recall_message(const std::string& msg_id,
                                 const std::string& by_account,
                                 std::int64_t ts_ms) {
  if (message_from(msg_id).empty()) return false; // 消息不存在
  if (!append_message_event(msg_id, "recalled", by_account, "", ts_ms))
    return false;
  record_recall_event(msg_id, by_account, ts_ms); // 对账表尽力（主证在事件表）
  return true;
}

// 查询消息是否被撤回
bool ServerStore::is_recalled(const std::string& msg_id) {
  const char* sql = "SELECT recall FROM messages WHERE msg_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  bool recalled = false;
  if (sqlite3_step(st) == SQLITE_ROW) {
    recalled = sqlite3_column_int(st, 0) != 0;
  }
  sqlite3_finalize(st);
  return recalled;
}

// 查某消息的发送方（撤回权限判定：只能撤回自己发的）
std::string ServerStore::message_from(const std::string& msg_id) {
  const char* sql = "SELECT from_account FROM messages WHERE msg_id = ?;";
  sqlite3_stmt* st = nullptr;
  std::string out;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) {
    out = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
  }
  sqlite3_finalize(st);
  return out;
}

// 撤回事件独立记录（原文与序得在此表对账；只附加，不删改）
bool ServerStore::record_recall_event(const std::string& msg_id,
                                      const std::string& by_account,
                                      std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO recall_events(msg_id, by_account, ts_ms) VALUES(?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, by_account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::size_t ServerStore::recall_event_count(const std::string& msg_id) {
  const char* sql = "SELECT COUNT(*) FROM recall_events WHERE msg_id = ?;";
  sqlite3_stmt* st = nullptr;
  std::size_t n = 0;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) {
    n = static_cast<std::size_t>(sqlite3_column_int64(st, 0));
  }
  sqlite3_finalize(st);
  return n;
}

// —— T2.6 组织架构 ——

// 部门路径逐级创建："公司/研发部/客户端组" → 三级，已存在即复用
int ServerStore::ensure_department_path(const std::string& path) {
  if (path.empty()) return -1;
  int parent_id = -1;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::string seg =
        path.substr(start, slash == std::string::npos ? std::string::npos
                                                      : slash - start);
    if (seg.empty()) return -1; // 连续斜杠／尾斜杠等非法路径
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT id FROM departments WHERE name = ? AND"
                      " ((parent_id IS NULL AND ? = -1) OR parent_id = ?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, seg.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, parent_id);
    sqlite3_bind_int(st, 3, parent_id);
    int id = -1;
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (id < 0) {
      const char* ins =
          "INSERT INTO departments(name, parent_id, created_ms)"
          " VALUES(?, ?, ?);";
      if (sqlite3_prepare_v2(db_, ins, -1, &st, nullptr) != SQLITE_OK) {
        return -1;
      }
      sqlite3_bind_text(st, 1, seg.c_str(), -1, SQLITE_TRANSIENT);
      if (parent_id < 0) {
        sqlite3_bind_null(st, 2);
      } else {
        sqlite3_bind_int(st, 2, parent_id);
      }
      sqlite3_bind_int64(st, 3, now_ms());
      if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
      }
      sqlite3_finalize(st);
      id = static_cast<int>(sqlite3_last_insert_rowid(db_));
    }
    parent_id = id;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return parent_id;
}

std::vector<std::pair<int, std::string>> ServerStore::department_list() {
  std::vector<std::pair<int, std::string>> out;
  // 自底向上拼全路径（递归 CTE；SQLite ≥3.8.3）
  const char* sql =
      "WITH RECURSIVE tree(id, name, path) AS ("
      " SELECT id, name, name FROM departments WHERE parent_id IS NULL"
      " UNION ALL"
      " SELECT d.id, d.name, tree.path || '/' || d.name"
      "  FROM departments d JOIN tree ON d.parent_id = tree.id)"
      " SELECT id, path FROM tree ORDER BY path;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    out.emplace_back(sqlite3_column_int(st, 0),
                     reinterpret_cast<const char*>(sqlite3_column_text(st, 1)));
  }
  sqlite3_finalize(st);
  return out;
}

std::string ServerStore::department_path(int id) {
  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT path FROM ("
                    "WITH RECURSIVE tree(id, name, path) AS ("
                    " SELECT id, name, name FROM departments WHERE parent_id IS NULL"
                    " UNION ALL"
                    " SELECT d.id, d.name, tree.path || '/' || d.name"
                    "  FROM departments d JOIN tree ON d.parent_id = tree.id)"
                    " SELECT id, path FROM tree) WHERE id = ?;";
  std::string out;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int(st, 1, id);
  if (sqlite3_step(st) == SQLITE_ROW) {
    out = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::set_member_profile(const std::string& account,
                                     int department_id, const std::string& title,
                                     const std::string& manager) {
  if (!find_account(account)) return false;           // 账号不存在
  if (manager == account) return false;               // 不得自为上级
  if (!manager.empty() && !find_account(manager)) {
    return false;                                     // 上级账号不存在
  }
  if (!manager.empty()) {
    // 环校验：从拟设上级沿现有链路上溯，若回到本人则构成环
    std::string cur = manager;
    std::set<std::string> seen;
    while (!cur.empty() && cur != account) {
      if (!seen.insert(cur).second) break; // 既有环防御，止步
      const auto p = member_profile(cur);
      if (!p) break;
      if (p->manager == account) return false; // 链路回到本人：环
      cur = p->manager;
    }
  }
  const char* sql =
      "INSERT INTO member_profiles(account, department_id, title, manager,"
      " updated_ms) VALUES(?, ?, ?, ?, ?)"
      " ON CONFLICT(account) DO UPDATE SET department_id = excluded.department_id,"
      " title = excluded.title, manager = excluded.manager,"
      " updated_ms = excluded.updated_ms;";
  if (!find_account(account)) return false;           // 账号不存在
  if (manager == account) return false;               // 不得自为上级
  if (!manager.empty() && !find_account(manager)) {
    return false;                                     // 上级账号不存在
  }
  if (!manager.empty()) {
    // 环校验：从拟设上级沿权威表链路上溯，若回到本人则构成环
    std::string cur = manager;
    std::set<std::string> seen;
    while (!cur.empty() && cur != account) {
      if (!seen.insert(cur).second) break; // 既有环防御，止步
      sqlite3_stmt* s = nullptr;
      if (sqlite3_prepare_v2(
              db_,
              "SELECT manager_account FROM org_reporting_lines"
              " WHERE account = ?;",
              -1, &s, nullptr) != SQLITE_OK)
        return false;
      sqlite3_bind_text(s, 1, cur.c_str(), -1, SQLITE_TRANSIENT);
      std::string next;
      if (sqlite3_step(s) == SQLITE_ROW &&
          sqlite3_column_type(s, 0) != SQLITE_NULL) {
        next = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
      }
      sqlite3_finalize(s);
      if (next == account) return false; // 链路回到本人：环
      cur = next;
    }
  }
  // 平台-3：三关联权威表与单值镜像同事务写（member_profiles 的
  // department_id/manager 列=单值视图，消费者 member_profile/manager_chain
  // 不改语义；多部门/链路独立维护走 membership_*/reporting_*）。
  // SAVEPOINT 而非 BEGIN：import_members 在外层事务内逐行调用本函数
  char* tx = nullptr;
  if (sqlite3_exec(db_, "SAVEPOINT mprof;", nullptr, nullptr, &tx) !=
      SQLITE_OK) {
    sqlite3_free(tx);
    return false;
  }
  bool ok = true;
  {
    sqlite3_stmt* d = nullptr;
    ok = sqlite3_prepare_v2(db_,
                            "DELETE FROM org_memberships WHERE account = ?;",
                            -1, &d, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(d, 1, account.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(d) == SQLITE_DONE;
    }
    sqlite3_finalize(d);
  }
  if (ok && department_id >= 0) {
    sqlite3_stmt* i = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "INSERT INTO org_memberships(account, department_id, created_ms)"
             " VALUES(?,?,?);",
             -1, &i, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(i, 1, account.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(i, 2, department_id);
      sqlite3_bind_int64(i, 3, now_ms());
      ok = sqlite3_step(i) == SQLITE_DONE;
    }
    sqlite3_finalize(i);
  }
  if (ok) {
    sqlite3_stmt* r = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "DELETE FROM org_reporting_lines WHERE account = ?;",
             -1, &r, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(r, 1, account.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(r) == SQLITE_DONE;
    }
    sqlite3_finalize(r);
  }
  if (ok && !manager.empty()) {
    sqlite3_stmt* i = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "INSERT INTO org_reporting_lines(account, manager_account,"
             " created_ms) VALUES(?,?,?);",
             -1, &i, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(i, 1, account.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(i, 2, manager.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(i, 3, now_ms());
      ok = sqlite3_step(i) == SQLITE_DONE;
    }
    sqlite3_finalize(i);
  }
  if (ok) {
    sqlite3_stmt* st = nullptr;
    ok = sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
      if (department_id < 0) {
        sqlite3_bind_null(st, 2);
      } else {
        sqlite3_bind_int(st, 2, department_id);
      }
      sqlite3_bind_text(st, 3, title.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 4, manager.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(st, 5, now_ms());
      ok = sqlite3_step(st) == SQLITE_DONE;
    }
    sqlite3_finalize(st);
  }
  if (ok) {
    ok = sqlite3_exec(db_, "RELEASE mprof;", nullptr, nullptr, &tx) == SQLITE_OK;
  } else {
    sqlite3_exec(db_, "ROLLBACK TO mprof;", nullptr, nullptr, &tx);
    sqlite3_exec(db_, "RELEASE mprof;", nullptr, nullptr, &tx);
  }
  sqlite3_free(tx);
  return ok;
}

std::optional<MemberProfile> ServerStore::member_profile(
    const std::string& account) {
  const char* sql =
      "SELECT p.title, p.manager, p.department_id,"
      " a.display_name, a.role FROM member_profiles p"
      " JOIN accounts a ON a.account = p.account WHERE p.account = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<MemberProfile> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    MemberProfile m;
    m.account = account;
    m.title = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    m.manager = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    if (sqlite3_column_type(st, 2) != SQLITE_NULL) {
      m.department_path = department_path(sqlite3_column_int(st, 2));
    }
    m.display_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    m.role = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    out = m;
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<MemberProfile> ServerStore::member_list() {
  std::vector<MemberProfile> out;
  const char* sql =
      "SELECT a.account, a.display_name, a.role, p.title, p.manager,"
      " p.department_id FROM accounts a"
      " LEFT JOIN member_profiles p ON p.account = a.account"
      " ORDER BY a.account;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    MemberProfile m;
    m.account = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    m.display_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    m.role = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    if (sqlite3_column_type(st, 3) != SQLITE_NULL) {
      m.title = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    }
    if (sqlite3_column_type(st, 4) != SQLITE_NULL) {
      m.manager = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    }
    if (sqlite3_column_type(st, 5) != SQLITE_NULL) {
      m.department_path = department_path(sqlite3_column_int(st, 5));
    }
    out.push_back(std::move(m));
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<std::string> ServerStore::manager_chain(
    const std::string& account) {
  std::vector<std::string> chain;
  std::set<std::string> seen{account};
  std::string cur = account;
  // 平台-3：沿权威表 org_reporting_lines 逐级上溯（镜像列不入链路判定）
  while (true) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT manager_account FROM org_reporting_lines"
            " WHERE account = ?;",
            -1, &s, nullptr) != SQLITE_OK)
      break;
    sqlite3_bind_text(s, 1, cur.c_str(), -1, SQLITE_TRANSIENT);
    std::string next;
    if (sqlite3_step(s) == SQLITE_ROW &&
        sqlite3_column_type(s, 0) != SQLITE_NULL) {
      next = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
    }
    sqlite3_finalize(s);
    if (next.empty()) break;
    if (!seen.insert(next).second) break; // 环防御
    chain.push_back(next);
    cur = next;
  }
  return chain;
}

// —— 平台-3：组织三关联（成员/授权/汇报线）——

bool ServerStore::membership_add(const std::string& account, int department_id,
                                 std::int64_t ts_ms) {
  if (!find_account(account).has_value()) return false;
  if (department_id < 0) return false;
  sqlite3_stmt* d = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT 1 FROM departments WHERE id = ?;", -1,
                         &d, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_int(d, 1, department_id);
  const bool dept_ok = sqlite3_step(d) == SQLITE_ROW;
  sqlite3_finalize(d);
  if (!dept_ok) return false;
  const char* sql =
      "INSERT INTO org_memberships(account, department_id, created_ms)"
      " VALUES(?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, department_id);
  sqlite3_bind_int64(st, 3, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE; // 唯一冲突（已入）→ false
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::membership_remove(const std::string& account,
                                    int department_id) {
  const char* sql =
      "DELETE FROM org_memberships WHERE account = ? AND department_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, department_id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::OrgMembership> ServerStore::memberships_of(
    const std::string& account) {
  std::vector<OrgMembership> out;
  const char* sql =
      "SELECT id, account, department_id, created_ms FROM org_memberships"
      " WHERE account = ? ORDER BY id;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    OrgMembership m;
    m.id = sqlite3_column_int64(st, 0);
    m.account = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    m.department_id = sqlite3_column_int(st, 2);
    m.created_ms = sqlite3_column_int64(st, 3);
    out.push_back(std::move(m));
  }
  sqlite3_finalize(st);
  return out;
}

std::int64_t ServerStore::role_grant(const std::string& account,
                                     const std::string& role,
                                     const std::string& scope,
                                     std::int64_t valid_from_ms,
                                     std::int64_t valid_until_ms,
                                     const std::string& granted_by,
                                     std::int64_t ts_ms) {
  if (!find_account(account).has_value()) return 0;
  if (role.empty()) return 0;
  // 时间窗倒置拒（0 端点=不开窗不参与比较）
  if (valid_from_ms > 0 && valid_until_ms > 0 && valid_from_ms >= valid_until_ms)
    return 0;
  const char* sql =
      "INSERT INTO org_role_assignments(account, role, scope, valid_from_ms,"
      " valid_until_ms, granted_by, created_ms) VALUES(?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, role.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, scope.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, valid_from_ms);
  sqlite3_bind_int64(st, 5, valid_until_ms);
  sqlite3_bind_text(st, 6, granted_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 7, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok ? sqlite3_last_insert_rowid(db_) : 0;
}

bool ServerStore::role_revoke(std::int64_t id) {
  const char* sql = "DELETE FROM org_role_assignments WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::vector<std::string> ServerStore::effective_roles(
    const std::string& account, std::int64_t at_ms) {
  std::vector<std::string> out;
  // 基础角色（accounts.role）
  sqlite3_stmt* b = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT role FROM accounts WHERE account = ?;",
                         -1, &b, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(b, 1, account.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(b) == SQLITE_ROW && sqlite3_column_type(b, 0) != SQLITE_NULL) {
      out.push_back(reinterpret_cast<const char*>(sqlite3_column_text(b, 0)));
    }
    sqlite3_finalize(b);
  }
  // 窗内追加授权：from=0 不设下界；until=0 不设上界；否则 from<=at<until
  sqlite3_stmt* st = nullptr;
  const char* sql =
      "SELECT role FROM org_role_assignments WHERE account = ?"
      " AND (valid_from_ms = 0 OR valid_from_ms <= ?)"
      " AND (valid_until_ms = 0 OR ? < valid_until_ms) ORDER BY id;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, at_ms);
  sqlite3_bind_int64(st, 3, at_ms);
  std::set<std::string> seen(out.begin(), out.end());
  while (sqlite3_step(st) == SQLITE_ROW) {
    if (sqlite3_column_type(st, 0) == SQLITE_NULL) continue;
    std::string role = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    if (role.empty() || !seen.insert(role).second) continue;
    out.push_back(std::move(role));
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::RoleAssignment> ServerStore::role_assignments(
    const std::string& account) {
  std::vector<RoleAssignment> out;
  std::string sql =
      "SELECT id, account, role, scope, valid_from_ms, valid_until_ms,"
      " granted_by, created_ms FROM org_role_assignments";
  if (!account.empty()) sql += " WHERE account = ?";
  sql += " ORDER BY id;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
    return out;
  if (!account.empty())
    sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    RoleAssignment r;
    r.id = sqlite3_column_int64(st, 0);
    r.account = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.role = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.scope = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.valid_from_ms = sqlite3_column_int64(st, 4);
    r.valid_until_ms = sqlite3_column_int64(st, 5);
    r.granted_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.created_ms = sqlite3_column_int64(st, 7);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::reporting_set(const std::string& account,
                                const std::string& manager,
                                std::int64_t ts_ms) {
  if (!find_account(account).has_value()) return false;
  if (manager.empty() || manager == account) return false; // 自为上级拒
  if (!find_account(manager).has_value()) return false;
  // 环校验：manager 的上游链不得回到 account（沿权威表走）
  {
    std::set<std::string> seen{account};
    std::string cur = manager;
    int guard = 0;
    while (!cur.empty() && guard++ < 256) {
      if (!seen.insert(cur).second) return false; // manager 已在环上
      if (cur == account) return false;
      sqlite3_stmt* s = nullptr;
      if (sqlite3_prepare_v2(
              db_,
              "SELECT manager_account FROM org_reporting_lines"
              " WHERE account = ?;",
              -1, &s, nullptr) != SQLITE_OK)
        return false;
      sqlite3_bind_text(s, 1, cur.c_str(), -1, SQLITE_TRANSIENT);
      std::string next;
      if (sqlite3_step(s) == SQLITE_ROW &&
          sqlite3_column_type(s, 0) != SQLITE_NULL) {
        next = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
      }
      sqlite3_finalize(s);
      cur = next;
    }
  }
  char* tx = nullptr;
  if (sqlite3_exec(db_, "SAVEPOINT rpt;", nullptr, nullptr, &tx) != SQLITE_OK) {
    sqlite3_free(tx);
    return false;
  }
  bool ok = true;
  {
    sqlite3_stmt* d = nullptr;
    ok = sqlite3_prepare_v2(
             db_, "DELETE FROM org_reporting_lines WHERE account = ?;", -1, &d,
             nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(d, 1, account.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(d) == SQLITE_DONE;
    }
    sqlite3_finalize(d);
  }
  if (ok) {
    sqlite3_stmt* i = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "INSERT INTO org_reporting_lines(account, manager_account,"
             " created_ms) VALUES(?,?,?);",
             -1, &i, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(i, 1, account.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(i, 2, manager.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(i, 3, ts_ms);
      ok = sqlite3_step(i) == SQLITE_DONE;
    }
    sqlite3_finalize(i);
  }
  if (ok) { // 镜像列同步（有档案才更；无档案不建档）
    sqlite3_stmt* u = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "UPDATE member_profiles SET manager = ?, updated_ms = ?"
             " WHERE account = ?;",
             -1, &u, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(u, 1, manager.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(u, 2, now_ms());
      sqlite3_bind_text(u, 3, account.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(u) == SQLITE_DONE;
    }
    sqlite3_finalize(u);
  }
  if (ok) {
    ok = sqlite3_exec(db_, "RELEASE rpt;", nullptr, nullptr, &tx) == SQLITE_OK;
  } else {
    sqlite3_exec(db_, "ROLLBACK TO rpt;", nullptr, nullptr, &tx);
    sqlite3_exec(db_, "RELEASE rpt;", nullptr, nullptr, &tx);
  }
  sqlite3_free(tx);
  return ok;
}

bool ServerStore::reporting_clear(const std::string& account) {
  if (!find_account(account).has_value()) return false;
  char* tx = nullptr;
  if (sqlite3_exec(db_, "SAVEPOINT rpt;", nullptr, nullptr, &tx) != SQLITE_OK) {
    sqlite3_free(tx);
    return false;
  }
  bool ok = true;
  {
    sqlite3_stmt* d = nullptr;
    ok = sqlite3_prepare_v2(
             db_, "DELETE FROM org_reporting_lines WHERE account = ?;", -1, &d,
             nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(d, 1, account.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(d) == SQLITE_DONE;
    }
    sqlite3_finalize(d);
  }
  if (ok) { // 镜像列同步（有档案才清；无档案不动）
    sqlite3_stmt* u = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "UPDATE member_profiles SET manager = '', updated_ms = ?"
             " WHERE account = ?;",
             -1, &u, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_int64(u, 1, now_ms());
      sqlite3_bind_text(u, 2, account.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(u) == SQLITE_DONE;
    }
    sqlite3_finalize(u);
  }
  if (ok) {
    ok = sqlite3_exec(db_, "RELEASE rpt;", nullptr, nullptr, &tx) == SQLITE_OK;
  } else {
    sqlite3_exec(db_, "ROLLBACK TO rpt;", nullptr, nullptr, &tx);
    sqlite3_exec(db_, "RELEASE rpt;", nullptr, nullptr, &tx);
  }
  sqlite3_free(tx);
  return ok;
}

OrgImportResult ServerStore::import_members(
    const std::vector<OrgImportRow>& rows) {
  OrgImportResult result;
  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  for (const OrgImportRow& r : rows) {
    sqlite3_exec(db_, "SAVEPOINT imp;", nullptr, nullptr, nullptr);
    int dept_id = -1;
    std::string error;
    if (!find_account(r.account)) {
      error = "账号不存在";
    } else if (r.manager == r.account) {
      error = "不得自为直属上级";
    } else if (!r.manager.empty() && !find_account(r.manager)) {
      error = "直属上级账号不存在";
    } else if (!r.dept.empty() && (dept_id = ensure_department_path(r.dept)) < 0) {
      error = "部门路径非法";
    }
    if (error.empty() && !set_member_profile(r.account, dept_id, r.title,
                                             r.manager)) {
      error = "构成汇报环或写入失败";
    }
    if (error.empty()) {
      sqlite3_exec(db_, "RELEASE imp;", nullptr, nullptr, nullptr);
      ++result.imported;
    } else {
      sqlite3_exec(db_, "ROLLBACK TO imp;", nullptr, nullptr, nullptr);
      sqlite3_exec(db_, "RELEASE imp;", nullptr, nullptr, nullptr);
      result.errors.push_back("第 " + std::to_string(r.line_no) + " 行（" +
                              r.account + "）被拒绝：" + error);
    }
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return result;
}

// —— T3.4 策略开关 ——

bool ServerStore::set_policy(const std::string& department_path,
                             bool allow_anonymous, bool allow_cross_state,
                             bool new_device_approval,
                             bool allow_cross_dept_file,
                             bool allow_forward_file) {
  if (!department_path.empty()) {
    // 部门行须挂已存在部门（防拼错挂空名；不顺手建部门）
    bool found = false;
    for (const auto& [id, path] : department_list()) {
      (void)id;
      if (path == department_path) {
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  const char* sql =
      "INSERT INTO policies(department_path, allow_anonymous, allow_cross_state,"
      " new_device_approval, allow_cross_dept_file, allow_forward_file)"
      " VALUES(?, ?, ?, ?, ?, ?)"
      " ON CONFLICT(department_path) DO UPDATE SET"
      " allow_anonymous = excluded.allow_anonymous,"
      " allow_cross_state = excluded.allow_cross_state,"
      " new_device_approval = excluded.new_device_approval,"
      " allow_cross_dept_file = excluded.allow_cross_dept_file,"
      " allow_forward_file = excluded.allow_forward_file;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, department_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, allow_anonymous ? 1 : 0);
  sqlite3_bind_int(st, 3, allow_cross_state ? 1 : 0);
  sqlite3_bind_int(st, 4, new_device_approval ? 1 : 0);
  sqlite3_bind_int(st, 5, allow_cross_dept_file ? 1 : 0);
  sqlite3_bind_int(st, 6, allow_forward_file ? 1 : 0);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<PolicyRow> ServerStore::policy_list() {
  std::vector<PolicyRow> out;
  const char* sql =
      "SELECT department_path, allow_anonymous, allow_cross_state,"
      " new_device_approval, allow_cross_dept_file, allow_forward_file"
      " FROM policies"
      " ORDER BY department_path = '' DESC, department_path ASC;"; // 全局行在前
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    PolicyRow p;
    const char* path_ptr =
        reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    p.department_path = path_ptr ? path_ptr : "";
    p.allow_anonymous = sqlite3_column_int(st, 1) != 0;
    p.allow_cross_state = sqlite3_column_int(st, 2) != 0;
    p.new_device_approval = sqlite3_column_int(st, 3) != 0;
    p.allow_cross_dept_file = sqlite3_column_int(st, 4) != 0;
    p.allow_forward_file = sqlite3_column_int(st, 5) != 0;
    out.push_back(std::move(p));
  }
  sqlite3_finalize(st);
  return out;
}

// 生效策略：本人部门 → 逐级上级部门（路径去尾）→ 全局行 → 内置默认
PolicyRow ServerStore::resolve_policy(const std::string& account) {
  const auto all = [&] {
    std::map<std::string, PolicyRow> by_path;
    for (const auto& p : policy_list()) by_path[p.department_path] = p;
    return by_path;
  }();
  std::string path;
  if (const auto prof = member_profile(account)) {
    path = prof->department_path;
  }
  while (!path.empty()) {
    const auto it = all.find(path);
    if (it != all.end()) return it->second;
    const auto pos = path.rfind('/');
    if (pos == std::string::npos) break;
    path.resize(pos);
  }
  if (const auto it = all.find(""); it != all.end()) return it->second;
  PolicyRow fallback; // 内置默认：宽松（免登录可用、跨态可通、新设备免审批）
  return fallback;
}

// —— T4.6 通讯录可见性 ——

bool ServerStore::set_visibility(const std::string& scope,
                                 const std::string& key, bool hidden,
                                 bool restrict_scope,
                                 const std::string& hide_fields) {
  if (scope != "member" && scope != "dept") return false;
  if (key.empty()) return false;
  if (scope == "member") {
    if (!find_account(key).has_value()) return false;
  } else {
    bool exists = false;
    for (const auto& [id, path] : department_list()) {
      (void)id;
      if (path == key) {
        exists = true;
        break;
      }
    }
    if (!exists) return false;
  }
  const char* sql =
      "INSERT INTO org_visibility(scope, target_key, hidden, restrict_scope,"
      " hide_fields) VALUES(?,?,?,?,?)"
      " ON CONFLICT(scope, target_key) DO UPDATE SET"
      " hidden=excluded.hidden, restrict_scope=excluded.restrict_scope,"
      " hide_fields=excluded.hide_fields;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, scope.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, hidden ? 1 : 0);
  sqlite3_bind_int(st, 4, restrict_scope ? 1 : 0);
  sqlite3_bind_text(st, 5, hide_fields.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::optional<VisibilityRow> ServerStore::visibility_row(
    const std::string& scope, const std::string& key) {
  const char* sql =
      "SELECT hidden, restrict_scope, hide_fields FROM org_visibility"
      " WHERE scope=? AND target_key=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, scope.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, key.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<VisibilityRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    VisibilityRow r;
    r.scope = scope;
    r.key = key;
    r.hidden = sqlite3_column_int(st, 0) != 0;
    r.restrict_scope = sqlite3_column_int(st, 1) != 0;
    if (sqlite3_column_type(st, 2) != SQLITE_NULL) {
      r.hide_fields = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    }
    out = r;
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<VisibilityRow> ServerStore::visibility_list() {
  std::vector<VisibilityRow> out;
  const char* sql =
      "SELECT scope, target_key, hidden, restrict_scope, hide_fields"
      " FROM org_visibility ORDER BY scope, target_key;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    VisibilityRow r;
    r.scope = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    r.key = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.hidden = sqlite3_column_int(st, 2) != 0;
    r.restrict_scope = sqlite3_column_int(st, 3) != 0;
    if (sqlite3_column_type(st, 4) != SQLITE_NULL) {
      r.hide_fields = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    }
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::add_visibility_allow(const std::string& viewer,
                                       const std::string& target) {
  if (viewer.empty() || target.empty()) return false;
  const char* sql =
      "INSERT OR IGNORE INTO org_visibility_allow(viewer, target)"
      " VALUES(?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, viewer.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, target.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::remove_visibility_allow(const std::string& viewer,
                                          const std::string& target) {
  const char* sql =
      "DELETE FROM org_visibility_allow WHERE viewer=? AND target=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, viewer.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, target.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<VisibilityAllow> ServerStore::visibility_allows() {
  std::vector<VisibilityAllow> out;
  const char* sql =
      "SELECT viewer, target FROM org_visibility_allow"
      " ORDER BY viewer, target;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    VisibilityAllow a;
    a.viewer = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    a.target = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    out.push_back(std::move(a));
  }
  sqlite3_finalize(st);
  return out;
}

namespace {

// 过滤上下文（一次计算，member/dept 两表共用）：查看者角色与部门、白名单
// 目标集、生效的限看范围、全部配置行。
struct VisCtx {
  bool is_admin{false};
  std::string vdept;              // 查看者所在部门（空=未分配）
  std::set<std::string> allows;   // 白名单目标（账号或部门全路径）
  std::string limit_dept;         // 生效的限看部门（空=不限）
  std::vector<VisibilityRow> rows; // 全部配置行
};

} // namespace

std::vector<MemberProfile> ServerStore::visible_members(
    const std::string& viewer) {
  VisCtx ctx;
  ctx.rows = visibility_list();
  // 平台-3：admin 判定走有效角色（基础 ∪ 窗内追加授权）
  const auto vroles = effective_roles(viewer, now_ms());
  ctx.is_admin = std::find(vroles.begin(), vroles.end(), "admin") != vroles.end();
  if (const auto vprof = member_profile(viewer)) {
    ctx.vdept = vprof->department_path;
  }
  for (const auto& a : visibility_allows()) {
    if (a.viewer == viewer) ctx.allows.insert(a.target);
  }
  if (!ctx.is_admin) {
    // 限看本部门：本人部门链上最近的 restrict 行（与策略解析同口径）
    std::string path = ctx.vdept;
    while (!path.empty()) {
      if (const auto r = visibility_row("dept", path); r && r->restrict_scope) {
        ctx.limit_dept = path;
        break;
      }
      const auto pos = path.rfind('/');
      if (pos == std::string::npos) break;
      path.resize(pos);
    }
  }

  const auto whitelisted = [&](const std::string& account,
                               const std::string& dept) {
    if (ctx.allows.count(account) != 0) return true;
    for (const std::string& t : ctx.allows) {
      if (dept_covers(t, dept)) return true;
    }
    return false;
  };
  const auto dept_hidden_outside =
      [&](const std::string& dept) { // 部门被隐藏且查看者不在其内
      if (dept.empty()) return false;
      for (const auto& r : ctx.rows) {
        if (r.scope == "dept" && r.hidden && dept_covers(r.key, dept) &&
            !dept_covers(r.key, ctx.vdept)) {
          return true;
        }
      }
      return false;
    };

  std::vector<MemberProfile> out;
  for (const auto& m : member_list()) {
    if (ctx.is_admin || m.account == viewer) {
      out.push_back(m); // 管理员全量；查看者本人始终在列
      continue;
    }
    const bool allow = whitelisted(m.account, m.department_path);
    if (!allow) {
      // ① 限看本部门：非白名单成员须落在限看部门子树内
      if (!ctx.limit_dept.empty() &&
          !dept_covers(ctx.limit_dept, m.department_path)) {
        continue;
      }
      // ② 隐藏成员；③ 隐藏部门（整树）——部门内自己人互见
      bool hidden = false;
      if (const auto r = visibility_row("member", m.account);
          r && r->hidden) {
        hidden = true;
      }
      if (!hidden) hidden = dept_hidden_outside(m.department_path);
      if (hidden) continue;
    }
    // ④ 敏感字段脱敏（非管理员、非白名单）
    MemberProfile m2 = m;
    if (!allow) {
      if (const auto r = visibility_row("member", m.account)) {
        if (csv_field_has(r->hide_fields, "title")) m2.title.clear();
        if (csv_field_has(r->hide_fields, "manager")) m2.manager.clear();
        if (csv_field_has(r->hide_fields, "role")) m2.role.clear();
      }
    }
    out.push_back(std::move(m2));
  }
  // ⑤ 上级引用随可见性走：直属上级若不可见则抹去（不留不可见者的账号线索）
  std::set<std::string> visible_accounts;
  for (const auto& m : out) visible_accounts.insert(m.account);
  for (auto& m : out) {
    if (!m.manager.empty() && visible_accounts.count(m.manager) == 0) {
      m.manager.clear();
    }
  }
  return out;
}

std::vector<std::pair<int, std::string>> ServerStore::visible_departments(
    const std::string& viewer) {
  // 与 visible_members 同规则；另保证可见成员所在部门链完整
  //（客户端按全路径成树，链缺一级成员就挂不上）
  VisCtx ctx;
  ctx.rows = visibility_list();
  const auto vroles = effective_roles(viewer, now_ms());
  ctx.is_admin = std::find(vroles.begin(), vroles.end(), "admin") != vroles.end();
  if (const auto vprof = member_profile(viewer)) {
    ctx.vdept = vprof->department_path;
  }
  for (const auto& a : visibility_allows()) {
    if (a.viewer == viewer) ctx.allows.insert(a.target);
  }
  if (!ctx.is_admin) {
    std::string path = ctx.vdept;
    while (!path.empty()) {
      if (const auto r = visibility_row("dept", path); r && r->restrict_scope) {
        ctx.limit_dept = path;
        break;
      }
      const auto pos = path.rfind('/');
      if (pos == std::string::npos) break;
      path.resize(pos);
    }
  }
  std::set<std::string> needed; // 可见成员所在部门链（含各级前缀）
  for (const auto& m : visible_members(viewer)) {
    std::string path = m.department_path;
    while (!path.empty()) {
      needed.insert(path);
      const auto pos = path.rfind('/');
      if (pos == std::string::npos) break;
      path.resize(pos);
    }
  }
  std::vector<std::pair<int, std::string>> out;
  for (const auto& [id, path] : department_list()) {
    if (needed.count(path) != 0) {
      out.emplace_back(id, path);
      continue;
    }
    if (ctx.is_admin) {
      out.emplace_back(id, path);
      continue;
    }
    if (!ctx.limit_dept.empty() && !dept_covers(ctx.limit_dept, path)) continue;
    bool skip = false;
    for (const auto& r : ctx.rows) {
      if (r.scope != "dept" || !r.hidden || !dept_covers(r.key, path)) continue;
      const bool in_dept = dept_covers(r.key, ctx.vdept);
      const bool allow = ctx.allows.count(r.key) != 0 ||
                         ctx.allows.count(path) != 0;
      if (!in_dept && !allow) skip = true;
    }
    if (skip) continue;
    out.emplace_back(id, path);
  }
  return out;
}

// —— T4.1 群聊 ——

std::uint64_t ServerStore::create_group(const std::string& name,
                                        const std::string& owner,
                                        const std::vector<std::string>& members) {
  if (name.empty() || owner.empty()) return 0;
  // 成员账号存在性校验（建群者一并校验）；去重
  std::vector<std::string> uniq;
  for (const auto& a : members) {
    if (std::find(uniq.begin(), uniq.end(), a) == uniq.end()) uniq.push_back(a);
  }
  for (const auto& a : uniq) {
    if (!find_account(a).has_value()) return 0;
  }
  if (std::find(uniq.begin(), uniq.end(), owner) == uniq.end()) {
    uniq.push_back(owner);
  }

  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  const char* sql =
      "INSERT INTO groups(name, owner, announcement, created_ms)"
      " VALUES(?, ?, '', ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return 0;
  }
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, now_ms());
  if (sqlite3_step(st) != SQLITE_DONE) {
    sqlite3_finalize(st);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return 0;
  }
  sqlite3_finalize(st);
  const std::uint64_t gid =
      static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db_));
  for (const auto& a : uniq) {
    sqlite3_stmt* ms = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT OR IGNORE INTO group_members(group_id,"
                           " account, joined_ms) VALUES(?, ?, ?);",
                           -1, &ms, nullptr) != SQLITE_OK) {
      continue;
    }
    sqlite3_bind_int64(ms, 1, static_cast<sqlite3_int64>(gid));
    sqlite3_bind_text(ms, 2, a.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ms, 3, now_ms());
    sqlite3_step(ms);
    sqlite3_finalize(ms);
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return gid;
}

std::vector<std::string> ServerStore::group_members(std::uint64_t group_id) {
  std::vector<std::string> out;
  const char* sql =
      "SELECT account FROM group_members WHERE group_id = ?"
      " ORDER BY joined_ms ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    if (p) out.emplace_back(p);
  }
  sqlite3_finalize(st);
  return out;
}

// —— 平台-12 群能力开关（权限模型「群能力管理员配全」）——

const std::vector<std::string>& ServerStore::group_capability_names() {
  static const std::vector<std::string> names = {
      "notice",       // 群公告
      "memo",         // 群备忘录
      "vault",        // 群密码箱
      "tools",        // 群工具（R25：打包/CI）
      "server_tools", // 群服务器工具（R26）
      "files",        // 群文件
      "uplink",       // 外网上传收件箱
  };
  return names;
}

bool ServerStore::group_capability_set(std::uint64_t gid,
                                       const std::string& capability,
                                       bool enabled, const std::string& by,
                                       std::int64_t ts_ms) {
  if (!group_info(gid).has_value()) return false;
  bool known = false;
  for (const auto& n : group_capability_names())
    if (n == capability) {
      known = true;
      break;
    }
  if (!known) return false;
  const char* sql =
      "INSERT INTO group_capabilities(gid, capability, enabled, updated_by,"
      " updated_ms) VALUES(?,?,?,?,?) ON CONFLICT(gid, capability) DO UPDATE"
      " SET enabled = excluded.enabled, updated_by = excluded.updated_by,"
      " updated_ms = excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(gid));
  sqlite3_bind_text(st, 2, capability.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, enabled ? 1 : 0);
  sqlite3_bind_text(st, 4, by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (ok) {
    // 权限模型「全程留痕」：能力开关变更落查阅台账（谁/何时/改了什么）
    AuditReadRow rec;
    rec.op_account = by;
    rec.action = "group.capability";
    rec.filters = "gid=" + std::to_string(gid) + " capability=" + capability +
                  " enabled=" + (enabled ? "on" : "off");
    rec.result_count = 0;
    rec.ts_ms = ts_ms;
    add_audit_read(rec);
  }
  return ok;
}

bool ServerStore::group_capability_enabled(std::uint64_t gid,
                                           const std::string& capability) {
  bool known = false;
  for (const auto& n : group_capability_names())
    if (n == capability) {
      known = true;
      break;
    }
  if (!known) return false; // 非法能力名不因为「没配置」而放行
  const char* sql =
      "SELECT enabled FROM group_capabilities WHERE gid = ? AND"
      " capability = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return true;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(gid));
  sqlite3_bind_text(st, 2, capability.c_str(), -1, SQLITE_TRANSIENT);
  bool enabled = true; // 未配置=现行口径（允许）
  if (sqlite3_step(st) == SQLITE_ROW) enabled = sqlite3_column_int(st, 0) != 0;
  sqlite3_finalize(st);
  return enabled;
}

std::vector<ServerStore::GroupCapability> ServerStore::group_capabilities_list(
    std::uint64_t gid) {
  std::vector<GroupCapability> out;
  const char* sql =
      "SELECT gid, capability, enabled, updated_by, updated_ms"
      " FROM group_capabilities WHERE gid = ? ORDER BY capability ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(gid));
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupCapability c;
    c.gid = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    c.capability = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    c.enabled = sqlite3_column_int(st, 2) != 0;
    c.updated_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    c.updated_ms = sqlite3_column_int64(st, 4);
    out.push_back(std::move(c));
  }
  sqlite3_finalize(st);
  return out;
}

// —— R27-1 个人任务清单 ——

namespace {
ServerStore::TaskRow task_row_read(sqlite3_stmt* st) {
  ServerStore::TaskRow t;
  t.id = sqlite3_column_int64(st, 0);
  t.owner = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
  t.creator = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
  t.title = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
  t.note = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
  t.due_ms = sqlite3_column_int64(st, 5);
  t.reminded_ms = sqlite3_column_int64(st, 6);
  t.done = sqlite3_column_int(st, 7) != 0;
  t.done_ms = sqlite3_column_int64(st, 8);
  t.created_ms = sqlite3_column_int64(st, 9);
  t.provider = reinterpret_cast<const char*>(sqlite3_column_text(st, 10));
  t.ext_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 11));
  return t;
}
const char* kTaskCols =
    "id, owner, creator, title, note, due_ms, reminded_ms, done, done_ms,"
    " created_ms, provider, ext_key";
} // namespace

std::int64_t ServerStore::task_create(const std::string& owner,
                                      const std::string& creator,
                                      const std::string& title,
                                      const std::string& note,
                                      std::int64_t due_ms,
                                      std::int64_t created_ms,
                                      const std::string& provider,
                                      const std::string& ext_key) {
  if (owner.empty() || creator.empty() || title.empty()) return 0;
  // 外部任务引用成对（路由层把守口径，库层再兜一道）
  if (provider.empty() != ext_key.empty()) return 0;
  // 双方都须为已建账号（幽灵账号不给建）
  if (!find_account(owner) || !find_account(creator)) return 0;
  const char* sql =
      "INSERT INTO tasks(owner, creator, title, note, due_ms, created_ms,"
      " provider, ext_key) VALUES(?,?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, creator.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, note.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, due_ms);
  sqlite3_bind_int64(st, 6, created_ms);
  sqlite3_bind_text(st, 7, provider.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 8, ext_key.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok ? sqlite3_last_insert_rowid(db_) : 0;
}

std::vector<ServerStore::TaskRow> ServerStore::tasks_of(
    const std::string& owner) {
  std::vector<TaskRow> out;
  const std::string sql =
      std::string("SELECT ") + kTaskCols + " FROM tasks WHERE owner = ?"
      " ORDER BY id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) out.push_back(task_row_read(st));
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::TaskRow> ServerStore::tasks_assigned_by(
    const std::string& creator) {
  std::vector<TaskRow> out;
  const std::string sql =
      std::string("SELECT ") + kTaskCols + " FROM tasks WHERE creator = ?"
      " AND owner != ? ORDER BY id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  sqlite3_bind_text(st, 1, creator.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, creator.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) out.push_back(task_row_read(st));
  sqlite3_finalize(st);
  return out;
}

std::optional<ServerStore::TaskRow> ServerStore::task_by_id(std::int64_t id) {
  const std::string sql =
      std::string("SELECT ") + kTaskCols + " FROM tasks WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, id);
  std::optional<TaskRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) out = task_row_read(st);
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::task_set_done(std::int64_t id, bool done,
                                std::int64_t done_ms) {
  const char* sql =
      "UPDATE tasks SET done = ?, done_ms = ? WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(st, 1, done ? 1 : 0);
  sqlite3_bind_int64(st, 2, done ? done_ms : 0);
  sqlite3_bind_int64(st, 3, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::task_mark_reminded(std::int64_t id,
                                     std::int64_t reminded_ms) {
  // 只落一次：已有回执不回退（多端各提醒一次，时刻取最早）
  const char* sql =
      "UPDATE tasks SET reminded_ms = ? WHERE id = ? AND"
      " (reminded_ms = 0 OR reminded_ms > ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, reminded_ms);
  sqlite3_bind_int64(st, 2, id);
  sqlite3_bind_int64(st, 3, reminded_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::task_delete(std::int64_t id) {
  const char* sql = "DELETE FROM tasks WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::co_members(const std::string& a, const std::string& b) {
  if (a.empty() || a == b) return false;
  const char* sql =
      "SELECT 1 FROM group_members m1 JOIN group_members m2"
      " ON m1.group_id = m2.group_id"
      " WHERE m1.account = ? AND m2.account = ? LIMIT 1;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, a.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, b.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  return ok;
}

// —— 二期·审批（请假起步）——

namespace {
ServerStore::ApprovalRow approval_row_read(sqlite3_stmt* st) {
  ServerStore::ApprovalRow t;
  t.id = sqlite3_column_int64(st, 0);
  t.applicant = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
  t.type = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
  t.leave_from = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
  t.leave_to = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
  t.reason = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
  t.status = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
  t.decider = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
  t.decision_note = reinterpret_cast<const char*>(sqlite3_column_text(st, 8));
  t.created_ms = sqlite3_column_int64(st, 9);
  t.decided_ms = sqlite3_column_int64(st, 10);
  return t;
}
const char* kApprovalCols =
    "id, applicant, type, leave_from, leave_to, reason, status, decider,"
    " decision_note, created_ms, decided_ms";
} // namespace

std::int64_t ServerStore::approval_create(const std::string& applicant,
                                          const std::string& type,
                                          const std::string& leave_from,
                                          const std::string& leave_to,
                                          const std::string& reason,
                                          std::int64_t created_ms) {
  if (applicant.empty() || type.empty()) return 0;
  if (!find_account(applicant)) return 0; // 幽灵账号不给建
  const char* sql =
      "INSERT INTO approvals(applicant, type, leave_from, leave_to, reason,"
      " status, created_ms) VALUES(?,?,?,?,?,'pending',?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, applicant.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, leave_from.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, leave_to.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, reason.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 6, created_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok ? sqlite3_last_insert_rowid(db_) : 0;
}

std::vector<ServerStore::ApprovalRow> ServerStore::approvals_of(
    const std::string& applicant) {
  std::vector<ApprovalRow> out;
  const std::string sql =
      std::string("SELECT ") + kApprovalCols +
      " FROM approvals WHERE applicant = ? ORDER BY id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  sqlite3_bind_text(st, 1, applicant.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) out.push_back(approval_row_read(st));
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::ApprovalRow> ServerStore::approvals_pending() {
  std::vector<ApprovalRow> out;
  const std::string sql =
      std::string("SELECT ") + kApprovalCols +
      " FROM approvals WHERE status = 'pending' ORDER BY id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  while (sqlite3_step(st) == SQLITE_ROW) out.push_back(approval_row_read(st));
  sqlite3_finalize(st);
  return out;
}

std::optional<ServerStore::ApprovalRow> ServerStore::approval_by_id(
    std::int64_t id) {
  const std::string sql =
      std::string("SELECT ") + kApprovalCols +
      " FROM approvals WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, id);
  std::optional<ApprovalRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) out = approval_row_read(st);
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::approval_decide(std::int64_t id, const std::string& decider,
                                  bool approved, const std::string& note,
                                  std::int64_t decided_ms) {
  if (decider.empty()) return false;
  const char* sql =
      "UPDATE approvals SET status = ?, decider = ?, decision_note = ?,"
      " decided_ms = ? WHERE id = ? AND status = 'pending';";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, approved ? "approved" : "rejected", -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, decider.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, note.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, decided_ms);
  sqlite3_bind_int64(st, 5, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) == 1; // 非 pending=0 行改动=拒
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::approval_withdraw(std::int64_t id,
                                    const std::string& applicant) {
  const char* sql =
      "UPDATE approvals SET status = 'withdrawn', decided_ms = ?"
      " WHERE id = ? AND applicant = ? AND status = 'pending';";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, now_ms());
  sqlite3_bind_int64(st, 2, id);
  sqlite3_bind_text(st, 3, applicant.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) == 1; // 他人/非 pending=0 行=拒
  sqlite3_finalize(st);
  return ok;
}

// —— 二期·日报周报（设计稿 §二）——

std::int64_t ServerStore::report_upsert(const std::string& author,
                                        const std::string& report_date,
                                        const std::string& content,
                                        std::int64_t ts_ms) {
  if (author.empty() || report_date.empty()) return 0;
  if (!find_account(author)) return 0; // 幽灵账号不给写
  const char* sql =
      "INSERT INTO reports(author, report_date, content, created_ms,"
      " updated_ms) VALUES(?,?,?,?,?)"
      " ON CONFLICT(author, report_date) DO UPDATE SET"
      " content = excluded.content, updated_ms = excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, author.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, report_date.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, content.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, ts_ms);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok) return 0;
  // upsert 两种路径统一按 (author, date) 回查 id（不依赖 last_insert_rowid
  // 在 DO UPDATE 路径的语义）
  const char* q = "SELECT id FROM reports WHERE author = ? AND report_date = ?;";
  st = nullptr;
  std::int64_t id = 0;
  if (sqlite3_prepare_v2(db_, q, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, author.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, report_date.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
  }
  sqlite3_finalize(st);
  return id;
}

std::vector<ServerStore::ReportRow> ServerStore::reports_of(
    const std::string& author) {
  std::vector<ReportRow> out;
  const char* sql =
      "SELECT id, author, report_date, content, created_ms, updated_ms"
      " FROM reports WHERE author = ? ORDER BY report_date DESC, id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, author.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    ReportRow t;
    t.id = sqlite3_column_int64(st, 0);
    t.author = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    t.report_date = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    t.content = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    t.created_ms = sqlite3_column_int64(st, 4);
    t.updated_ms = sqlite3_column_int64(st, 5);
    out.push_back(std::move(t));
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<std::string> ServerStore::direct_reports(
    const std::string& manager) {
  std::vector<std::string> out;
  const char* sql =
      "SELECT DISTINCT account FROM org_reporting_lines"
      " WHERE manager_account = ? ORDER BY account;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, manager.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    out.emplace_back(
        reinterpret_cast<const char*>(sqlite3_column_text(st, 0)));
  }
  sqlite3_finalize(st);
  return out;
}

// —— 二期·办公室位置图（设计稿 docs/design/办公室位置图.md）——

std::int64_t ServerStore::seat_upsert(const std::string& floor,
                                      const std::string& label, double x,
                                      double y, std::int64_t ts_ms) {
  static_cast<void>(ts_ms); // 拖拽改位高频不高敏：不落操作留痕（设计口径）
  if (floor.empty() || label.empty()) return 0;
  const char* sql =
      "INSERT INTO office_seats(floor, label, x, y) VALUES(?,?,?,?)"
      " ON CONFLICT(floor, label) DO UPDATE SET x = excluded.x,"
      " y = excluded.y;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, floor.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, label.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(st, 3, x);
  sqlite3_bind_double(st, 4, y);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok) return 0;
  const char* q =
      "SELECT id FROM office_seats WHERE floor = ? AND label = ?;";
  st = nullptr;
  std::int64_t id = 0;
  if (sqlite3_prepare_v2(db_, q, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, floor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, label.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
  }
  sqlite3_finalize(st);
  return id;
}

bool ServerStore::seat_delete(std::int64_t id) {
  const char* sql = "DELETE FROM office_seats WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::seat_bind(std::int64_t id, const std::string& account,
                            std::int64_t ts_ms) {
  if (!account.empty() && !find_account(account)) return false; // 幽灵拒
  const char* sql = "UPDATE office_seats SET account = ? WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::SeatRow> ServerStore::seats_on_floor(
    const std::string& floor) {
  std::vector<SeatRow> out;
  const char* sql =
      "SELECT id, floor, label, x, y, account FROM office_seats"
      " WHERE floor = ? ORDER BY label;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, floor.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    SeatRow t;
    t.id = sqlite3_column_int64(st, 0);
    t.floor = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    t.label = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    t.x = sqlite3_column_double(st, 3);
    t.y = sqlite3_column_double(st, 4);
    t.account = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    out.push_back(std::move(t));
  }
  sqlite3_finalize(st);
  return out;
}

std::optional<ServerStore::SeatRow> ServerStore::seat_of(
    const std::string& account) {
  const char* sql =
      "SELECT id, floor, label, x, y, account FROM office_seats"
      " WHERE account = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<SeatRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    SeatRow t;
    t.id = sqlite3_column_int64(st, 0);
    t.floor = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    t.label = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    t.x = sqlite3_column_double(st, 3);
    t.y = sqlite3_column_double(st, 4);
    t.account = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    out = std::move(t);
  }
  sqlite3_finalize(st);
  return out;
}

std::optional<GroupInfo> ServerStore::group_info(std::uint64_t group_id) {
  const char* sql =
      "SELECT group_id, name, owner, announcement FROM groups"
      " WHERE group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  std::optional<GroupInfo> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    GroupInfo g;
    g.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    const auto text_of = [&st](int col) {
      const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
      return p ? std::string(p) : std::string{};
    };
    g.name = text_of(1);
    g.owner = text_of(2);
    g.announcement = text_of(3);
    g.members = group_members(group_id);
    out = std::move(g);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<GroupInfo> ServerStore::groups_list() {
  std::vector<GroupInfo> out;
  const char* sql =
      "SELECT group_id, name, owner, announcement FROM groups"
      " ORDER BY group_id;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupInfo g;
    g.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    g.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    g.owner = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    g.announcement = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    g.members = group_members(g.group_id); // 含群主
    out.push_back(std::move(g));
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<GroupInfo> ServerStore::groups_of(const std::string& account) {
  std::vector<GroupInfo> out;
  const char* sql =
      "SELECT group_id FROM group_members WHERE account = ?"
      " ORDER BY group_id ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  std::vector<std::uint64_t> ids;
  while (sqlite3_step(st) == SQLITE_ROW) {
    ids.push_back(static_cast<std::uint64_t>(sqlite3_column_int64(st, 0)));
  }
  sqlite3_finalize(st);
  for (const auto id : ids) {
    if (auto g = group_info(id)) out.push_back(std::move(*g));
  }
  return out;
}

bool ServerStore::is_group_member(std::uint64_t group_id,
                                  const std::string& account) {
  for (const auto& a : group_members(group_id)) {
    if (a == account) return true;
  }
  return false;
}

bool ServerStore::group_invite(std::uint64_t group_id,
                               const std::string& account) {
  if (!group_info(group_id).has_value()) return false;
  if (!find_account(account).has_value()) return false;
  if (is_group_member(group_id, account)) return false;
  const char* sql =
      "INSERT INTO group_members(group_id, account, joined_ms)"
      " VALUES(?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, now_ms());
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::group_leave(std::uint64_t group_id,
                              const std::string& account) {
  const auto info = group_info(group_id);
  if (!info.has_value() || !is_group_member(group_id, account)) return false;
  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "DELETE FROM group_members WHERE group_id = ?"
                         " AND account = ?;",
                         -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
    sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  if (info->owner == account) {
    // 群主退群＝解散：清完整张成员表（群号与历史归档保留，留痕纪律）
    if (sqlite3_prepare_v2(db_,
                           "DELETE FROM group_members WHERE group_id = ?;",
                           -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
      sqlite3_step(st);
      sqlite3_finalize(st);
    }
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return true;
}

bool ServerStore::group_announce(std::uint64_t group_id,
                                 const std::string& account,
                                 const std::string& announcement) {
  const auto info = group_info(group_id);
  if (!info.has_value()) return false;
  // R24-1 权限放宽：群主或管理员可设（设计「可写=群主/管理员」；
  // 群主身份在 groups.owner——role 列不含群主行）。
  if (info->owner != account && group_role(group_id, account) != "admin") {
    return false;
  }
  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  const char* sql = "UPDATE groups SET announcement = ? WHERE group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_bind_text(st, 1, announcement.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(group_id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  // 编辑历史留痕（只附加）：清除也落一笔（content 空串=清除）
  const char* log_sql =
      "INSERT INTO group_announcement_log(group_id, editor, content, ts_ms)"
      " VALUES(?, ?, ?, ?);";
  st = nullptr;
  if (sqlite3_prepare_v2(db_, log_sql, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
    sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, announcement.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return true;
}

std::vector<ServerStore::AnnouncementRevision>
ServerStore::announcement_history(std::uint64_t group_id, int limit) {
  std::vector<AnnouncementRevision> out;
  const char* sql =
      "SELECT id, editor, content, ts_ms FROM group_announcement_log"
      " WHERE group_id = ? ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_int(st, 2, limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    AnnouncementRevision r;
    r.id = sqlite3_column_int64(st, 0);
    const char* e = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* c = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.editor = e ? e : "";
    r.content = c ? c : "";
    r.ts_ms = sqlite3_column_int64(st, 3);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// —— T4.2 跨态会话日志 ——

// 只记时间/双方/时长，不含内容（上报帧本身也无内容字段）
bool ServerStore::cross_log_start(const std::string& account,
                                  const std::string& peer_device,
                                  const std::string& peer_name,
                                  std::int64_t started_ms) {
  if (account.empty() || peer_device.empty()) return false;
  const char* sql =
      "INSERT INTO cross_state_logs(account, peer_device, peer_name,"
      " started_ms) SELECT ?, ?, ?, ? WHERE NOT EXISTS"
      " (SELECT 1 FROM cross_state_logs WHERE account = ? AND peer_device = ?"
      "  AND started_ms = ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, peer_device.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, peer_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, started_ms);
  sqlite3_bind_text(st, 5, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, peer_device.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 7, started_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::cross_log_end(const std::string& account,
                                const std::string& peer_device,
                                std::int64_t started_ms,
                                std::int64_t ended_ms) {
  if (account.empty() || peer_device.empty() || ended_ms <= started_ms) {
    return false;
  }
  const char* sql =
      "UPDATE cross_state_logs SET ended_ms = ?, duration_ms = ?"
      " WHERE id = (SELECT id FROM cross_state_logs WHERE account = ?"
      "  AND peer_device = ? AND started_ms = ? AND ended_ms = 0"
      "  ORDER BY id ASC LIMIT 1);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, ended_ms);
  sqlite3_bind_int64(st, 2, ended_ms - started_ms);
  sqlite3_bind_text(st, 3, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, peer_device.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, started_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::vector<CrossLogRow> ServerStore::cross_logs(int limit) {
  std::vector<CrossLogRow> out;
  const char* sql =
      "SELECT id, account, peer_device, peer_name, started_ms, ended_ms,"
      " duration_ms FROM cross_state_logs ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int(st, 1, limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    CrossLogRow r;
    r.id = sqlite3_column_int64(st, 0);
    const auto text_of = [&st](int col) {
      const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
      return p ? std::string(p) : std::string{};
    };
    r.account = text_of(1);
    r.peer_device = text_of(2);
    r.peer_name = text_of(3);
    r.started_ms = sqlite3_column_int64(st, 4);
    r.ended_ms = sqlite3_column_int64(st, 5);
    r.duration_ms = sqlite3_column_int64(st, 6);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// —— T4.5 常用联系人 ——

bool ServerStore::fav_star(const std::string& account,
                           const std::string& peer, bool starred) {
  if (account.empty() || peer.empty()) return false;
  const char* sql =
      "INSERT INTO favorites(account, peer, starred, last_ms)"
      " VALUES(?, ?, ?, 0)"
      " ON CONFLICT(account, peer) DO UPDATE SET starred=excluded.starred;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, peer.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, starred ? 1 : 0);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::fav_touch(const std::string& account,
                            const std::string& peer, std::int64_t ts_ms) {
  if (account.empty() || peer.empty() || ts_ms <= 0) return false;
  const char* sql =
      "INSERT INTO favorites(account, peer, starred, last_ms)"
      " VALUES(?, ?, 0, ?)"
      " ON CONFLICT(account, peer) DO UPDATE SET"
      " last_ms=MAX(last_ms, excluded.last_ms);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, peer.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<FavRow> ServerStore::fav_list(const std::string& account) {
  std::vector<FavRow> out;
  const char* sql =
      "SELECT peer, starred, last_ms FROM favorites WHERE account = ?"
      " ORDER BY starred DESC, last_ms DESC, peer ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    FavRow r;
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    r.peer = p ? std::string(p) : std::string{};
    r.starred = sqlite3_column_int(st, 1) != 0;
    r.last_ms = sqlite3_column_int64(st, 2);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// 归档起点（A8）：首条归档消息（含其所在群消息）之前最近一次成功登录时刻。
// 跨态直连会话不进归档——故归档起点＝进入协作态的实际登录时间。
// 已读上报只给归档库存在的消息留痕（伪造 msg_id 灌库直接拒绝）；
// 同 (msg_id, 已读方) 重复上报幂等——首条为准，仍返回 true。
bool ServerStore::record_read(const std::string& msg_id,
                              const std::string& reader,
                              std::int64_t read_ms) {
  if (msg_id.empty() || reader.empty() || read_ms <= 0) return false;
  sqlite3_stmt* st = nullptr;
  const char* exists = "SELECT 1 FROM messages WHERE msg_id = ? LIMIT 1;";
  if (sqlite3_prepare_v2(db_, exists, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  const bool known = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  if (!known) return false;
  const char* sql =
      "INSERT OR IGNORE INTO message_reads(msg_id, reader, read_ms)"
      " VALUES(?, ?, ?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, reader.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, read_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ReadRow> ServerStore::readers_for(const std::string& msg_id) {
  std::vector<ReadRow> out;
  sqlite3_stmt* st = nullptr;
  const char* sql =
      "SELECT msg_id, reader, read_ms FROM message_reads WHERE msg_id = ?"
      " ORDER BY read_ms ASC;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    ReadRow r;
    r.msg_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    r.reader = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.read_ms = sqlite3_column_int64(st, 2);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

std::int64_t ServerStore::archive_start_ms(const std::string& account) {
  if (account.empty()) return 0;
  std::int64_t first_msg = 0;
  {
    const char* sql =
        "SELECT MIN(ts_ms) FROM messages WHERE from_account = ?"
        " OR to_account = ?"
        " OR to_account IN (SELECT 'group:' || group_id FROM group_members"
        "  WHERE account = ?);";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, account.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW &&
        sqlite3_column_type(st, 0) != SQLITE_NULL) {
      first_msg = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
  }
  if (first_msg == 0) return 0; // 无归档
  std::int64_t login_ms = 0;
  {
    const char* sql =
        "SELECT MAX(ts_ms) FROM login_records WHERE account = ?"
        " AND result = 'ok' AND ts_ms <= ?;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      return first_msg;
    }
    sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, first_msg);
    if (sqlite3_step(st) == SQLITE_ROW &&
        sqlite3_column_type(st, 0) != SQLITE_NULL) {
      login_ms = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
  }
  return login_ms > 0 ? login_ms : first_msg;
}

std::int64_t ServerStore::webhook_create(const std::string& token_hash,
                                          const std::string& target,
                                          const std::string& name,
                                          std::int64_t created_ms) {
  if (token_hash.empty() || target.empty()) return 0;
  const char* sql =
      "INSERT INTO webhooks(token_hash, target, name, created_ms)"
      " VALUES(?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, token_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, target.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, created_ms);
  std::int64_t id = 0;
  if (sqlite3_step(st) == SQLITE_DONE) id = sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(st);
  return id;
}

std::optional<WebhookRow> ServerStore::webhook_by_token(
    const std::string& token_hash) {
  if (token_hash.empty()) return std::nullopt;
  const char* sql =
      "SELECT id, token_hash, target, name, created_ms, revoked"
      " FROM webhooks WHERE token_hash=? AND revoked=0;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, token_hash.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<WebhookRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    WebhookRow r;
    r.id = sqlite3_column_int64(st, 0);
    r.token_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.target = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    if (sqlite3_column_type(st, 3) != SQLITE_NULL) {
      r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    }
    r.created_ms = sqlite3_column_int64(st, 4);
    r.revoked = sqlite3_column_int(st, 5) != 0;
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<WebhookRow> ServerStore::webhook_list() {
  std::vector<WebhookRow> out;
  const char* sql =
      "SELECT id, token_hash, target, name, created_ms, revoked"
      " FROM webhooks ORDER BY id;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    WebhookRow r;
    r.id = sqlite3_column_int64(st, 0);
    r.token_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.target = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    if (sqlite3_column_type(st, 3) != SQLITE_NULL) {
      r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    }
    r.created_ms = sqlite3_column_int64(st, 4);
    r.revoked = sqlite3_column_int(st, 5) != 0;
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::webhook_revoke(std::int64_t id) {
  const char* sql = "UPDATE webhooks SET revoked=1 WHERE id=? AND revoked=0;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

// —— 机器人（bot）台账 ——

std::int64_t ServerStore::bot_add(const std::string& name,
                                  const std::string& token_hash,
                                  const std::string& created_by,
                                  std::int64_t created_ms) {
  if (name.empty() || name.find(':') != std::string::npos ||
      token_hash.empty()) {
    return 0;
  }
  const char* sql =
      "INSERT INTO bots(name, token_hash, created_by, created_ms)"
      " VALUES(?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, token_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, created_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, created_ms);
  std::int64_t id = 0;
  if (sqlite3_step(st) == SQLITE_DONE) id = sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(st);
  return id;
}

std::optional<BotRow> ServerStore::bot_by_token(
    const std::string& token_hash) {
  if (token_hash.empty()) return std::nullopt;
  const char* sql =
      "SELECT name, token_hash, created_by, created_ms, disabled"
      " FROM bots WHERE token_hash=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, token_hash.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<BotRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    BotRow r;
    r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    r.token_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.created_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.created_ms = sqlite3_column_int64(st, 3);
    r.disabled = sqlite3_column_int(st, 4) != 0;
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<BotRow> ServerStore::bot_list() {
  std::vector<BotRow> out;
  const char* sql =
      "SELECT name, token_hash, created_by, created_ms, disabled"
      " FROM bots ORDER BY created_ms, name;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    BotRow r;
    r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    r.token_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.created_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.created_ms = sqlite3_column_int64(st, 3);
    r.disabled = sqlite3_column_int(st, 4) != 0;
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

std::optional<BotRow> ServerStore::bot_by_name(const std::string& name) {
  if (name.empty()) return std::nullopt;
  const char* sql =
      "SELECT name, token_hash, created_by, created_ms, disabled"
      " FROM bots WHERE name=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<BotRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    BotRow r;
    r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    r.token_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.created_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.created_ms = sqlite3_column_int64(st, 3);
    r.disabled = sqlite3_column_int(st, 4) != 0;
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::bot_remove(const std::string& name) {
  // 连带清全部群成员行（伪账号退群由本方法一并兜底）。
  const char* del_member =
      "DELETE FROM group_members WHERE account=?;";
  const char* del_bot = "DELETE FROM bots WHERE name=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, del_member, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, ("bot:" + name).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
  if (sqlite3_prepare_v2(db_, del_bot, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::bot_set_disabled(const std::string& name, bool disabled) {
  const char* sql = "UPDATE bots SET disabled=? WHERE name=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(st, 1, disabled ? 1 : 0);
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::bot_join_group(const std::string& name,
                                 std::uint64_t group_id,
                                 std::int64_t joined_ms) {
  if (!bot_by_name(name) || !group_info(group_id)) return false;
  const char* sql =
      "INSERT OR IGNORE INTO group_members(group_id, account, joined_ms)"
      " VALUES(?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, ("bot:" + name).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, joined_ms);
  sqlite3_step(st);
  sqlite3_finalize(st);
  return true;
}

bool ServerStore::bot_leave_group(const std::string& name,
                                  std::uint64_t group_id) {
  if (!bot_by_name(name)) return false;
  const char* sql =
      "DELETE FROM group_members WHERE group_id=? AND account=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, ("bot:" + name).c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

// —— 模型网关：端点登记＋调用审计 ——

std::int64_t ServerStore::model_endpoint_add(const ModelEndpointRow& ep) {
  if (ep.name.empty() || ep.name.find(':') != std::string::npos ||
      ep.base_url.empty()) {
    return 0;
  }
  const char* sql =
      "INSERT INTO model_endpoints(name, base_url, api_key, model, is_local,"
      " enabled, created_by, created_ms) VALUES(?,?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, ep.name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, ep.base_url.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, ep.api_key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, ep.model.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 5, ep.is_local ? 1 : 0);
  sqlite3_bind_int(st, 6, ep.enabled ? 1 : 0);
  sqlite3_bind_text(st, 7, ep.created_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 8, ep.created_ms);
  std::int64_t id = 0;
  if (sqlite3_step(st) == SQLITE_DONE) id = sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(st);
  return id;
}

ModelEndpointRow model_endpoint_row_read(sqlite3_stmt* st) {
  ModelEndpointRow r;
  r.id = sqlite3_column_int64(st, 0);
  r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
  r.base_url = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
  if (sqlite3_column_type(st, 3) != SQLITE_NULL) {
    r.api_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
  }
  if (sqlite3_column_type(st, 4) != SQLITE_NULL) {
    r.model = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
  }
  r.is_local = sqlite3_column_int(st, 5) != 0;
  r.enabled = sqlite3_column_int(st, 6) != 0;
  r.created_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
  r.created_ms = sqlite3_column_int64(st, 8);
  return r;
}

// 列清单（与 model_endpoint_row_read 读序对齐；两处相邻防漂移）
constexpr const char* kModelEndpointCols =
    "id, name, base_url, api_key, model, is_local, enabled, created_by,"
    " created_ms";

std::optional<ModelEndpointRow> ServerStore::model_endpoint_by_name(
    const std::string& name) {
  if (name.empty()) return std::nullopt;
  const std::string sql =
      std::string("SELECT ") + kModelEndpointCols +
      " FROM model_endpoints WHERE name=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<ModelEndpointRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) out = model_endpoint_row_read(st);
  sqlite3_finalize(st);
  return out;
}

std::vector<ModelEndpointRow> ServerStore::model_endpoints_list() {
  std::vector<ModelEndpointRow> out;
  const std::string sql = std::string("SELECT ") + kModelEndpointCols +
                          " FROM model_endpoints ORDER BY id;"; // 注册序
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  while (sqlite3_step(st) == SQLITE_ROW) out.push_back(model_endpoint_row_read(st));
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::model_endpoint_remove(const std::string& name) {
  const char* sql = "DELETE FROM model_endpoints WHERE name=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::model_endpoint_set_enabled(const std::string& name,
                                             bool enabled) {
  const char* sql = "UPDATE model_endpoints SET enabled=? WHERE name=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(st, 1, enabled ? 1 : 0);
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::model_endpoint_set_local(const std::string& name,
                                           bool is_local) {
  const char* sql = "UPDATE model_endpoints SET is_local=? WHERE name=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(st, 1, is_local ? 1 : 0);
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::int64_t ServerStore::model_call_add(const ModelCallRow& call) {
  const char* sql =
      "INSERT INTO model_calls(caller, endpoint, model, archive_scope,"
      " prompt_chars, completion_chars, status, latency_ms, created_ms)"
      " VALUES(?,?,?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, call.caller.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, call.endpoint.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, call.model.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 4, call.archive_scope ? 1 : 0);
  sqlite3_bind_int(st, 5, call.prompt_chars);
  sqlite3_bind_int(st, 6, call.completion_chars);
  sqlite3_bind_int(st, 7, call.status);
  sqlite3_bind_int64(st, 8, call.latency_ms);
  sqlite3_bind_int64(st, 9, call.created_ms);
  std::int64_t id = 0;
  if (sqlite3_step(st) == SQLITE_DONE) id = sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(st);
  return id;
}

std::vector<ModelCallRow> ServerStore::model_calls_list(int limit) {
  std::vector<ModelCallRow> out;
  const char* sql =
      "SELECT id, caller, endpoint, model, archive_scope, prompt_chars,"
      " completion_chars, status, latency_ms, created_ms"
      " FROM model_calls ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int(st, 1, limit < 1 ? 50 : (limit > 1000 ? 1000 : limit));
  while (sqlite3_step(st) == SQLITE_ROW) {
    ModelCallRow r;
    r.id = sqlite3_column_int64(st, 0);
    r.caller = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.endpoint = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.model = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.archive_scope = sqlite3_column_int(st, 4) != 0;
    r.prompt_chars = sqlite3_column_int(st, 5);
    r.completion_chars = sqlite3_column_int(st, 6);
    r.status = sqlite3_column_int(st, 7);
    r.latency_ms = sqlite3_column_int64(st, 8);
    r.created_ms = sqlite3_column_int64(st, 9);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// —— R23-1 文件存储元数据（服务端权限/配额判断层）——
// 秒传键 UNIQUE(file_hash, owner)：同属主同哈希复用对象键，不重复落字节。

std::int64_t ServerStore::create_file_meta(const FileMeta& meta) {
  if (meta.owner.empty() || meta.file_name.empty()) return 0;
  const char* sql =
      "INSERT OR IGNORE INTO files(owner, belong_gid, belong_uid, file_name,"
      " file_size, file_hash, object_key, source, upload_ts, status, kind)"
      " VALUES(?,?,?,?,?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, meta.owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, meta.belong_gid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, meta.belong_uid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, meta.file_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, meta.file_size);
  sqlite3_bind_text(st, 6, meta.file_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 7, meta.object_key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 8, static_cast<int>(meta.source));
  sqlite3_bind_int64(st, 9, meta.upload_ts);
  sqlite3_bind_int(st, 10, static_cast<int>(meta.status));
  sqlite3_bind_int(st, 11, static_cast<int>(meta.kind));
  std::int64_t id = 0;
  if (sqlite3_step(st) == SQLITE_DONE) {
    if (sqlite3_changes(db_) > 0) {
      id = sqlite3_last_insert_rowid(db_);
    } else {
      // 秒传键冲突（UNIQUE(file_hash, owner, belong_gid, belong_uid, kind)
      // 已有行）：last_insert_rowid 会返回连接上上一次无关插入的 rowid，
      // 必须回查已有行的 id 返回。
      sqlite3_finalize(st);
      const char* lookup =
          "SELECT id FROM files WHERE file_hash=? AND owner=?"
          " AND belong_gid=? AND belong_uid=? AND kind=?;";
      if (sqlite3_prepare_v2(db_, lookup, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, meta.file_hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, meta.owner.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, meta.belong_gid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, meta.belong_uid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 5, static_cast<int>(meta.kind));
        if (sqlite3_step(st) == SQLITE_ROW) {
          id = sqlite3_column_int64(st, 0);
        }
      }
    }
  }
  sqlite3_finalize(st);
  return id;
}

std::optional<ServerStore::FileMeta> ServerStore::check_second_transfer(
    const std::string& owner, const std::string& file_hash,
    const std::string& belong_gid, const std::string& belong_uid,
    FileKind kind) {
  if (owner.empty() || file_hash.empty()) return std::nullopt;
  // 秒传按「属主+哈希+归属+类目」判：同属主同哈希在另一群/个人空间/
  // 收件箱不算命中（对象前缀本就按归属隔离，见设计铁律 3）。
  const char* sql =
      "SELECT id, owner, belong_gid, belong_uid, file_name, file_size,"
      " file_hash, object_key, source, upload_ts, status, pin, kind"
      " FROM files WHERE owner=? AND file_hash=? AND status=0"
      " AND belong_gid=? AND belong_uid=? AND kind=? LIMIT 1;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, file_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, belong_gid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, belong_uid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 5, static_cast<int>(kind));
  std::optional<FileMeta> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    FileMeta r;
    r.id = sqlite3_column_int64(st, 0);
    r.owner = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.belong_gid = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.belong_uid = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.file_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.file_size = sqlite3_column_int64(st, 5);
    r.file_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.object_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
    r.source = static_cast<FileSource>(sqlite3_column_int(st, 8));
    r.upload_ts = sqlite3_column_int64(st, 9);
    r.status = static_cast<FileStatus>(sqlite3_column_int(st, 10));
    r.pin = sqlite3_column_int(st, 11) != 0;
    r.kind = static_cast<FileKind>(sqlite3_column_int(st, 12));
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::FileMeta> ServerStore::list_files(
    const std::string& belong_gid, const std::string& belong_uid, int limit,
    int offset, int kind) {
  std::vector<FileMeta> out;
  // kind<0 不过滤类目（全量）；0/1 只列对应类目（R23-3 空间隔离）
  const char* sql =
      "SELECT id, owner, belong_gid, belong_uid, file_name, file_size,"
      " file_hash, object_key, source, upload_ts, status, pin, kind"
      " FROM files"
      " WHERE (? = '' OR belong_gid = ?) AND (? = '' OR belong_uid = ?)"
      " AND (? < 0 OR kind = ?)"
      " ORDER BY pin DESC, id DESC LIMIT ? OFFSET ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, belong_gid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, belong_gid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, belong_uid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, belong_uid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 5, kind);
  sqlite3_bind_int(st, 6, kind);
  sqlite3_bind_int(st, 7, limit);
  sqlite3_bind_int(st, 8, offset);
  while (sqlite3_step(st) == SQLITE_ROW) {
    FileMeta r;
    r.id = sqlite3_column_int64(st, 0);
    r.owner = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.belong_gid = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.belong_uid = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.file_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.file_size = sqlite3_column_int64(st, 5);
    r.file_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.object_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
    r.source = static_cast<FileSource>(sqlite3_column_int(st, 8));
    r.upload_ts = sqlite3_column_int64(st, 9);
    r.status = static_cast<FileStatus>(sqlite3_column_int(st, 10));
    r.pin = sqlite3_column_int(st, 11) != 0;
    r.kind = static_cast<FileKind>(sqlite3_column_int(st, 12));
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::delete_file_meta(std::int64_t file_id) {
  const char* sql = "DELETE FROM files WHERE id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, file_id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

namespace {
// 配额行读取（两级：群/人；无行=0 用量、0 上限=不限）。
ServerStore::QuotaInfo read_quota(sqlite3* db, const char* sql,
                                  const std::string& key) {
  ServerStore::QuotaInfo q;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return q;
  sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) {
    q.gid = key;
    q.uid = key;
    q.used_bytes = sqlite3_column_int64(st, 1);
    q.limit_bytes = sqlite3_column_int64(st, 2);
  } else {
    q.gid = key;
    q.uid = key;
  }
  sqlite3_finalize(st);
  return q;
}
bool add_quota_used(sqlite3* db, const char* table, const char* key_col,
                    const std::string& key, std::int64_t delta) {
  std::string upsert = std::string("INSERT INTO ") + table + "(" + key_col +
                       ", used_bytes, limit_bytes) VALUES(?,?,0)"
                       " ON CONFLICT(" +
                       key_col +
                       ") DO UPDATE SET used_bytes=used_bytes+excluded.used_bytes;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, upsert.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, delta);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}
bool set_quota_limit(sqlite3* db, const char* table, const char* key_col,
                     const std::string& key, std::int64_t limit) {
  std::string upsert = std::string("INSERT INTO ") + table + "(" + key_col +
                       ", used_bytes, limit_bytes) VALUES(?,0,?)"
                       " ON CONFLICT(" +
                       key_col + ") DO UPDATE SET limit_bytes=excluded.limit_bytes;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, upsert.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, limit);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}
// 受检扣费（原子，单语句判限）：无行则建（limit=0=不限）；有行时仅当
// limit=0 或 used+delta≤limit 才更新。超限/负 delta 一律 changes=0。
bool charge_quota(sqlite3* db, const char* table, const char* key_col,
                  const std::string& key, std::int64_t delta) {
  std::string upsert =
      std::string("INSERT INTO ") + table + "(" + key_col +
      ", used_bytes, limit_bytes) VALUES(?,?,0)"
      " ON CONFLICT(" +
      key_col +
      ") DO UPDATE SET used_bytes=used_bytes+excluded.used_bytes"
      " WHERE " +
      table + ".limit_bytes=0"
              " OR " +
      table + ".used_bytes+excluded.used_bytes<=" + table + ".limit_bytes;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, upsert.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, delta);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) > 0;
  sqlite3_finalize(st);
  return ok;
}
} // namespace

std::optional<ServerStore::QuotaInfo> ServerStore::get_group_quota(
    const std::string& gid) {
  if (gid.empty()) return std::nullopt;
  return read_quota(db_,
                    "SELECT gid, used_bytes, limit_bytes FROM group_quota"
                    " WHERE gid=?;",
                    gid);
}

std::optional<ServerStore::QuotaInfo> ServerStore::get_user_quota(
    const std::string& uid) {
  if (uid.empty()) return std::nullopt;
  return read_quota(db_,
                    "SELECT uid, used_bytes, limit_bytes FROM user_quota"
                    " WHERE uid=?;",
                    uid);
}

bool ServerStore::add_group_quota_used(const std::string& gid,
                                       std::int64_t delta_bytes) {
  if (gid.empty()) return false;
  return add_quota_used(db_, "group_quota", "gid", gid, delta_bytes);
}

bool ServerStore::add_user_quota_used(const std::string& uid,
                                      std::int64_t delta_bytes) {
  if (uid.empty()) return false;
  return add_quota_used(db_, "user_quota", "uid", uid, delta_bytes);
}

bool ServerStore::set_group_quota_limit(const std::string& gid,
                                        std::int64_t limit_bytes) {
  if (gid.empty() || limit_bytes < 0) return false;
  return set_quota_limit(db_, "group_quota", "gid", gid, limit_bytes);
}

bool ServerStore::set_user_quota_limit(const std::string& uid,
                                       std::int64_t limit_bytes) {
  if (uid.empty() || limit_bytes < 0) return false;
  return set_quota_limit(db_, "user_quota", "uid", uid, limit_bytes);
}

bool ServerStore::add_uplink_log(const UplinkLog& log) {
  if (log.uploader.empty() || log.file_name.empty()) return false;
  const char* sql =
      "INSERT INTO uplink_logs(uploader, file_name, file_size, file_hash,"
      " object_key, upload_ts) VALUES(?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, log.uploader.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, log.file_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, log.file_size);
  sqlite3_bind_text(st, 4, log.file_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, log.object_key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 6, log.upload_ts);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::UplinkLog> ServerStore::list_uplink_logs(
    const std::string& uploader, int limit) {
  std::vector<UplinkLog> out;
  const char* sql =
      "SELECT id, uploader, file_name, file_size, file_hash, object_key,"
      " upload_ts FROM uplink_logs"
      " WHERE (? = '' OR uploader = ?)"
      " ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, uploader.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, uploader.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    UplinkLog r;
    r.id = sqlite3_column_int64(st, 0);
    r.uploader = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.file_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.file_size = sqlite3_column_int64(st, 3);
    r.file_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.object_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    r.upload_ts = sqlite3_column_int64(st, 6);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::FileMeta> ServerStore::list_uplink_files(
    const std::string& owner, int limit, int offset) {
  std::vector<FileMeta> out;
  // 只列本人 uplink 来源的收件箱文件（source/kind 双过滤：内网收件箱
  // 文件不出现在外网面；他人记录同理不可见）
  const char* sql =
      "SELECT id, owner, belong_gid, belong_uid, file_name, file_size,"
      " file_hash, object_key, source, upload_ts, status, pin, kind"
      " FROM files"
      " WHERE owner = ? AND source = 1 AND kind = 1"
      " ORDER BY id DESC LIMIT ? OFFSET ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, limit);
  sqlite3_bind_int(st, 3, offset);
  while (sqlite3_step(st) == SQLITE_ROW) {
    FileMeta r;
    r.id = sqlite3_column_int64(st, 0);
    r.owner = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.belong_gid = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.belong_uid = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.file_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.file_size = sqlite3_column_int64(st, 5);
    r.file_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.object_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
    r.source = static_cast<FileSource>(sqlite3_column_int(st, 8));
    r.upload_ts = sqlite3_column_int64(st, 9);
    r.status = static_cast<FileStatus>(sqlite3_column_int(st, 10));
    r.pin = sqlite3_column_int(st, 11) != 0;
    r.kind = static_cast<FileKind>(sqlite3_column_int(st, 12));
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// —— R23-2 文件管理面：按 id 取/置顶/状态、对象引用计数、受检配额扣费、
//    群内角色（权限模型：群主/管理员/成员）——

std::optional<ServerStore::FileMeta> ServerStore::file_by_id(
    std::int64_t file_id) {
  const char* sql =
      "SELECT id, owner, belong_gid, belong_uid, file_name, file_size,"
      " file_hash, object_key, source, upload_ts, status, pin, kind"
      " FROM files WHERE id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, file_id);
  std::optional<FileMeta> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    FileMeta r;
    r.id = sqlite3_column_int64(st, 0);
    r.owner = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.belong_gid = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.belong_uid = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.file_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.file_size = sqlite3_column_int64(st, 5);
    r.file_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.object_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
    r.source = static_cast<FileSource>(sqlite3_column_int(st, 8));
    r.upload_ts = sqlite3_column_int64(st, 9);
    r.status = static_cast<FileStatus>(sqlite3_column_int(st, 10));
    r.pin = sqlite3_column_int(st, 11) != 0;
    r.kind = static_cast<FileKind>(sqlite3_column_int(st, 12));
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::set_file_pin(std::int64_t file_id, bool pin) {
  const char* sql = "UPDATE files SET pin=? WHERE id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(st, 1, pin ? 1 : 0);
  sqlite3_bind_int64(st, 2, file_id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::set_file_status(std::int64_t file_id, FileStatus status) {
  const char* sql = "UPDATE files SET status=? WHERE id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(st, 1, static_cast<int>(status));
  sqlite3_bind_int64(st, 2, file_id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::int64_t ServerStore::count_file_refs(const std::string& object_key) {
  if (object_key.empty()) return 0;
  const char* sql = "SELECT COUNT(*) FROM files WHERE object_key=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, object_key.c_str(), -1, SQLITE_TRANSIENT);
  std::int64_t n = 0;
  if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return n;
}

// ---- R23-3 文件助手：备忘录（本人收件箱文本面） ----
// 自备忘录不留修订史（修订史属 R24-2）；owner 过滤即权限边界，server 层再判权。

std::int64_t ServerStore::create_memo(const std::string& owner,
                                      const std::string& content,
                                      std::int64_t ts_ms) {
  if (owner.empty() || content.empty()) return 0;
  const char* sql =
      "INSERT INTO memos (owner, content, created_ms, updated_ms)"
      " VALUES (?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, content.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, ts_ms);
  sqlite3_bind_int64(st, 4, ts_ms);
  std::int64_t id = 0;
  if (sqlite3_step(st) == SQLITE_DONE) id = sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(st);
  return id;
}

std::optional<ServerStore::MemoRow> ServerStore::memo_by_id(std::int64_t id) {
  const char* sql =
      "SELECT id, owner, content, created_ms, updated_ms"
      " FROM memos WHERE id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, id);
  std::optional<MemoRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    MemoRow r;
    r.id = sqlite3_column_int64(st, 0);
    r.owner = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.content = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.created_ms = sqlite3_column_int64(st, 3);
    r.updated_ms = sqlite3_column_int64(st, 4);
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::MemoRow> ServerStore::list_memos(
    const std::string& owner, int limit, int offset) {
  std::vector<MemoRow> out;
  if (owner.empty()) return out;
  const char* sql =
      "SELECT id, owner, content, created_ms, updated_ms"
      " FROM memos WHERE owner=?"
      " ORDER BY updated_ms DESC, id DESC LIMIT ? OFFSET ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, limit);
  sqlite3_bind_int(st, 3, offset);
  while (sqlite3_step(st) == SQLITE_ROW) {
    MemoRow r;
    r.id = sqlite3_column_int64(st, 0);
    r.owner = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.content = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.created_ms = sqlite3_column_int64(st, 3);
    r.updated_ms = sqlite3_column_int64(st, 4);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::update_memo(std::int64_t id, const std::string& owner,
                              const std::string& content,
                              std::int64_t ts_ms) {
  if (owner.empty() || content.empty()) return false;
  // WHERE owner= 二次校验：仅本人可改自己的行。
  const char* sql =
      "UPDATE memos SET content=?, updated_ms=? WHERE id=? AND owner=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, content.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, ts_ms);
  sqlite3_bind_int64(st, 3, id);
  sqlite3_bind_text(st, 4, owner.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::delete_memo(std::int64_t id, const std::string& owner) {
  if (owner.empty()) return false;
  const char* sql = "DELETE FROM memos WHERE id=? AND owner=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, id);
  sqlite3_bind_text(st, 2, owner.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::charge_group_quota(const std::string& gid,
                                     std::int64_t delta_bytes) {
  if (gid.empty() || delta_bytes < 0) return false;
  // 原子受检扣费：limit=0=不限；超限整条不生效（changes=0 → 上传拒绝）。
  return charge_quota(db_, "group_quota", "gid", gid, delta_bytes);
}

bool ServerStore::charge_user_quota(const std::string& uid,
                                    std::int64_t delta_bytes) {
  if (uid.empty() || delta_bytes < 0) return false;
  return charge_quota(db_, "user_quota", "uid", uid, delta_bytes);
}

std::string ServerStore::group_role(std::uint64_t group_id,
                                    const std::string& account) {
  if (account.empty()) return "";
  const char* sql =
      "SELECT role FROM group_members WHERE group_id=? AND account=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return "";
  sqlite3_bind_int64(st, 1, static_cast<std::int64_t>(group_id));
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  std::string role;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const char* r = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    if (r) role = r;
  }
  sqlite3_finalize(st);
  return role;
}

bool ServerStore::group_set_role(std::uint64_t group_id,
                                 const std::string& account,
                                 const std::string& role) {
  if (account.empty()) return false;
  if (role != "member" && role != "admin") return false;
  // 群主行拒改（owner 身份在 groups.owner，不在本列——任免/转让另走群面）
  const char* sql =
      "UPDATE group_members SET role=? WHERE group_id=? AND account=?"
      " AND account != (SELECT owner FROM groups WHERE group_id=?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, role.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, static_cast<std::int64_t>(group_id));
  sqlite3_bind_text(st, 3, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, static_cast<std::int64_t>(group_id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

// —— R24-2 群备忘录 ——

std::int64_t ServerStore::create_group_memo(std::uint64_t group_id,
                                            const std::string& title,
                                            const std::string& content,
                                            const std::string& author,
                                            std::int64_t ts_ms) {
  if (group_id == 0 || title.empty() || content.empty() || author.empty()) {
    return 0;
  }
  if (!group_info(group_id).has_value()) return 0;
  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  const char* sql =
      "INSERT INTO group_memos(group_id, title, content, author, created_ms,"
      " updated_ms) VALUES(?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return 0;
  }
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, content.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, author.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  sqlite3_bind_int64(st, 6, ts_ms);
  if (sqlite3_step(st) != SQLITE_DONE) {
    sqlite3_finalize(st);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return 0;
  }
  sqlite3_finalize(st);
  const std::int64_t id = sqlite3_last_insert_rowid(db_);
  // 首建即落首笔修订（editor=作者）——历史从此完整
  const char* rev_sql =
      "INSERT INTO group_memo_revisions(memo_id, title, content, editor,"
      " ts_ms) VALUES(?, ?, ?, ?, ?);";
  st = nullptr;
  if (sqlite3_prepare_v2(db_, rev_sql, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, author.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, ts_ms);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return id;
}

std::optional<ServerStore::GroupMemo> ServerStore::group_memo_by_id(
    std::int64_t id) {
  const char* sql =
      "SELECT id, group_id, title, content, author, created_ms, updated_ms"
      " FROM group_memos WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, id);
  std::optional<GroupMemo> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    GroupMemo m;
    m.id = sqlite3_column_int64(st, 0);
    m.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* c = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    m.title = t ? t : "";
    m.content = c ? c : "";
    m.author = a ? a : "";
    m.created_ms = sqlite3_column_int64(st, 5);
    m.updated_ms = sqlite3_column_int64(st, 6);
    out = std::move(m);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::GroupMemo> ServerStore::list_group_memos(
    std::uint64_t group_id, const std::string& keyword, int limit,
    int offset) {
  std::vector<GroupMemo> out;
  std::string sql =
      "SELECT id, group_id, title, content, author, created_ms, updated_ms"
      " FROM group_memos WHERE group_id = ?";
  std::string like;
  if (!keyword.empty()) {
    for (const char c : keyword) {
      if (c == '%' || c == '_' || c == '\\') like += '\\';
      like += c;
    }
    sql += " AND (title LIKE ? ESCAPE '\\' OR content LIKE ? ESCAPE '\\')";
  }
  sql += " ORDER BY updated_ms DESC LIMIT ? OFFSET ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  int idx = 1;
  sqlite3_bind_int64(st, idx++, static_cast<sqlite3_int64>(group_id));
  if (!like.empty()) {
    const std::string pat = "%" + like + "%";
    sqlite3_bind_text(st, idx++, pat.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, idx++, pat.c_str(), -1, SQLITE_TRANSIENT);
  }
  sqlite3_bind_int(st, idx++, limit);
  sqlite3_bind_int(st, idx, offset);
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupMemo m;
    m.id = sqlite3_column_int64(st, 0);
    m.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* c = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    m.title = t ? t : "";
    m.content = c ? c : "";
    m.author = a ? a : "";
    m.created_ms = sqlite3_column_int64(st, 5);
    m.updated_ms = sqlite3_column_int64(st, 6);
    out.push_back(std::move(m));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::update_group_memo(std::int64_t id, const std::string& title,
                                    const std::string& content,
                                    const std::string& editor,
                                    std::int64_t ts_ms) {
  if (title.empty() || content.empty() || editor.empty()) return false;
  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  const char* sql =
      "UPDATE group_memos SET title = ?, content = ?, updated_ms = ?"
      " WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_bind_text(st, 1, title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, content.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, ts_ms);
  sqlite3_bind_int64(st, 4, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  if (!ok) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  // 修订笔与条目更新同事务——留痕不落半截
  const char* rev_sql =
      "INSERT INTO group_memo_revisions(memo_id, title, content, editor,"
      " ts_ms) VALUES(?, ?, ?, ?, ?);";
  st = nullptr;
  if (sqlite3_prepare_v2(db_, rev_sql, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, editor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, ts_ms);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return true;
}

bool ServerStore::delete_group_memo(std::int64_t id, std::uint64_t group_id) {
  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  const char* sql =
      "DELETE FROM group_memos WHERE id = ? AND group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_bind_int64(st, 1, id);
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(group_id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  if (!ok) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  // 条目删除连带修订史（不留孤儿；历史属条目的一部分）
  const char* rev_sql = "DELETE FROM group_memo_revisions WHERE memo_id = ?;";
  st = nullptr;
  if (sqlite3_prepare_v2(db_, rev_sql, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return true;
}

std::vector<ServerStore::GroupMemoRevision> ServerStore::group_memo_history(
    std::int64_t memo_id) {
  std::vector<GroupMemoRevision> out;
  const char* sql =
      "SELECT id, memo_id, title, content, editor, ts_ms"
      " FROM group_memo_revisions WHERE memo_id = ? ORDER BY id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, memo_id);
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupMemoRevision r;
    r.id = sqlite3_column_int64(st, 0);
    r.memo_id = sqlite3_column_int64(st, 1);
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* c = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* e = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.title = t ? t : "";
    r.content = c ? c : "";
    r.editor = e ? e : "";
    r.ts_ms = sqlite3_column_int64(st, 5);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::set_group_memo_open_edit(std::uint64_t group_id,
                                           bool open) {
  if (!group_info(group_id).has_value()) return false;
  const char* sql =
      "UPDATE groups SET open_memo_edit = ? WHERE group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(st, 1, open ? 1 : 0);
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(group_id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::group_memo_open_edit(std::uint64_t group_id) {
  const char* sql =
      "SELECT open_memo_edit FROM groups WHERE group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  bool open = false;
  if (sqlite3_step(st) == SQLITE_ROW) {
    open = sqlite3_column_int(st, 0) != 0;
  }
  sqlite3_finalize(st);
  return open;
}


// —— R24-3 群密码箱：服务端只存 b64 密文/包裹块，不碰明文、无解锁状态 ——
bool ServerStore::group_vault_init(std::uint64_t group_id,
                                   const std::string& kdf_salt,
                                   int kdf_iters,
                                   const std::string& wrapped_dek,
                                   std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO group_vaults (group_id, kdf_salt, kdf_iters, wrapped_dek,"
      " created_ms, updated_ms) VALUES (?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, kdf_salt.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, kdf_iters);
  sqlite3_bind_text(st, 4, wrapped_dek.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  sqlite3_bind_int64(st, 6, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE; // 主键冲突=false（409）
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::group_vault_rekey(std::uint64_t group_id,
                                    const std::string& kdf_salt,
                                    int kdf_iters,
                                    const std::string& wrapped_dek,
                                    std::int64_t ts_ms) {
  const char* sql =
      "UPDATE group_vaults SET kdf_salt = ?, kdf_iters = ?, wrapped_dek = ?,"
      " updated_ms = ? WHERE group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, kdf_salt.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, kdf_iters);
  sqlite3_bind_text(st, 3, wrapped_dek.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, ts_ms);
  sqlite3_bind_int64(st, 5, static_cast<sqlite3_int64>(group_id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok) return false;
  sqlite3_stmt* cnt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT changes();", -1, &cnt, nullptr) !=
      SQLITE_OK) {
    return true;
  }
  const bool touched = sqlite3_step(cnt) == SQLITE_ROW &&
                       sqlite3_column_int(cnt, 0) > 0;
  sqlite3_finalize(cnt);
  return touched; // 无箱行（404）
}

std::optional<ServerStore::GroupVault> ServerStore::group_vault_info(
    std::uint64_t group_id) {
  const char* sql =
      "SELECT group_id, kdf_salt, kdf_iters, wrapped_dek, created_ms,"
      " updated_ms FROM group_vaults WHERE group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  std::optional<GroupVault> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    GroupVault v;
    v.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* w = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    v.kdf_salt = s ? s : "";
    v.kdf_iters = sqlite3_column_int(st, 2);
    v.wrapped_dek = w ? w : "";
    v.created_ms = sqlite3_column_int64(st, 4);
    v.updated_ms = sqlite3_column_int64(st, 5);
    out = std::move(v);
  }
  sqlite3_finalize(st);
  return out;
}

std::int64_t ServerStore::vault_create_entry(
    std::uint64_t group_id, const std::string& name,
    const std::string& account_name, const std::string& secret_ct,
    const std::string& secret_nonce, const std::string& author,
    std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO group_vault_entries (group_id, name, account_name,"
      " secret_ct, secret_nonce, created_by, created_ms, updated_ms)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return -1;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, account_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, secret_ct.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, secret_nonce.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, author.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 7, ts_ms);
  sqlite3_bind_int64(st, 8, ts_ms);
  std::int64_t id = -1;
  if (sqlite3_step(st) == SQLITE_DONE) {
    id = sqlite3_last_insert_rowid(db_);
  }
  sqlite3_finalize(st);
  return id;
}

std::optional<ServerStore::GroupVaultEntry> ServerStore::vault_entry_by_id(
    std::int64_t id) {
  const char* sql =
      "SELECT id, group_id, name, account_name, secret_ct, secret_nonce,"
      " created_by, created_ms, updated_ms FROM group_vault_entries"
      " WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, id);
  std::optional<GroupVaultEntry> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    GroupVaultEntry e;
    e.id = sqlite3_column_int64(st, 0);
    e.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* s[5] = {reinterpret_cast<const char*>(
                            sqlite3_column_text(st, 2)),
                        reinterpret_cast<const char*>(
                            sqlite3_column_text(st, 3)),
                        reinterpret_cast<const char*>(
                            sqlite3_column_text(st, 4)),
                        reinterpret_cast<const char*>(
                            sqlite3_column_text(st, 5)),
                        reinterpret_cast<const char*>(
                            sqlite3_column_text(st, 6))};
    e.name = s[0] ? s[0] : "";
    e.account_name = s[1] ? s[1] : "";
    e.secret_ct = s[2] ? s[2] : "";
    e.secret_nonce = s[3] ? s[3] : "";
    e.created_by = s[4] ? s[4] : "";
    e.created_ms = sqlite3_column_int64(st, 7);
    e.updated_ms = sqlite3_column_int64(st, 8);
    out = std::move(e);
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::vault_update_entry(std::int64_t id,
                                     const std::string& name,
                                     const std::string& account_name,
                                     const std::string& secret_ct,
                                     const std::string& secret_nonce,
                                     const std::string& editor,
                                     std::int64_t ts_ms) {
  const char* sql =
      "UPDATE group_vault_entries SET name = ?, account_name = ?,"
      " secret_ct = ?, secret_nonce = ?, updated_ms = ?"
      " WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, account_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, secret_ct.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, secret_nonce.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  sqlite3_bind_int64(st, 6, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::vault_delete_entry(std::int64_t id, std::uint64_t group_id) {
  const char* sql =
      "DELETE FROM group_vault_entries WHERE id = ? AND group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, id);
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(group_id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::GroupVaultEntry> ServerStore::vault_list_entries(
    std::uint64_t group_id) {
  std::vector<GroupVaultEntry> out;
  const char* sql =
      "SELECT id, group_id, name, account_name, created_by, created_ms,"
      " updated_ms FROM group_vault_entries WHERE group_id = ?"
      " ORDER BY updated_ms DESC, id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupVaultEntry e;
    e.id = sqlite3_column_int64(st, 0);
    e.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* b = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    e.name = n ? n : "";
    e.account_name = a ? a : "";
    e.created_by = b ? b : "";
    e.created_ms = sqlite3_column_int64(st, 5);
    e.updated_ms = sqlite3_column_int64(st, 6);
    out.push_back(std::move(e));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::vault_set_acl(
    std::uint64_t group_id, const std::vector<std::string>& accounts) {
  // 事务替换：清空重灌（空名单=恢复全成员）
  if (sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr) != SQLITE_OK) {
    return false;
  }
  const char* del = "DELETE FROM group_vault_acl WHERE group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, del, -1, &st, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  if (sqlite3_step(st) != SQLITE_DONE) {
    sqlite3_finalize(st);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_finalize(st);
  const char* ins =
      "INSERT OR IGNORE INTO group_vault_acl (group_id, account, added_ms)"
      " VALUES (?, ?, ?);";
  if (sqlite3_prepare_v2(db_, ins, -1, &st, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  const std::int64_t now = now_ms();
  for (const auto& a : accounts) {
    if (a.empty()) continue;
    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
    sqlite3_bind_text(st, 2, a.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, now);
    if (sqlite3_step(st) != SQLITE_DONE) {
      sqlite3_finalize(st);
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
  }
  sqlite3_finalize(st);
  if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  return true;
}

std::vector<std::string> ServerStore::vault_acl_list(std::uint64_t group_id) {
  std::vector<std::string> out;
  const char* sql =
      "SELECT account FROM group_vault_acl WHERE group_id = ?"
      " ORDER BY added_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    out.emplace_back(a ? a : "");
  }
  sqlite3_finalize(st);
  return out;
}

void ServerStore::vault_audit_add(std::uint64_t group_id,
                                  std::int64_t entry_id,
                                  const std::string& actor,
                                  const std::string& action,
                                  std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO group_vault_audit (group_id, entry_id, actor, action,"
      " ts_ms) VALUES (?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_int64(st, 2, entry_id);
  sqlite3_bind_text(st, 3, actor.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, action.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  sqlite3_step(st);
  sqlite3_finalize(st);
}

std::vector<ServerStore::GroupVaultAudit> ServerStore::vault_audit_list(
    std::uint64_t group_id, int limit) {
  std::vector<GroupVaultAudit> out;
  const char* sql =
      "SELECT id, group_id, entry_id, actor, action, ts_ms"
      " FROM group_vault_audit WHERE group_id = ?"
      " ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_int(st, 2, limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupVaultAudit a;
    a.id = sqlite3_column_int64(st, 0);
    a.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    a.entry_id = sqlite3_column_int64(st, 2);
    const char* ac = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* act = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    a.actor = ac ? ac : "";
    a.action = act ? act : "";
    a.ts_ms = sqlite3_column_int64(st, 5);
    out.push_back(std::move(a));
  }
  sqlite3_finalize(st);
  return out;
}

// —— R25-1 群工具框架 ——

bool ServerStore::tool_set_actions(std::uint64_t group_id,
                                   const std::string& tool,
                                   const std::string& actions_json,
                                   const std::string& updated_by,
                                   std::int64_t ts_ms) {
  if (group_id == 0 || tool.empty() || actions_json.empty() ||
      updated_by.empty()) {
    return false;
  }
  if (!group_info(group_id).has_value()) return false;
  const char* sql =
      "INSERT INTO group_tools(group_id, tool, actions_json, updated_by,"
      " updated_ms) VALUES(?, ?, ?, ?, ?)"
      " ON CONFLICT(group_id, tool) DO UPDATE SET actions_json=excluded."
      "actions_json, updated_by=excluded.updated_by, updated_ms=excluded."
      "updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, tool.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, actions_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, updated_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::optional<ServerStore::GroupToolConfig> ServerStore::tool_config(
    std::uint64_t group_id, const std::string& tool) {
  const char* sql =
      "SELECT group_id, tool, actions_json, updated_by, updated_ms"
      " FROM group_tools WHERE group_id = ? AND tool = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, tool.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<GroupToolConfig> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    GroupToolConfig c;
    c.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* u = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    c.tool = t ? t : "";
    c.actions_json = a ? a : "";
    c.updated_by = u ? u : "";
    c.updated_ms = sqlite3_column_int64(st, 4);
    out = std::move(c);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::GroupToolConfig> ServerStore::tool_list(
    std::uint64_t group_id) {
  std::vector<GroupToolConfig> out;
  const char* sql =
      "SELECT group_id, tool, actions_json, updated_by, updated_ms"
      " FROM group_tools WHERE group_id = ? ORDER BY tool ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupToolConfig c;
    c.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* u = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    c.tool = t ? t : "";
    c.actions_json = a ? a : "";
    c.updated_by = u ? u : "";
    c.updated_ms = sqlite3_column_int64(st, 4);
    out.push_back(std::move(c));
  }
  sqlite3_finalize(st);
  return out;
}

void ServerStore::tool_audit_add(std::uint64_t group_id,
                                 const std::string& tool,
                                 const std::string& action,
                                 const std::string& actor,
                                 const std::string& params_json,
                                 const std::string& result_json,
                                 std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO group_tool_audit(group_id, tool, action, actor, params_json,"
      " result_json, ts_ms) VALUES (?, ?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, tool.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, action.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, actor.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, params_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, result_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 7, ts_ms);
  sqlite3_step(st);
  sqlite3_finalize(st);
}

std::vector<ServerStore::GroupToolAudit> ServerStore::tool_audit_list(
    std::uint64_t group_id, int limit) {
  std::vector<GroupToolAudit> out;
  const char* sql =
      "SELECT id, group_id, tool, action, actor, params_json, result_json,"
      " ts_ms FROM group_tool_audit WHERE group_id = ?"
      " ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_int(st, 2, limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupToolAudit a;
    a.id = sqlite3_column_int64(st, 0);
    a.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* ac = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* actor = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    const char* pj = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    const char* rj = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    a.tool = t ? t : "";
    a.action = ac ? ac : "";
    a.actor = actor ? actor : "";
    a.params_json = pj ? pj : "";
    a.result_json = rj ? rj : "";
    a.ts_ms = sqlite3_column_int64(st, 7);
    out.push_back(std::move(a));
  }
  sqlite3_finalize(st);
  return out;
}

// —— R25-2 CI/CD 工具 ——

bool ServerStore::ci_pipeline_upsert(std::uint64_t group_id,
                                     const std::string& name,
                                     const std::string& description,
                                     const std::string& updated_by,
                                     std::int64_t ts_ms) {
  if (!group_info(group_id).has_value()) return false;
  const char* sql =
      "INSERT INTO group_ci_pipelines(group_id, name, description,"
      " updated_by, updated_ms) VALUES(?,?,?,?,?)"
      " ON CONFLICT(group_id, name) DO UPDATE SET"
      " description=excluded.description, updated_by=excluded.updated_by,"
      " updated_ms=excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, description.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, updated_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::ci_pipeline_delete(std::uint64_t group_id,
                                     const std::string& name) {
  const char* sql =
      "DELETE FROM group_ci_pipelines WHERE group_id = ? AND name = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::CiPipeline> ServerStore::ci_pipeline_list(
    std::uint64_t group_id) {
  std::vector<CiPipeline> out;
  const char* sql =
      "SELECT group_id, name, description, updated_by, updated_ms"
      " FROM group_ci_pipelines WHERE group_id = ? ORDER BY name ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    CiPipeline p;
    p.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* d = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* u = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    p.name = n ? n : "";
    p.description = d ? d : "";
    p.updated_by = u ? u : "";
    p.updated_ms = sqlite3_column_int64(st, 4);
    out.push_back(std::move(p));
  }
  sqlite3_finalize(st);
  return out;
}

std::int64_t ServerStore::ci_run_add(std::uint64_t group_id,
                                     const std::string& pipeline,
                                     const std::string& actor,
                                     const std::string& status,
                                     const std::string& params_json,
                                     const std::string& result_json,
                                     std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO group_ci_runs(group_id, pipeline, actor, status,"
      " params_json, result_json, ts_ms) VALUES(?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, pipeline.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, actor.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, params_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, result_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 7, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  const std::int64_t id = ok ? sqlite3_last_insert_rowid(db_) : 0;
  sqlite3_finalize(st);
  return id;
}

std::vector<ServerStore::CiRun> ServerStore::ci_run_list(
    std::uint64_t group_id, const std::string& pipeline, int limit) {
  std::vector<CiRun> out;
  const char* sql =
      "SELECT id, group_id, pipeline, actor, status, params_json,"
      " result_json, ts_ms FROM group_ci_runs WHERE group_id = ?"
      " AND (? = '' OR pipeline = ?) ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, pipeline.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, pipeline.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 4, limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    CiRun r;
    r.id = sqlite3_column_int64(st, 0);
    r.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    const char* pj = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    const char* rj = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.pipeline = p ? p : "";
    r.actor = a ? a : "";
    r.status = s ? s : "";
    r.params_json = pj ? pj : "";
    r.result_json = rj ? rj : "";
    r.ts_ms = sqlite3_column_int64(st, 7);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::CiRun> ServerStore::ci_status_list(
    std::uint64_t group_id) {
  std::vector<CiRun> out;
  // 每条流水线只取最近一笔（id 最大）——红绿灯面
  const char* sql =
      "SELECT id, group_id, pipeline, actor, status, params_json,"
      " result_json, ts_ms FROM group_ci_runs WHERE id IN"
      " (SELECT MAX(id) FROM group_ci_runs WHERE group_id = ?"
      "  GROUP BY pipeline) ORDER BY pipeline ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    CiRun r;
    r.id = sqlite3_column_int64(st, 0);
    r.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    const char* pj = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    const char* rj = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.pipeline = p ? p : "";
    r.actor = a ? a : "";
    r.status = s ? s : "";
    r.params_json = pj ? pj : "";
    r.result_json = rj ? rj : "";
    r.ts_ms = sqlite3_column_int64(st, 7);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// —— R25-3 打包工具 ——

bool ServerStore::pack_artifact_upsert(std::uint64_t group_id,
                                       const std::string& name,
                                       const std::string& version,
                                       const std::string& note,
                                       const std::string& created_by,
                                       std::int64_t ts_ms) {
  if (!group_info(group_id).has_value()) return false;
  const char* sql =
      "INSERT INTO group_pack_artifacts(group_id, name, version, note,"
      " created_by, created_ms) VALUES(?,?,?,?,?,?)"
      " ON CONFLICT(group_id, name, version) DO UPDATE SET"
      " note=excluded.note, created_by=excluded.created_by,"
      " created_ms=excluded.created_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, version.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, note.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, created_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 6, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::PackArtifact> ServerStore::pack_artifact_list(
    std::uint64_t group_id) {
  std::vector<PackArtifact> out;
  const char* sql =
      "SELECT id, group_id, name, version, note, created_by, created_ms"
      " FROM group_pack_artifacts WHERE group_id = ?"
      " ORDER BY created_ms DESC, name ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    PackArtifact a;
    a.id = sqlite3_column_int64(st, 0);
    a.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* v = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* no = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    const char* c = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    a.name = n ? n : "";
    a.version = v ? v : "";
    a.note = no ? no : "";
    a.created_by = c ? c : "";
    a.created_ms = sqlite3_column_int64(st, 6);
    out.push_back(std::move(a));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::pack_artifact_delete(std::uint64_t group_id,
                                       const std::string& name,
                                       const std::string& version) {
  const char* sql =
      "DELETE FROM group_pack_artifacts WHERE group_id = ? AND name = ?"
      " AND version = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, version.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

// —— R25-4 凭据面 ——

bool ServerStore::tool_cred_set(std::uint64_t group_id,
                                const std::string& tool,
                                const std::string& sealed_hex,
                                const std::string& updated_by,
                                std::int64_t ts_ms) {
  if (!group_info(group_id).has_value()) return false;
  const char* sql =
      "INSERT INTO group_tool_credentials(group_id, tool, sealed_hex,"
      " updated_by, updated_ms) VALUES(?,?,?,?,?)"
      " ON CONFLICT(group_id, tool) DO UPDATE SET"
      " sealed_hex=excluded.sealed_hex, updated_by=excluded.updated_by,"
      " updated_ms=excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, tool.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, sealed_hex.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, updated_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::tool_cred_delete(std::uint64_t group_id,
                                   const std::string& tool) {
  const char* sql =
      "DELETE FROM group_tool_credentials WHERE group_id = ? AND tool = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, tool.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::optional<std::string> ServerStore::tool_cred_sealed(
    std::uint64_t group_id, const std::string& tool) {
  const char* sql =
      "SELECT sealed_hex FROM group_tool_credentials WHERE group_id = ?"
      " AND tool = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, tool.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<std::string> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const char* h = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    out = h ? h : "";
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::ToolCredentialMeta> ServerStore::tool_cred_list(
    std::uint64_t group_id) {
  std::vector<ToolCredentialMeta> out;
  const char* sql =
      "SELECT group_id, tool, updated_by, updated_ms"
      " FROM group_tool_credentials WHERE group_id = ? ORDER BY tool ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    ToolCredentialMeta m;
    m.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* u = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    m.tool = t ? t : "";
    m.updated_by = u ? u : "";
    m.updated_ms = sqlite3_column_int64(st, 3);
    out.push_back(std::move(m));
  }
  sqlite3_finalize(st);
  return out;
}

// —— R26-1 服务器 agent 面 ——

std::uint64_t ServerStore::server_enroll(std::uint64_t group_id,
                                         const std::string& name,
                                         const std::string& host,
                                         const std::string& token_hash,
                                         const std::string& enrolled_by,
                                         std::int64_t ts_ms) {
  if (!group_info(group_id).has_value()) return 0;
  // 同 gid+name 复用一行（UNIQUE 闸）：重登记=轮换令牌，id 稳定
  const char* sql =
      "INSERT INTO group_servers(group_id, name, host, token_hash,"
      " enrolled_by, created_ms) VALUES(?,?,?,?,?,?)"
      " ON CONFLICT(group_id, name) DO UPDATE SET host=excluded.host,"
      " token_hash=excluded.token_hash,"
      " enrolled_by=excluded.enrolled_by, created_ms=excluded.created_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, host.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, token_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, enrolled_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 6, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  const std::uint64_t id =
      ok ? static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db_)) : 0;
  sqlite3_finalize(st);
  return id;
}

std::optional<ServerStore::GroupServerRow>
ServerStore::server_by_token_hash(const std::string& token_hash) {
  const char* sql =
      "SELECT id, group_id, name, host, enrolled_by, created_ms,"
      " last_seen_ms, cpu_percent, mem_used_mb, mem_total_mb,"
      " disk_used_mb, disk_total_mb, load1"
      " FROM group_servers WHERE token_hash = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, token_hash.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<GroupServerRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    GroupServerRow r;
    r.id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    r.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* h = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* e = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.name = n ? n : "";
    r.host = h ? h : "";
    r.enrolled_by = e ? e : "";
    r.created_ms = sqlite3_column_int64(st, 5);
    r.last_seen_ms = sqlite3_column_int64(st, 6);
    r.cpu_percent = sqlite3_column_double(st, 7);
    r.mem_used_mb = sqlite3_column_double(st, 8);
    r.mem_total_mb = sqlite3_column_double(st, 9);
    r.disk_used_mb = sqlite3_column_double(st, 10);
    r.disk_total_mb = sqlite3_column_double(st, 11);
    r.load1 = sqlite3_column_double(st, 12);
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::server_heartbeat(std::uint64_t id, double cpu_percent,
                                   double mem_used_mb, double mem_total_mb,
                                   double disk_used_mb, double disk_total_mb,
                                   double load1, std::int64_t ts_ms) {
  const char* sql =
      "UPDATE group_servers SET last_seen_ms=?, cpu_percent=?,"
      " mem_used_mb=?, mem_total_mb=?, disk_used_mb=?, disk_total_mb=?,"
      " load1=? WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, ts_ms);
  sqlite3_bind_double(st, 2, cpu_percent);
  sqlite3_bind_double(st, 3, mem_used_mb);
  sqlite3_bind_double(st, 4, mem_total_mb);
  sqlite3_bind_double(st, 5, disk_used_mb);
  sqlite3_bind_double(st, 6, disk_total_mb);
  sqlite3_bind_double(st, 7, load1);
  sqlite3_bind_int64(st, 8, static_cast<sqlite3_int64>(id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::GroupServerRow> ServerStore::server_list(
    std::uint64_t group_id) {
  std::vector<GroupServerRow> out;
  const char* sql =
      "SELECT s.id, s.group_id, s.name, s.host, s.enrolled_by, s.created_ms,"
      " s.last_seen_ms, s.cpu_percent, s.mem_used_mb, s.mem_total_mb,"
      " s.disk_used_mb, s.disk_total_mb, s.load1,"
      " COALESCE(c.updated_by, ''), COALESCE(c.updated_ms, 0)"
      " FROM group_servers s"
      " LEFT JOIN group_server_credentials c ON c.server_id = s.id"
      " WHERE s.group_id = ? ORDER BY s.id ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupServerRow r;
    r.id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    r.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* h = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* e = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.name = n ? n : "";
    r.host = h ? h : "";
    r.enrolled_by = e ? e : "";
    r.created_ms = sqlite3_column_int64(st, 5);
    r.last_seen_ms = sqlite3_column_int64(st, 6);
    r.cpu_percent = sqlite3_column_double(st, 7);
    r.mem_used_mb = sqlite3_column_double(st, 8);
    r.mem_total_mb = sqlite3_column_double(st, 9);
    r.disk_used_mb = sqlite3_column_double(st, 10);
    r.disk_total_mb = sqlite3_column_double(st, 11);
    r.load1 = sqlite3_column_double(st, 12);
    const char* cb = reinterpret_cast<const char*>(sqlite3_column_text(st, 13));
    r.cred_updated_by = cb ? cb : "";
    r.cred_updated_ms = sqlite3_column_int64(st, 14);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// —— R26-3 远程会话面 ——

std::uint64_t ServerStore::server_session_open(std::uint64_t group_id,
                                               std::uint64_t server_id,
                                               const std::string& actor,
                                               const std::string& protocol,
                                               const std::string& ticket_hash,
                                               std::int64_t ts_ms) {
  // 短票锚定的服务器须属该群（不透他群服务器存在性）
  sqlite3_stmt* chk = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT id FROM group_servers WHERE id = ? AND"
                         " group_id = ?;",
                         -1, &chk, nullptr) != SQLITE_OK) {
    return 0;
  }
  sqlite3_bind_int64(chk, 1, static_cast<sqlite3_int64>(server_id));
  sqlite3_bind_int64(chk, 2, static_cast<sqlite3_int64>(group_id));
  const bool owned = sqlite3_step(chk) == SQLITE_ROW;
  sqlite3_finalize(chk);
  if (!owned) return 0;
  const char* sql =
      "INSERT INTO group_server_sessions(group_id, server_id, actor,"
      " protocol, ticket_hash, opened_ms) VALUES(?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(server_id));
  sqlite3_bind_text(st, 3, actor.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, protocol.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, ticket_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 6, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  const std::uint64_t id =
      ok ? static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db_)) : 0;
  sqlite3_finalize(st);
  return id;
}

std::optional<ServerStore::ServerSessionRow>
ServerStore::server_session_by_ticket(const std::string& ticket_hash) {
  const char* sql =
      "SELECT s.id, s.group_id, s.server_id, g.name, g.host, s.actor,"
      " s.protocol, s.opened_ms, s.redeemed_ms, s.closed_ms"
      " FROM group_server_sessions s"
      " JOIN group_servers g ON g.id = s.server_id"
      " WHERE s.ticket_hash = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, ticket_hash.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<ServerSessionRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    ServerSessionRow r;
    r.id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    r.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    r.server_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 2));
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* h = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.server_name = n ? n : "";
    r.host = h ? h : "";
    r.actor = a ? a : "";
    r.protocol = p ? p : "";
    r.opened_ms = sqlite3_column_int64(st, 7);
    r.redeemed_ms = sqlite3_column_int64(st, 8);
    r.closed_ms = sqlite3_column_int64(st, 9);
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::server_session_mark_redeemed(std::uint64_t id,
                                               std::int64_t ts_ms) {
  // 一次性闸：redeemed_ms=0 才更新（并发双兑只有一胜）
  const char* sql =
      "UPDATE group_server_sessions SET redeemed_ms=? WHERE id = ? AND"
      " redeemed_ms = 0;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, ts_ms);
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::server_session_close(std::uint64_t group_id,
                                       std::uint64_t id,
                                       const std::string& actor,
                                       std::int64_t ts_ms) {
  const char* sql =
      "UPDATE group_server_sessions SET closed_ms=? WHERE id = ? AND"
      " group_id = ? AND actor = ? AND closed_ms = 0;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, ts_ms);
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(id));
  sqlite3_bind_int64(st, 3, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 4, actor.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::ServerSessionRow> ServerStore::server_session_list(
    std::uint64_t group_id) {
  std::vector<ServerSessionRow> out;
  const char* sql =
      "SELECT s.id, s.group_id, s.server_id, g.name, g.host, s.actor,"
      " s.protocol, s.opened_ms, s.redeemed_ms, s.closed_ms"
      " FROM group_server_sessions s"
      " JOIN group_servers g ON g.id = s.server_id"
      " WHERE s.group_id = ? ORDER BY s.id DESC LIMIT 200;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    ServerSessionRow r;
    r.id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    r.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    r.server_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 2));
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* h = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.server_name = n ? n : "";
    r.host = h ? h : "";
    r.actor = a ? a : "";
    r.protocol = p ? p : "";
    r.opened_ms = sqlite3_column_int64(st, 7);
    r.redeemed_ms = sqlite3_column_int64(st, 8);
    r.closed_ms = sqlite3_column_int64(st, 9);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// —— R26-4 服务器凭据面 ——

bool ServerStore::server_cred_set(std::uint64_t group_id,
                                  std::uint64_t server_id,
                                  const std::string& sealed_hex,
                                  const std::string& updated_by,
                                  std::int64_t ts_ms) {
  // 服务器须属该群（跨群 id 不给过）
  sqlite3_stmt* own = nullptr;
  const char* own_sql =
      "SELECT 1 FROM group_servers WHERE id = ? AND group_id = ?;";
  if (sqlite3_prepare_v2(db_, own_sql, -1, &own, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(own, 1, static_cast<sqlite3_int64>(server_id));
  sqlite3_bind_int64(own, 2, static_cast<sqlite3_int64>(group_id));
  const bool owned = sqlite3_step(own) == SQLITE_ROW;
  sqlite3_finalize(own);
  if (!owned) return false;
  const char* sql =
      "INSERT INTO group_server_credentials(server_id, sealed_hex,"
      " updated_by, updated_ms) VALUES(?,?,?,?)"
      " ON CONFLICT(server_id) DO UPDATE SET"
      " sealed_hex=excluded.sealed_hex, updated_by=excluded.updated_by,"
      " updated_ms=excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(server_id));
  sqlite3_bind_text(st, 2, sealed_hex.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, updated_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::server_cred_delete(std::uint64_t group_id,
                                     std::uint64_t server_id) {
  const char* sql =
      "DELETE FROM group_server_credentials WHERE server_id = ? AND"
      " EXISTS(SELECT 1 FROM group_servers s"
      " WHERE s.id = server_id AND s.group_id = ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(server_id));
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(group_id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::optional<std::string> ServerStore::server_cred_sealed(
    std::uint64_t server_id) {
  const char* sql =
      "SELECT sealed_hex FROM group_server_credentials WHERE server_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(server_id));
  std::optional<std::string> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const char* h = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    out = h ? h : "";
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::ServerCredentialMeta> ServerStore::server_cred_list(
    std::uint64_t group_id) {
  std::vector<ServerCredentialMeta> out;
  const char* sql =
      "SELECT c.server_id, c.updated_by, c.updated_ms"
      " FROM group_server_credentials c"
      " JOIN group_servers s ON s.id = c.server_id"
      " WHERE s.group_id = ? ORDER BY c.server_id ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    ServerCredentialMeta m;
    m.server_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    const char* u = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    m.updated_by = u ? u : "";
    m.updated_ms = sqlite3_column_int64(st, 2);
    out.push_back(std::move(m));
  }
  sqlite3_finalize(st);
  return out;
}

// —— 二期群工具三件（原生互动，不走 R25 外部工具代理）——

std::int64_t ServerStore::poll_create(std::uint64_t group_id,
                                      const std::string& topic,
                                      const std::vector<std::string>& options,
                                      std::int64_t deadline_ms,
                                      const std::string& by,
                                      std::int64_t ts_ms, bool anonymous,
                                      bool multi) {
  if (!group_info(group_id).has_value()) return 0;
  if (topic.empty() || options.size() < 2 || options.size() > 10) return 0;
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& o : options) arr.push_back(o);
  const char* sql =
      "INSERT INTO group_polls(group_id, topic, options_json, deadline_ms,"
      " closed, anonymous, multi, created_by, created_ms)"
      " VALUES(?, ?, ?, ?, 0, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, topic.c_str(), -1, SQLITE_TRANSIENT);
  const std::string arr_s = arr.dump();
  sqlite3_bind_text(st, 3, arr_s.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, deadline_ms);
  sqlite3_bind_int64(st, 5, anonymous ? 1 : 0);
  sqlite3_bind_int64(st, 6, multi ? 1 : 0);
  sqlite3_bind_text(st, 7, by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 8, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  const std::int64_t id = ok ? sqlite3_last_insert_rowid(db_) : 0;
  sqlite3_finalize(st);
  return id;
}

std::optional<ServerStore::GroupPoll> ServerStore::poll_by_id(
    std::int64_t id) {
  const char* sql =
      "SELECT id, group_id, topic, options_json, deadline_ms, closed,"
      " anonymous, multi, created_by, created_ms"
      " FROM group_polls WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, id);
  std::optional<GroupPoll> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    GroupPoll p;
    p.id = sqlite3_column_int64(st, 0);
    p.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    p.topic = t ? t : "";
    const char* oj = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    if (oj) {
      const auto arr = nlohmann::json::parse(oj, nullptr, false);
      if (arr.is_array()) {
        for (const auto& e : arr) {
          if (e.is_string()) p.options.push_back(e.get<std::string>());
        }
      }
    }
    p.deadline_ms = sqlite3_column_int64(st, 4);
    p.closed = sqlite3_column_int64(st, 5) != 0;
    p.anonymous = sqlite3_column_int64(st, 6) != 0;
    p.multi = sqlite3_column_int64(st, 7) != 0;
    const char* cb = reinterpret_cast<const char*>(sqlite3_column_text(st, 8));
    p.created_by = cb ? cb : "";
    p.created_ms = sqlite3_column_int64(st, 9);
    out = std::move(p);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::GroupPoll> ServerStore::polls_list(
    std::uint64_t group_id) {
  std::vector<GroupPoll> out;
  const char* sql =
      "SELECT id, group_id, topic, options_json, deadline_ms, closed,"
      " anonymous, multi, created_by, created_ms"
      " FROM group_polls WHERE group_id = ?"
      " ORDER BY id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupPoll p;
    p.id = sqlite3_column_int64(st, 0);
    p.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    p.topic = t ? t : "";
    const char* oj = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    if (oj) {
      const auto arr = nlohmann::json::parse(oj, nullptr, false);
      if (arr.is_array()) {
        for (const auto& e : arr) {
          if (e.is_string()) p.options.push_back(e.get<std::string>());
        }
      }
    }
    p.deadline_ms = sqlite3_column_int64(st, 4);
    p.closed = sqlite3_column_int64(st, 5) != 0;
    p.anonymous = sqlite3_column_int64(st, 6) != 0;
    p.multi = sqlite3_column_int64(st, 7) != 0;
    const char* cb = reinterpret_cast<const char*>(sqlite3_column_text(st, 8));
    p.created_by = cb ? cb : "";
    p.created_ms = sqlite3_column_int64(st, 9);
    out.push_back(std::move(p));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::poll_vote(std::int64_t poll_id, const std::string& account,
                            int choice, std::int64_t ts_ms) {
  const auto p = poll_by_id(poll_id);
  // 到点视同已关闭（与 closed 同语义；store 层兜底——直接调 store 的
  // 路径同样被挡，惰性判定不回写）
  if (!p.has_value() || p->closed ||
      (p->deadline_ms > 0 && ts_ms >= p->deadline_ms)) {
    return false;
  }
  const int n = static_cast<int>(p->options.size());
  if (p->multi) {
    // 多选：choice=位集（bit i=选 i+1 号）——非零且每位都在界内
    if (choice < 1 || choice >= (1 << n)) return false;
  } else {
    if (choice < 1 || choice > n) return false;
  }
  const char* sql =
      "INSERT INTO group_poll_votes(poll_id, account, choice, ts_ms)"
      " VALUES(?, ?, ?, ?)"
      " ON CONFLICT(poll_id, account) DO UPDATE SET choice = excluded.choice,"
      " ts_ms = excluded.ts_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, poll_id);
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, choice);
  sqlite3_bind_int64(st, 4, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::GroupPollVote> ServerStore::poll_votes(
    std::int64_t poll_id) {
  std::vector<GroupPollVote> out;
  const char* sql =
      "SELECT account, choice, ts_ms FROM group_poll_votes"
      " WHERE poll_id = ? ORDER BY ts_ms ASC, account ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, poll_id);
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupPollVote v;
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    v.account = a ? a : "";
    v.choice = sqlite3_column_int(st, 1);
    v.ts_ms = sqlite3_column_int64(st, 2);
    out.push_back(std::move(v));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::poll_close(std::int64_t poll_id) {
  const auto p = poll_by_id(poll_id);
  if (!p.has_value()) return false;
  if (p->closed) return true; // 幂等收口
  const char* sql = "UPDATE group_polls SET closed = 1 WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, poll_id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

std::int64_t ServerStore::chain_create(std::uint64_t group_id,
                                       const std::string& title,
                                       const std::string& format_hint,
                                       const std::string& by,
                                       std::int64_t ts_ms) {
  if (!group_info(group_id).has_value()) return 0;
  if (title.empty()) return 0;
  const char* sql =
      "INSERT INTO group_chains(group_id, title, format_hint, closed,"
      " created_by, created_ms) VALUES(?, ?, ?, 0, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, format_hint.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  const std::int64_t id = ok ? sqlite3_last_insert_rowid(db_) : 0;
  sqlite3_finalize(st);
  return id;
}

std::optional<ServerStore::GroupChain> ServerStore::chain_by_id(
    std::int64_t id) {
  const char* sql =
      "SELECT id, group_id, title, format_hint, closed, created_by,"
      " created_ms FROM group_chains WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, id);
  std::optional<GroupChain> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    GroupChain c;
    c.id = sqlite3_column_int64(st, 0);
    c.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    c.title = t ? t : "";
    const char* fh = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    c.format_hint = fh ? fh : "";
    c.closed = sqlite3_column_int64(st, 4) != 0;
    const char* cb = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    c.created_by = cb ? cb : "";
    c.created_ms = sqlite3_column_int64(st, 6);
    out = std::move(c);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::GroupChain> ServerStore::chains_list(
    std::uint64_t group_id) {
  std::vector<GroupChain> out;
  const char* sql =
      "SELECT id, group_id, title, format_hint, closed, created_by,"
      " created_ms FROM group_chains WHERE group_id = ? ORDER BY id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupChain c;
    c.id = sqlite3_column_int64(st, 0);
    c.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    c.title = t ? t : "";
    const char* fh = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    c.format_hint = fh ? fh : "";
    c.closed = sqlite3_column_int64(st, 4) != 0;
    const char* cb = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    c.created_by = cb ? cb : "";
    c.created_ms = sqlite3_column_int64(st, 6);
    out.push_back(std::move(c));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::chain_join(std::int64_t chain_id,
                             const std::string& account,
                             const std::string& content, std::int64_t ts_ms) {
  const auto c = chain_by_id(chain_id);
  if (!c.has_value() || c->closed) return false;
  if (content.empty()) return false;
  const char* sql =
      "INSERT INTO group_chain_entries(chain_id, account, content, ts_ms)"
      " VALUES(?, ?, ?, ?)"
      " ON CONFLICT(chain_id, account) DO UPDATE SET content ="
      " excluded.content, ts_ms = excluded.ts_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, chain_id);
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, content.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::GroupChainEntry> ServerStore::chain_entries(
    std::int64_t chain_id) {
  std::vector<GroupChainEntry> out;
  const char* sql =
      "SELECT account, content, ts_ms FROM group_chain_entries"
      " WHERE chain_id = ? ORDER BY ts_ms ASC, account ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, chain_id);
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupChainEntry e;
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    e.account = a ? a : "";
    const char* c = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    e.content = c ? c : "";
    e.ts_ms = sqlite3_column_int64(st, 2);
    out.push_back(std::move(e));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::chain_close(std::int64_t chain_id) {
  const auto c = chain_by_id(chain_id);
  if (!c.has_value()) return false;
  if (c->closed) return true; // 幂等收口
  const char* sql = "UPDATE group_chains SET closed = 1 WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, chain_id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

std::int64_t ServerStore::gtask_create(std::uint64_t group_id,
                                       const std::string& title,
                                       const std::string& assignee,
                                       std::int64_t due_ms,
                                       const std::string& by,
                                       std::int64_t ts_ms) {
  if (!group_info(group_id).has_value()) return 0;
  if (title.empty()) return 0;
  if (!assignee.empty() && !is_group_member(group_id, assignee)) return 0;
  const char* sql =
      "INSERT INTO group_tasks(group_id, title, assignee, due_ms, status,"
      " created_by, created_ms) VALUES(?, ?, ?, ?, 'todo', ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, assignee.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, due_ms);
  sqlite3_bind_text(st, 5, by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 6, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  const std::int64_t id = ok ? sqlite3_last_insert_rowid(db_) : 0;
  sqlite3_finalize(st);
  return id;
}

std::vector<ServerStore::GroupTask> ServerStore::gtasks_list(
    std::uint64_t group_id) {
  std::vector<GroupTask> out;
  const char* sql =
      "SELECT id, group_id, title, assignee, due_ms, claimed_ms, status,"
      " created_by, created_ms, done_by, done_ms FROM group_tasks"
      " WHERE group_id = ? ORDER BY id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupTask t;
    t.id = sqlite3_column_int64(st, 0);
    t.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* ti = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    t.title = ti ? ti : "";
    const char* as = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    t.assignee = as ? as : "";
    t.due_ms = sqlite3_column_int64(st, 4);
    t.claimed_ms = sqlite3_column_int64(st, 5);
    const char* stt = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    t.status = stt ? stt : "todo";
    const char* cb = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
    t.created_by = cb ? cb : "";
    t.created_ms = sqlite3_column_int64(st, 8);
    const char* db2 = reinterpret_cast<const char*>(sqlite3_column_text(st, 9));
    t.done_by = db2 ? db2 : "";
    t.done_ms = sqlite3_column_int64(st, 10);
    out.push_back(std::move(t));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::gtask_claim(std::int64_t id, const std::string& account,
                              std::int64_t ts_ms) {
  // 认领＝todo 且无人认领；占位原子落（status/assignee 同行判）
  const char* sql =
      "UPDATE group_tasks SET assignee = ?, claimed_ms = ? WHERE id = ?"
      " AND status = 'todo' AND assignee = '';";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, ts_ms);
  sqlite3_bind_int64(st, 3, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::gtask_done(std::int64_t id, const std::string& done_by,
                             std::int64_t ts_ms) {
  // 终态留痕：todo→done 一次性迁移，done_by/done_ms 落行不删行
  const char* sql =
      "UPDATE group_tasks SET status = 'done', done_by = ?, done_ms = ?"
      " WHERE id = ? AND status = 'todo';";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, done_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, ts_ms);
  sqlite3_bind_int64(st, 3, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

} // namespace memex::server
