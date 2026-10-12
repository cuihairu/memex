// 协作面连接默认值（部署批 423）：首次启动向导（登录对话框）与各处服务器
// 地址/端口缺省取同一来源——改一处即全局生效，禁止在各对话框里各写一份。
// 用户已配置的值永远优先：QSettings(memex/collab) 落盘回读，此处常量仅在
// 无配置时兜底（局域网部署改地址不受默认值约束，连接目标始终可改）。
#pragma once

#include <QString>

namespace memex::client {

// 公网入口（Cloudflare tunnel，回源宿主 8088）；局域网经 hosts 强制解析或
// 直接填内网 IP——见 README「公网接入：Cloudflare tunnel」节。
inline QString default_collab_host() {
  return QStringLiteral("server.memex.cuihairu.site");
}

inline QString default_collab_port() {
  return QStringLiteral("24360");
}

// 文件面端口与 compose/server 部署口径一致（Dockerfile EXPOSE 24561）。
inline QString default_files_port() {
  return QStringLiteral("24561");
}

} // namespace memex::client
