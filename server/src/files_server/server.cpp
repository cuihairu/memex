// FileServer 公开面（LAY-003 拆分）：构造/析构/端口/回调/accept 链、
// verify_file_objects 与 make_file_sessions（对外 API 见 files_server.hpp）。

#include "files_server.hpp"
#include "files_server/conn.hpp"
#include "files_server/impl.hpp"
#include "files_server/common.hpp"

namespace memex::server {

std::shared_ptr<FileSessions> make_file_sessions() {
  return std::make_shared<FileSessions>();
}
FileServer::FileServer(asio::io_context& io, ServerStore& store,
                       std::shared_ptr<S3Storage> storage, std::uint16_t port,
                       bool uplink_mode,
                       std::shared_ptr<FileSessions> sessions,
                       UplinkPolicy uplink_policy, LdapDirectory* ldap)
    : impl_(std::make_unique<Impl>(store, std::move(storage), uplink_mode,
                                   std::move(sessions),
                                   std::move(uplink_policy), ldap)),
      acceptor_(io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)) {}

FileServer::~FileServer() {
  // 先落旗标再随成员析构关闭 acceptor：挂起的 accept 完成回调见到
  // false 即短路（回调持有旗标的 shared_ptr，生命周期独立于实例）
  alive_->store(false);
}

std::uint16_t FileServer::port() const {
  return acceptor_.local_endpoint().port();
}

void FileServer::set_notice(GroupNoticeFn fn) { impl_->notice = std::move(fn); }

void FileServer::set_group_text(GroupTextFn fn) {
  impl_->group_text = std::move(fn);
}

void FileServer::set_tool_cred_secret(const std::string& secret) {
  // 派生空（secret 空）＝凭据面未启用；只存派生密钥，明文主密钥不驻留
  impl_->tool_cred_key = derive_tool_cred_key(secret);
}

void FileServer::set_task_pull_secret(const std::string& secret) {
  // R27-5 任务源拉取面：PBKDF2 同水位派生（空=未启用，/files/task-sources
  // 一律 503）；明文主密钥不驻留
  impl_->task_pull_key = derive_task_pull_key(secret);
}

void FileServer::start_accept() {
  if (impl_->uplink_mode) {
    // 安全默认（用户硬要求）：外网入口默认关闭、显式开启、开启时明示
    // 暴露范围——审计面日志固定带这行
    std::cout << "[MEMEX] uplink 监听 0.0.0.0:" << port()
              << "（外网单向：仅 /uplink/session|upload|mine|delete 与"
              << " /uplink/health；无任何下载/读取内网数据端点；上传全审计）"
              << (impl_->storage ? "" : "（存储未配置：一律 503）")
              << std::endl;
  } else {
    std::cout << "[MEMEX] Files 监听 0.0.0.0:" << port()
              << (impl_->storage ? "" : "（存储未配置：一律 503）")
              << std::endl;
  }
  do_accept();
}

void FileServer::do_accept() {
  // 旗标按值随回调持有：实例析构（旗标置 false）后，acceptor 随成员析构
  // 关闭并把挂起的 accept 以 operation_aborted 完成——回调在旗标上短路，
  // 不再触达已析构的 this；错误态一律停链（closed acceptor 续链＝空转）
  const auto alive = alive_;
  acceptor_.async_accept(
      [this, alive](std::error_code ec, asio::ip::tcp::socket socket) {
        if (ec || !alive->load()) {
          if (ec && alive->load()) {
            std::cerr << "[MEMEX] Files accept 停链：" << ec.message()
                      << std::endl;
          }
          return;
        }
        std::make_shared<FileConn>(std::move(socket), *impl_)->start();
        do_accept();
      });
}

FilesVerifyReport verify_file_objects(ServerStore& store, S3Storage& storage) {
  FilesVerifyReport rep;
  std::set<std::string> referenced;
  const auto rows = store.list_files("", "", 1000000, 0, -1);
  for (const auto& m : rows) {
    if (m.object_key.empty()) continue; // 无字节面行不核（不入对象存储）
    referenced.insert(m.object_key);    // 引用面含全部状态行（隔离/过期同算）
    if (m.status != ServerStore::FileStatus::Normal) continue;
    ++rep.checked;
    std::int64_t size = 0;
    if (!storage.head_object(m.object_key, &size)) {
      rep.missing.push_back(m.object_key); // 元数据在、对象缺＝不一致
      continue;
    }
    if (size != m.file_size) {
      rep.mismatches.push_back({m.object_key, m.file_size, size});
    }
  }
  // 孤儿核对走带成败通道的全量列举：list 中途失败（网络/兼容层错）时
  // 短清单静默当「桶内全核过」＝假绿——objects_listed=false 由 CLI 判不通过
  std::vector<std::string> bucket_keys;
  rep.objects_listed = storage.list_objects_checked("", 1000000, bucket_keys);
  for (const auto& key : bucket_keys) {
    ++rep.objects;
    if (referenced.find(key) == referenced.end()) {
      rep.orphans.push_back(key); // 对象在、无元数据引用＝孤儿（告警不判死）
    }
  }
  return rep;
}

} // namespace memex::server
