// R23-2 内网文件面 HTTP 服务：/files/session|upload|download|list|manage|quota。
// 铁律：客户端永不直连对象存储（字节全过本面）；权限只在元数据层判
//（AuthorizationService＋ServerStore，对象层只管字节）；不用 presign。
// 会话令牌（/files/session 换 Bearer token）只在内存（重启即失效须重登，
// 换取最小暴露面）；口令校验一次（与消息面同源 PBKDF2），HTTP 各请求不
// 重烧 PBKDF2。判权策略注册进 AuthorizationService：隔离文件显式拒读
//（R23-5 杀毒钩子落点）、个人文件仅本人（显式）、群主/管理员可管群文件
//（显式）、群成员读/列/传（入群即继承）、无规则命中一律 Default Deny。
// R23-4 外网单向 uplink：uplink_mode=true 的实例只挂 /uplink/* 写入端点
//（session|upload|mine|delete；无任何下载/读取内网数据的路由，404 兜底），
// 物理面分离（独立监听口随隧道单独出网）；token 带 scope，内网面收到
// uplink 令牌一律 403——单向性由服务端两道闸保证，不靠客户端自觉。
#pragma once

#include <asio.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "authz.hpp"
#include "scan.hpp"
#include "storage.hpp"
#include "store.hpp"

namespace memex::server {

// R25-2 工具结果卡片回群回调：FileServer 不持 CollabServer（两面各自
// 独立端口），main 接线 deliver_notice（走 NOTICE 信封＝离线入队＋归档
// ＋在线扇出同一投递面）；测试注桩。urgency 取 Notice::Urgency 数值。
using GroupNoticeFn = std::function<void(const std::string& target,
                                         const std::string& title,
                                         const std::string& content,
                                         int urgency)>;

// 会话库前置声明（实体在 files_server.cpp；调用方只持句柄不 deref）：
// 双实例部署两面共享一份，scope 闸才有实体。
struct FileSessions;
// 建两面共享的会话库句柄（实体私有，FileSession 不外泄）
std::shared_ptr<FileSessions> make_file_sessions();

// R23-5 外网面防护参数（只作用于 uplink_mode=true 实例；按「安全默认」
// 原则：缺省只让危险扩展黑名单生效，其余防护一律显式开启）
struct UplinkPolicy {
  // 危险扩展名黑名单（小写、不带点；无后缀不命中）——缺省常见可执行/
  // 脚本/安装包集；--uplink-ext-denylist 显式给值即整表替换（空串=清空）
  std::vector<std::string> ext_denylist = {
      "exe", "dll", "bat", "cmd", "com", "scr", "msi", "msp", "cpl",
      "hta", "jar", "js", "jse", "vbs", "vbe", "wsf", "wsh", "ps1",
      "sh",  "bash", "apk", "deb", "rpm",
  };
  // 非空＝白名单模式：后缀不在名单内一律拒（无后缀也拒），黑名单失效
  std::vector<std::string> ext_allowlist;
  // uplink 面独立大小上限（收 body 前拦截，413）；0=沿用全局 kMaxUpload
  std::int64_t max_upload_bytes = 0;
  // 非空＝外网登录二次验证：/uplink/session 请求体须带 secondary 字段
  // 匹配（部署级第二口令；TOTP 另批），不匹配 401
  std::string login_secret;
  // 安全扫描钩子（R23-5）：infected 拒收 422 字节不落；空=直通
  std::shared_ptr<UploadScanner> scanner;
};

class FileServer {
 public:
  // storage 为空＝存储后端未配置：文件面整体 503（消息面不受影响）。
  // uplink_mode（R23-4，默认 false）＝外网单向面实例（默认关闭，须显式
  // --uplink-port 才创建）。sessions＝两面共享的会话库句柄：双实例部署
  // 必须共用一份，scope 闸才认得出跨面令牌（uplink 令牌打内网面 403
  // 「只许写入」、内网令牌打 uplink 面 403「须外网会话」）；缺省自建
  // （单实例部署不变）。
  FileServer(asio::io_context& io, ServerStore& store,
             std::shared_ptr<S3Storage> storage, std::uint16_t port,
             bool uplink_mode = false,
             std::shared_ptr<FileSessions> sessions = {},
             UplinkPolicy uplink_policy = {});
  ~FileServer();

  FileServer(const FileServer&) = delete;
  FileServer& operator=(const FileServer&) = delete;

  std::uint16_t port() const;
  void start_accept();

  // 工具结果卡片回群回调（R25-2；缺省未设＝只落账不回群，测试面用）
  void set_notice(GroupNoticeFn fn);

  // 连接处理内部类（匿名空间）需触达：公开类型、私有成员不可触达
  struct Impl;
  std::unique_ptr<Impl> impl_;

 private:
  asio::ip::tcp::acceptor acceptor_;
  void do_accept();
};

} // namespace memex::server
