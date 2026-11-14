// serve 面旗标解析（从 main.cpp cmd_serve 机械搬移）：结果结构＋解析函数，
// 供单测直打（test_serve_args）——字段默认值/文案/退出码与原版逐字一致，
// main.cpp 只保留装配与生命周期。
#pragma once

#include <string>

#include "files_server.hpp" // UplinkPolicy
#include "ldap.hpp"         // LdapConfig
#include "storage.hpp"      // S3Config

namespace memex::server {

// 服务端默认监听端口。评审报告只固定了直连态端口段（UDP 2425–2436／TCP 2426–2437），
// 服务端监听端口未钉死，此默认值待与网络管理侧确认后写入部署文档。
inline constexpr std::uint16_t kDefaultPort = 24360;
// webhook 接入独立端口（T4.10）：与消息端口分离，HTTP 面不与长连接混线；
// --webhook-port 0 可整体关闭接入（其余功能不受影响）。
inline constexpr std::uint16_t kDefaultWebhookPort = 24361;
// 模型网关（平台三期）默认关闭：须显式 --model-port 开启（上游端点未
// 登记时网关空转无意义；安全默认=不暴露）。
inline constexpr std::uint16_t kDefaultModelPort = 24362;

// cmd_serve 的旗标解析结果（默认值＝原 cmd_serve 局部变量初值）
struct ServeArgs {
  std::uint16_t port = kDefaultPort;
  int webhook_port = kDefaultWebhookPort; // int 才能表达 0＝关闭
  int model_port = 0; // 模型网关默认关闭：--model-port N 显式开启
  std::string assistant_bot;   // 群内智能助手（平台三期）：bot 名（空=关）
  std::string assistant_token; // 该 bot 明文 token（建议 env
                               // MEMEX_ASSISTANT_TOKEN——旗标值进 ps）
  int files_port = 0; // 文件面默认关闭：须显式 --files-port 且给 S3 配置
  // 外网单向 uplink 面（R23-4）默认关闭：须显式 --uplink-port 开启
  //（安全默认：开启即明示暴露范围，见装配处日志）
  int uplink_port = 0;
  // R23-5 外网面防护参数：缺省只让危险扩展黑名单生效，其余显式开启
  UplinkPolicy uplink_policy;
  S3Config s3;
  // 544 LDAP 企业身份（可选）：缺省 host 空＝不启用（判登全本地，
  // 轻量部署行为不变）
  LdapConfig ldap_cfg;
  // R25-4 凭据面主密钥（缺省空＝凭据面未启用；建议随部署 env 走
  // ——旗标进 ps，值仅服务端内存内派生）
  std::string tool_cred_secret;
  // R27-5 任务源拉取面主密钥（缺省空＝拉取面未启用＝零外呼；开关另在
  // admin 后台，两道闸全开才拉）
  std::string task_pull_secret;
};

// 解析 serve 面旗标＋S3/LDAP 环境变量兜底（旗标恒赢）。返回 0=成功；
// 2=用法错。argv 契约：必须是「剥离子命令」后的参数段——argv[0] 即
// 首个旗标（如 {"--port","24370"}），不带程序名/子命令（裸词一律按
// 位置参数 rc2 拒）；argc=0 合法（全默认值）。严格解析口径见
// serve_args.cpp 头注释（未知旗标/缺值/布尔带值/非严格数字均 rc2）。
int parse_serve_args(int argc, char** argv, ServeArgs& out);

} // namespace memex::server
