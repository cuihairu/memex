#include "ipmsg.hpp"

#include <QList>

#include <QStringDecoder>
#include <QStringEncoder>

namespace memex::client::ipmsg {

bool parse(const QByteArray& raw, Packet& out) {
  // 形态：ver:packet_no:user:host:command:additional——additional 可为空，
  // 但前五段一个不能少；字段内不容 ':'（协议保证），宽容截断即可
  QList<QByteArray> head = raw.left(1024).split(':');
  if (head.size() < 5) return false;
  const QByteArray extra =
      raw.mid(head[0].size() + head[1].size() + head[2].size() +
              head[3].size() + head[4].size() + 5);

  bool ok_ver = false, ok_no = false, ok_cmd = false;
  const quint32 ver = head[0].toUInt(&ok_ver, 10);
  const quint32 no = head[1].toUInt(&ok_no, 10);
  const quint32 cmd = head[4].toUInt(&ok_cmd, 16);
  if (!ok_ver || !ok_no || !ok_cmd) return false;

  out.ver = ver;
  out.no = no;
  out.cmd = cmd;
  out.user = head[2];
  out.host = head[3];
  out.additional = extra;
  return true;
}

QByteArray encode(quint32 no, quint32 cmd, const QByteArray& user,
                  const QByteArray& host, const QByteArray& additional) {
  QByteArray out;
  out += QByteArray::number(kVersion);
  out += ':';
  out += QByteArray::number(no);
  out += ':';
  out += user;
  out += ':';
  out += host;
  out += ':';
  // 命令区十六进制（飞秋样例 "1:100:zhang:pc:32:hi"——32=SENDMSG）
  out += QByteArray::number(cmd, 16);
  out += ':';
  out += additional;
  return out;
}

QString decode_text(const QByteArray& bytes, bool utf8) {
  if (utf8) return QString::fromUtf8(bytes);
  QStringDecoder dec("GB18030");
  if (!dec.isValid()) return QString::fromLocal8Bit(bytes); // 无 ICU 兜底
  return dec.decode(bytes);
}

QByteArray encode_text(const QString& text, bool& used_utf8) {
  used_utf8 = false;
  // GBK/CP936（真飞秋 Windows ANSI 码页）：emoji 等 GBK 外字符编不出
  // （hasError），不退 GB18030——超集编得出的字节老飞秋照样乱码，不如
  // 回退 UTF-8 让带 UTF8OPT 的新版对端按位解
  QStringEncoder enc("GBK");
  if (enc.isValid()) {
    const QByteArray bytes = enc.encode(text);
    if (!enc.hasError()) return bytes;
  }
  used_utf8 = true;
  return text.toUtf8();
}

} // namespace memex::client::ipmsg
