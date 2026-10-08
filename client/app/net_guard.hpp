// 网络与远程守卫（用户令 2026-10-08 ⑤）：
// - net_guard：网段黑名单纯函数面（「段+掩码」a.b.c.d/len 列表，匹配＝
//   直连发现不响应＋入连接拒）；
// - net_blacklist / remote_control：QSettings 落盘面（黑名单串列表；
//   远程控制配对密码同 away_lock 口径——只存盐＋SHA-256，默认关远程）。
#pragma once

#include <QHostAddress>
#include <QString>
#include <QStringList>

namespace memex::client {

namespace net_guard {
// 单段匹配：addr 是否落在 cidr（a.b.c.d/len）内；形态坏＝不匹配（不误伤）
bool cidr_matches(const QString& cidr, const QHostAddress& addr);
// 黑名单任一段命中即 true（空表＝全放行；仅 IPv4 段参与判定）
bool blocked(const QStringList& cidrs, const QHostAddress& addr);
// 段形态预检（设置页本地门：坏段不发网不落盘）——合法＝「a.b.c.d/len」，
// len 0..32，各节 0..255；返回空串＝合法，否则返回拒因文案
QString validate_cidr(const QString& cidr);
} // namespace net_guard

namespace net_blacklist {
bool enabled(); // 黑名单总开关（默认关＝全放行）
void set_enabled(bool on);
QStringList entries();      // 已录段列表（原样，含未启用时的）
void set_entries(const QStringList& cidrs);
} // namespace net_blacklist

namespace remote_control {
bool enabled(); // 远程控制总开关（默认关＝收方批准面前先挡）
void set_enabled(bool on);
bool has_password();
bool set_password(const QString& pwd); // 空串拒；生成新盐落盘（同 away_lock）
bool verify_password(const QString& pwd);
} // namespace remote_control

} // namespace memex::client
