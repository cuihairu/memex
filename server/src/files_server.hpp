// R23-2 内网文件面 HTTP 服务：/files/session|upload|download|list|manage|quota。
// 铁律：客户端永不直连对象存储（字节全过本面）；权限只在元数据层判
//（AuthorizationService＋ServerStore，对象层只管字节）；不用 presign。
// 会话令牌（/files/session 换 Bearer token）只在内存（重启即失效须重登，
// 换取最小暴露面）；口令校验一次（与消息面同源 PBKDF2），HTTP 各请求不
// 重烧 PBKDF2。判权策略注册进 AuthorizationService：隔离文件显式拒读
//（R23-5 杀毒钩子落点）、个人文件仅本人（显式）、群主/管理员可管群文件
//（显式）、群成员读/列/传（入群即继承）、无规则命中一律 Default Deny。
#pragma once

#include <asio.hpp>

#include <cstdint>
#include <memory>

#include "authz.hpp"
#include "storage.hpp"
#include "store.hpp"

namespace memex::server {

class FileServer {
 public:
  // storage 为空＝存储后端未配置：文件面整体 503（消息面不受影响）。
  FileServer(asio::io_context& io, ServerStore& store,
             std::shared_ptr<S3Storage> storage, std::uint16_t port);
  ~FileServer();

  FileServer(const FileServer&) = delete;
  FileServer& operator=(const FileServer&) = delete;

  std::uint16_t port() const;
  void start_accept();

  // 连接处理内部类（匿名空间）需触达：公开类型、私有成员不可触达
  struct Impl;
  std::unique_ptr<Impl> impl_;

 private:
  asio::ip::tcp::acceptor acceptor_;
  void do_accept();
};

} // namespace memex::server
