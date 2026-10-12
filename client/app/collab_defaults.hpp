// 协作面连接默认值（部署批 423）：首次启动向导（登录对话框）与各处服务器
// 地址/端口缺省取同一来源——改一处即全局生效，禁止在各对话框里各写一份。
// 用户已配置的值永远优先：QSettings(memex/collab) 落盘回读，此处常量仅在
// 无配置时兜底（局域网部署改地址不受默认值约束，连接目标始终可改）。
#pragma once

#include <QSettings>
#include <QString>
#include <QStringList>

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

// —— 域名→IP 解析覆盖（部署批 424，局域网直连）——
// 规则串形态「域名=IP;域名2=IP2」（QSettings(memex/collab) 的
// host_overrides 键，设置→连接设置编辑）。命中（域名大小写不敏感精确
// 匹配）回覆盖 IP；规则空/未命中/非法项（缺 = 或空段）一律原样返回——
// 默认不覆盖，公网场景走系统 DNS 过 tunnel 不受影响。纯函数便于单测；
// 覆盖只改连接目标，不改用户配置里的显示地址（用户值永远优先于默认值）。
inline QString apply_host_rules(const QString& host, const QString& rules) {
  const QString h = host.trimmed();
  if (h.isEmpty() || rules.trimmed().isEmpty()) return host;
  const QStringList items = rules.split(QLatin1Char(';'));
  for (const QString& item : items) {
    const int eq = item.indexOf(QLatin1Char('='));
    if (eq <= 0) continue; // 缺 = 或键为空：非法项跳过
    const QString domain = item.left(eq).trimmed();
    const QString ip = item.mid(eq + 1).trimmed();
    if (ip.isEmpty()) continue;
    if (domain.compare(h, Qt::CaseInsensitive) == 0) return ip;
  }
  return host;
}

// 连接侧统一入口：各连接发起处（登录对话框/各工具对话框/文件面回退）把
// 用户输入的 host 过一遍该函数再下发引擎——应用层收口，引擎保持纯库。
inline QString resolve_conn_host(const QString& host) {
  if (host.trimmed().isEmpty()) return host;
  const QSettings s(QStringLiteral("memex"), QStringLiteral("collab"));
  return apply_host_rules(
      host, s.value(QStringLiteral("host_overrides")).toString());
}

} // namespace memex::client
