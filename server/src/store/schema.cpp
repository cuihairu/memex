#include "store.hpp"

#include "cred.hpp"

#include <nlohmann/json.hpp>

#include <sqlite3.h>

#include <chrono>
#include <algorithm>
#include <map>
#include <set>

#include "store/common.hpp"

namespace memex::server {

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
      // 需求批⑩ 在线时长：上下线事件流水（online/offline 配对扫掠求
      // 并集时长，多端并行不叠加）
      "CREATE TABLE IF NOT EXISTS presence_events ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  account TEXT NOT NULL,"
      "  event TEXT NOT NULL,"
      "  ts_ms INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_presence_events_account"
      "  ON presence_events(account, id);"
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
      // 评审四·远期加固：归档完整性账（事件流哈希链清单，分块封账；
      // 旧库 open 即补建，空账=未封状态校验恒真）
      "CREATE TABLE IF NOT EXISTS archive_chain ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  first_event_id INTEGER NOT NULL,"
      "  last_event_id INTEGER NOT NULL,"
      "  event_count INTEGER NOT NULL,"
      "  prev_hash TEXT NOT NULL,"
      "  chain_hash TEXT NOT NULL,"
      "  sealed_ms INTEGER NOT NULL);"
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
      "  ext_key TEXT NOT NULL DEFAULT '',"
      // R27-6：类型/开始时间/状态（done 双写过渡，读取以 status 为准）
      "  type TEXT NOT NULL DEFAULT '任务',"
      "  start_ms INTEGER NOT NULL DEFAULT 0,"
      "  status TEXT NOT NULL DEFAULT '待办',"
      "  start_reminded_ms INTEGER NOT NULL DEFAULT 0);"
      "CREATE INDEX IF NOT EXISTS idx_tasks_owner ON tasks(owner);"
      // R27-6 任务类型清单（服务端配置，admin 后台增删改）：初始四类
      //（回执#6），默认「任务」（补①）——桌面/手机从服务端拉取
      "CREATE TABLE IF NOT EXISTS task_types ("
      "  name TEXT PRIMARY KEY,"
      "  is_default INTEGER NOT NULL DEFAULT 0);"
      "INSERT OR IGNORE INTO task_types(name, is_default) VALUES"
      "  ('任务',1),('需求',0),('缺陷',0),('其他',0);"
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
      // 品牌物料（设计稿 docs/design/品牌物料.md）：全服务器单行聚合
      //（id 钉 1），version 每次写 +1＝客户端变更判据；PNG 字节直存
      // BLOB（校验在路由/CLI 层）
      "CREATE TABLE IF NOT EXISTS branding ("
      "  id INTEGER PRIMARY KEY CHECK (id = 1),"
      "  company_name TEXT NOT NULL DEFAULT '',"
      "  accent TEXT NOT NULL DEFAULT '',"
      "  slogan TEXT NOT NULL DEFAULT '',"
      "  logo BLOB,"
      "  splash BLOB,"
      "  version INTEGER NOT NULL DEFAULT 0,"
      "  updated_ms INTEGER NOT NULL DEFAULT 0);"
      // 表情包素材（需求批②）：个人素材库——收藏随账号走（换机即得）；
      // BLOB 落库（≤1MiB/张，与品牌素材同口径），大文件仍走对象存储面
      "CREATE TABLE IF NOT EXISTS emoji_assets ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  account TEXT NOT NULL,"
      "  name TEXT NOT NULL,"
      "  size INTEGER NOT NULL,"
      "  ts_ms INTEGER NOT NULL,"
      "  bytes BLOB NOT NULL);"
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
      "  updated_ms INTEGER NOT NULL,"
      "  signature TEXT NOT NULL DEFAULT '');"
      // 需求批⑫ 用户头像：每账号四档（32/64/128/256），(account,size) 主键
      // upsert 覆盖；版本戳=MAX(ts_ms) 随行生灭（见 store.hpp 注）
      "CREATE TABLE IF NOT EXISTS member_avatars ("
      "  account TEXT NOT NULL,"
      "  size INTEGER NOT NULL,"
      "  mime TEXT NOT NULL,"
      "  bytes BLOB NOT NULL,"
      "  ts_ms INTEGER NOT NULL,"
      "  PRIMARY KEY(account, size));"
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
      "  topic TEXT NOT NULL DEFAULT '',"
      "  purpose TEXT NOT NULL DEFAULT '',"
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
      // bot 可用范围白名单（细粒度权限）：名单非空才收紧，空=不设限。
      // 两表均无外键（同 group_members/offline_messages 口径）；bot_remove
      // 连带清。created_ms 留痕。
      "CREATE TABLE IF NOT EXISTS bot_group_allow ("
      "  name TEXT NOT NULL,"
      "  group_id INTEGER NOT NULL,"
      "  created_ms INTEGER NOT NULL,"
      "  UNIQUE(name, group_id));"
      "CREATE TABLE IF NOT EXISTS bot_dept_allow ("
      "  name TEXT NOT NULL,"
      "  department_path TEXT NOT NULL,"
      "  created_ms INTEGER NOT NULL,"
      "  UNIQUE(name, department_path));"
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
      // —— R27-5 服务端任务源集成：拉取配置（owner+provider 唯一；开关
      // 默认关=enabled 0）、源凭据（只存密文）、全局开关（kv，缺行=关）——
      "CREATE TABLE IF NOT EXISTS task_pull_configs ("
      "  owner TEXT NOT NULL,"
      "  provider TEXT NOT NULL,"
      "  repo TEXT NOT NULL DEFAULT '',"
      "  enabled INTEGER NOT NULL DEFAULT 0,"
      "  interval_s INTEGER NOT NULL DEFAULT 300,"
      "  last_pull_ms INTEGER NOT NULL DEFAULT 0,"
      "  updated_ms INTEGER NOT NULL DEFAULT 0,"
      "  PRIMARY KEY (owner, provider));"
      "CREATE TABLE IF NOT EXISTS task_source_credentials ("
      "  owner TEXT NOT NULL,"
      "  provider TEXT NOT NULL,"
      "  sealed_hex TEXT NOT NULL,"
      "  updated_ms INTEGER NOT NULL DEFAULT 0,"
      "  PRIMARY KEY (owner, provider));"
      "CREATE TABLE IF NOT EXISTS server_flags ("
      "  key TEXT PRIMARY KEY,"
      "  value TEXT NOT NULL DEFAULT '',"
      "  updated_ms INTEGER NOT NULL DEFAULT 0);"
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
      // —— 邮箱·统一收件箱（设计稿 docs/design/mailbox.md）：条目由
      // 各既有事件源落行（告警=webhook 上报/磁盘阈值/登录异常，通知=
      // @我/任务到期/审批结果/共享变更，公告=R24 群公告），状态按账号·
      // 按条目独立（未读/已读/已处理/确认时刻）；边界②：任何邮箱操作
      // 不触 messages/group_announcement_log/告警台账行——
      "CREATE TABLE IF NOT EXISTS mailbox_items ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  kind TEXT NOT NULL,"        // warning|notice|announce
      "  title TEXT NOT NULL,"
      "  sub TEXT NOT NULL DEFAULT '',"
      "  source TEXT NOT NULL DEFAULT '',"
      "  ref TEXT NOT NULL DEFAULT ''," // 正文/跳转位置（group:N 等）
      // 幂等键（同源同事件不重复入箱；NULL=不判重——SQLite UNIQUE 对
      // NULL 各行互不冲突，空键天然多行）
      "  dedup TEXT,"
      "  created_ms INTEGER NOT NULL,"
      "  UNIQUE(dedup));"
      "CREATE TABLE IF NOT EXISTS mailbox_status ("
      "  item_id INTEGER NOT NULL,"
      "  account TEXT NOT NULL,"
      "  state INTEGER NOT NULL DEFAULT 0," // 0=未读 1=已读 2=已处理
      "  handled_by TEXT NOT NULL DEFAULT '',"
      "  handled_ms INTEGER NOT NULL DEFAULT 0,"
      "  confirmed_ms INTEGER NOT NULL DEFAULT 0,"
      "  PRIMARY KEY(item_id, account));"
      "CREATE INDEX IF NOT EXISTS idx_mailbox_status_account"
      " ON mailbox_status(account, item_id);"
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
  // 旧库迁移（群组-1）：群资料主题/目的（TEXT DEFAULT ''＝存量落空串
  // 即「未填」态——定案①展示口径：群名下方不显示该行）
  sqlite3_exec(db_, "ALTER TABLE groups ADD COLUMN"
                    " topic TEXT NOT NULL DEFAULT ''",
              nullptr, nullptr, nullptr);
  sqlite3_exec(db_, "ALTER TABLE groups ADD COLUMN"
                    " purpose TEXT NOT NULL DEFAULT ''",
              nullptr, nullptr, nullptr);
  // 旧库迁移（R27-2）：任务表补外部任务引用列（provider/ext_key 成对，
  // 空=本地任务；R27-1 建的库无此两列）
  sqlite3_exec(db_, "ALTER TABLE tasks ADD COLUMN"
                    " provider TEXT NOT NULL DEFAULT ''",
              nullptr, nullptr, nullptr);
  sqlite3_exec(db_, "ALTER TABLE tasks ADD COLUMN"
                    " ext_key TEXT NOT NULL DEFAULT ''",
              nullptr, nullptr, nullptr);
  // 旧库迁移（R27-6）：任务表补 type/start_ms/status/start_reminded_ms；
  // status 存量回填 done=1→「完成」否则→「待办」（回执#3；done 列保留
  // 写双份过渡）。回填幂等：新世界 done/status 双写恒一致，重复执行无效果。
  sqlite3_exec(db_, "ALTER TABLE tasks ADD COLUMN"
                    " type TEXT NOT NULL DEFAULT '任务'",
              nullptr, nullptr, nullptr);
  sqlite3_exec(db_, "ALTER TABLE tasks ADD COLUMN"
                    " start_ms INTEGER NOT NULL DEFAULT 0",
              nullptr, nullptr, nullptr);
  sqlite3_exec(db_, "ALTER TABLE tasks ADD COLUMN"
                    " status TEXT NOT NULL DEFAULT '待办'",
              nullptr, nullptr, nullptr);
  sqlite3_exec(db_, "ALTER TABLE tasks ADD COLUMN"
                    " start_reminded_ms INTEGER NOT NULL DEFAULT 0",
              nullptr, nullptr, nullptr);
  sqlite3_exec(db_,
               "UPDATE tasks SET status='完成'"
               " WHERE done=1 AND status='待办'",
               nullptr, nullptr, nullptr);
  // 旧库迁移（第七笔）：投票表补匿名/多选两列（默认 0=记名单选不变）
  sqlite3_exec(db_, "ALTER TABLE group_polls ADD COLUMN"
                    " anonymous INTEGER NOT NULL DEFAULT 0",
              nullptr, nullptr, nullptr);
  sqlite3_exec(db_, "ALTER TABLE group_polls ADD COLUMN"
                    " multi INTEGER NOT NULL DEFAULT 0",
              nullptr, nullptr, nullptr);
  // 旧库迁移（需求批⑪）：成员档案补个性签名列（默认空=未设）
  sqlite3_exec(db_, "ALTER TABLE member_profiles ADD COLUMN"
                    " signature TEXT NOT NULL DEFAULT ''",
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


} // namespace memex::server
