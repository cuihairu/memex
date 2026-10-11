// IPMSG（飞鸽/飞秋）报文编解码（飞秋兼容 2026-10-11）：纯函数面，
// 不碰 socket——发收与字符集都在这里，网络侧（discovery）只做分流与路由。
// 协议常量按飞秋源码（compilelife/feiq feiqlib/ipmsg.h）与 ipmsg.h 主源
// 对拍定案；报文文本格式 "ver:packet_no:user:host:command:additional"。
#pragma once

#include <QByteArray>
#include <QString>

#include <string>

namespace memex::client::ipmsg {

inline constexpr quint16 kPort = 2425; // 飞秋/IPMSG 固定 UDP 端口
inline constexpr quint32 kVersion = 1;

// 基本命令（command 低 24 位）
inline constexpr quint32 kNoOperation = 0x00000000;
inline constexpr quint32 kBrEntry = 0x00000001;    // 上线广播
inline constexpr quint32 kBrExit = 0x00000002;     // 退出广播
inline constexpr quint32 kAnsEntry = 0x00000003;   // 对 BR_ENTRY 的在线应答
inline constexpr quint32 kBrAbsence = 0x00000004;  // 离开模式/昵称变更通报
inline constexpr quint32 kSendMsg = 0x00000020;    // 发消息
inline constexpr quint32 kRecvMsg = 0x00000021;    // 收到确认（附加区=原包号）

// 选项位（command 高位）
inline constexpr quint32 kSendCheckOpt = 0x00000100;   // 要 RECVMSG 回执
inline constexpr quint32 kSecretOpt = 0x00000200;      // 密文消息（按未加密处理）
inline constexpr quint32 kBroadcastOpt = 0x00000400;   // 广播（免回执组）
inline constexpr quint32 kAutoRetOpt = 0x00002000;     // 自动应答（免回执组）
inline constexpr quint32 kNoAddListOpt = 0x00080000;   // 不入列
inline constexpr quint32 kFileAttachOpt = 0x00200000;  // 带附件（附加区尾部）
inline constexpr quint32 kEncryptOpt = 0x00400000;     // 协商加密
inline constexpr quint32 kUtf8Opt = 0x00800000;        // 消息体 UTF-8

// 免回执选项（ipmsg.h IPMSG_NO_REPLY_OPTS：带这两位不回 RECVMSG）
inline constexpr quint32 kNoReplyOpts = kBroadcastOpt | kAutoRetOpt;

struct Packet {
  quint32 ver{0};
  quint32 no{0};       // 包号（发送端单调；RECVMSG 附加区回填此号）
  quint32 cmd{0};      // 命令＋选项位
  QByteArray user;     // 头部用户名（本地编码，通常 ASCII）
  QByteArray host;     // 头部主机名
  QByteArray additional; // 附加区原字节（解码按命令走 charset）
};

// 解析一条报文（严格形态：5 个 ':' 分段、ver/no 为十进制；坏形态=false）
bool parse(const QByteArray& raw, Packet& out);

// 编码一条报文（user/host/additional 由调用方按命令给好编码字节）
QByteArray encode(quint32 no, quint32 cmd, const QByteArray& user,
                  const QByteArray& host, const QByteArray& additional);

// 命令号（去选项位）：IPMSG 命令占低 8 位，选项位从第 8 位起
// （SEND_CHECK=0x100 等都在低 24 位内，掩码不能放大）
inline quint32 command_of(quint32 cmd) { return cmd & 0x000000FF; }

// —— 字符集（收发都要按 charset，别假设 UTF-8：飞秋 Windows 端 ANSI
// 码页＝GBK/CP936，新版可 UTF-8；BR_ENTRY/BR_EXIT 协议禁用 UTF8OPT
// ——附加区恒本地编码）——
// 收：utf8=true 按 UTF-8，否则 GB18030（GBK 的超集，真飞秋字节解出无损）
QString decode_text(const QByteArray& bytes, bool utf8);
// 发：先 GBK/CP936（真飞秋口径）；编不出（emoji 等 GBK 外字符）回退
// UTF-8，调用方置 kUtf8Opt（新版飞秋按位识别）。返回字节；used_utf8
// 回填是否走了 UTF-8。
QByteArray encode_text(const QString& text, bool& used_utf8);

} // namespace memex::client::ipmsg
