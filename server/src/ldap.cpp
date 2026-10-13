// LDAP 最小客户端实现——见 ldap.hpp 头注（红线口径）。
// 线协议：RFC 4511（BER）子集——bind/search/unbind，simple auth；禁用
// 检测用 extensibleMatch（AD 停用位）；组从用户条目 memberOf 读。
// 传输：POSIX TCP＋SO 收发超时；ldaps 走 OpenSSL（libssl，整连接 TLS）。
#include "ldap.hpp"

#include <openssl/ssl.h>

#include "cred.hpp"  // random_salt_hex（影子账号随机口令）
#include "store.hpp" // ServerStore 完整类型（判登路由＋角色同步）

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace memex::server {

namespace {

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// —— BER mini-writer（RFC 4511 所需子集）——
std::string ber_len(std::size_t n) {
  if (n < 0x80) return std::string(1, static_cast<char>(n));
  std::string bytes;
  while (n > 0) {
    bytes.insert(bytes.begin(), static_cast<char>(n & 0xFF));
    n >>= 8;
  }
  return std::string(1, static_cast<char>(0x80 | bytes.size())) + bytes;
}

std::string ber_tlv(unsigned char tag, const std::string& content) {
  return std::string(1, static_cast<char>(tag)) + ber_len(content.size()) +
         content;
}

std::string ber_int(std::int64_t v) {
  std::string bytes;
  // 最小字节编码（含符号位处理；协议里值都很小）
  bool started = false;
  for (int shift = 56; shift >= 0; shift -= 8) {
    const auto byte = static_cast<unsigned char>((v >> shift) & 0xFF);
    if (!started) {
      if (shift > 0 && byte == 0x00 &&
          (shift == 0 || (v >> (shift - 8) & 0x80) == 0))
        continue;
      if (shift > 0 && byte == 0xFF &&
          (shift == 0 || (v >> (shift - 8) & 0x80) != 0))
        continue;
      started = true;
    }
    bytes.push_back(static_cast<char>(byte));
  }
  if (bytes.empty()) bytes.push_back('\0');
  return ber_tlv(0x02, bytes);
}

std::string ber_enum(std::int64_t v) {
  // ENUMERATED 值在协议里都是小非负数（scope/code/limit）——单字节足矣
  return ber_tlv(0x0A, std::string(1, static_cast<char>(v & 0xFF)));
}

std::string ber_bool(bool b) {
  return ber_tlv(0x01, std::string(1, b ? '\xFF' : '\0'));
}

// —— LDAP 过滤器值/DN 值转义 ——
// RFC 4515 过滤器值：* ( ) \ NUL → \XX（用户名入过滤器前的注入防线）
std::string escape_filter_value(const std::string& v) {
  std::string out;
  out.reserve(v.size());
  char buf[4];
  for (unsigned char c : v) {
    if (c == '*' || c == '(' || c == ')' || c == '\\' || c == 0) {
      std::snprintf(buf, sizeof(buf), "\\%02X", c);
      out += buf;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

// DN 模板 {u} 替换（RDN 值最小转义：RFC 4514 特殊字符）
std::string escape_dn_value(const std::string& v) {
  std::string out;
  out.reserve(v.size() + 2);
  for (unsigned char c : v) {
    if (c == ',' || c == '+' || c == '"' || c == '\\' || c == '<' ||
        c == '>' || c == ';')
      out.push_back('\\');
    out.push_back(static_cast<char>(c));
  }
  return out;
}

std::string replace_u(const std::string& tpl, const std::string& escaped) {
  std::string out;
  for (std::size_t i = 0; i < tpl.size();) {
    if (tpl.compare(i, 3, "{u}") == 0) {
      out += escaped;
      i += 3;
    } else {
      out.push_back(tpl[i++]);
    }
  }
  return out;
}

// 过滤器构造（tag：0xA3 等值 / 0x87 存在 / 0xA9 extensibleMatch）
std::string filter_eq(const std::string& attr, const std::string& value) {
  return ber_tlv(0xA3, ber_tlv(0x04, attr) + ber_tlv(0x04, value));
}
std::string filter_present(const std::string& attr) {
  return ber_tlv(0x87, attr);
}
std::string filter_ext(const std::string& attr, const std::string& rule,
                       const std::string& value) {
  std::string content;
  if (!rule.empty()) content += ber_tlv(0x81, rule); // matchingRule [1]
  content += ber_tlv(0x82, attr);                    // type [2]
  content += ber_tlv(0x83, value);                   // matchValue [3]
  return ber_tlv(0xA9, content);                     // dnAttributes 缺省 false
}

// LDAPMessage ::= SEQUENCE { messageID INTEGER, protocolOp ... }
std::string ldap_message(std::int64_t id, unsigned char op_tag,
                         const std::string& op) {
  return ber_tlv(0x30, ber_int(id) + ber_tlv(op_tag, op));
}

std::string bind_request(const std::string& dn, const std::string& password) {
  // bindRequest 内容（[APPLICATION 0] tag 由 ldap_message 统一封）：
  // { version 3, name, [0] simple password }
  return ber_int(3) + ber_tlv(0x04, dn) + ber_tlv(0x80, password);
}

// scope：0 base / 2 subtree；attrs 请求项（可空）。返回内容（tag 由
// ldap_message 统一封——与 bind_request 同口径，避免双层包裹）。
std::string search_request(const std::string& base, int scope,
                           std::int64_t size_limit, const std::string& filter,
                           const std::vector<std::string>& attrs) {
  std::string attr_sel;
  for (const auto& a : attrs) attr_sel += ber_tlv(0x04, a);
  // searchRequest 内容：{ base, scope, deref 0, sizeLimit, timeLimit,
  // typesOnly false, filter, attributes }
  return ber_tlv(0x04, base) + ber_enum(scope) + ber_enum(0) +
         ber_int(size_limit) + ber_int(0) + ber_bool(false) + filter +
         ber_tlv(0x30, attr_sel);
}

// —— BER mini-reader：解析一层孩子（tag, content）——
struct BerNode {
  unsigned char tag{0};
  std::string content;
};

bool ber_read_node(const std::string& buf, std::size_t& pos, BerNode& out) {
  if (pos + 2 > buf.size()) return false;
  out.tag = static_cast<unsigned char>(buf[pos++]);
  unsigned char first = static_cast<unsigned char>(buf[pos++]);
  std::size_t len = 0;
  if (first < 0x80) {
    len = first;
  } else {
    const int n = first & 0x7F;
    if (n == 0 || n > 4 || pos + static_cast<std::size_t>(n) > buf.size())
      return false;
    for (int i = 0; i < n; ++i)
      len = (len << 8) | static_cast<unsigned char>(buf[pos++]);
  }
  if (pos + len > buf.size()) return false;
  out.content = buf.substr(pos, len);
  pos += len;
  return true;
}

std::vector<BerNode> ber_children(const std::string& content) {
  std::vector<BerNode> out;
  std::size_t pos = 0;
  BerNode n;
  while (pos < content.size() && ber_read_node(content, pos, n))
    out.push_back(n);
  return out;
}

// ENUM resultCode 解值
bool ber_enum_value(const BerNode& n, int& out) {
  if (n.tag != 0x0A || n.content.empty()) return false;
  out = 0;
  for (unsigned char c : n.content) out = (out << 8) | c;
  return true;
}

// —— 连接（POSIX TCP；SO 收发超时；ldaps 走 OpenSSL）——
class LdapConn {
public:
  ~LdapConn() { close(); }

  bool connect(const LdapConfig& cfg, std::string& err) {
    ::addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ::addrinfo* res = nullptr;
    const std::string port = std::to_string(cfg.port);
    if (::getaddrinfo(cfg.host.c_str(), port.c_str(), &hints, &res) != 0 ||
        res == nullptr) {
      err = "地址解析失败: " + cfg.host;
      return false;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(cfg.timeout_ms);
    bool ok = false;
    for (::addrinfo* p = res; p != nullptr && !ok; p = p->ai_next) {
      fd_ = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if (fd_ < 0) continue;
      // 非阻塞 connect＋poll（SO_CONNECTTIMEO 不可移植）
      const int fl = ::fcntl(fd_, F_GETFL, 0);
      ::fcntl(fd_, F_SETFL, fl | O_NONBLOCK);
      if (::connect(fd_, p->ai_addr, p->ai_addrlen) != 0 &&
          errno != EINPROGRESS) {
        ::close(fd_);
        fd_ = -1;
        continue;
      }
      ::pollfd pfd{fd_, POLLOUT, 0};
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0 ||
          ::poll(&pfd, 1, static_cast<int>(left.count())) != 1) {
        ::close(fd_);
        fd_ = -1;
        continue;
      }
      int soerr = 0;
      ::socklen_t slen = sizeof(soerr);
      if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 ||
          soerr != 0) {
        ::close(fd_);
        fd_ = -1;
        continue;
      }
      ::fcntl(fd_, F_SETFL, fl); // 回阻塞；收发超时交给 SO_*TIMEO
      const timeval tv{cfg.timeout_ms / 1000,
                       static_cast<suseconds_t>((cfg.timeout_ms % 1000) * 1000)};
      ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      ok = true;
    }
    ::freeaddrinfo(res);
    if (!ok) {
      if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
      }
      err = "连接超时/失败: " + cfg.host + ":" + port;
      return false;
    }
    if (cfg.use_ssl && !upgrade_tls(cfg, err)) return false;
    return true;
  }

  // ldaps（整连接 TLS）与 StartTLS（升级后）共用握手；证书校验默认
  // 必开（insecure_tls 仅自签场景显式放行——启动日志明示警告）。
  bool upgrade_tls(const LdapConfig& cfg, std::string& err) {
    const std::string port = std::to_string(cfg.port);
    ssl_ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx_) {
      err = "SSL_CTX 创建失败";
      return false;
    }
    ssl_ = SSL_new(ssl_ctx_);
    SSL_set_fd(ssl_, fd_);
    if (cfg.insecure_tls) {
      SSL_set_verify(ssl_, SSL_VERIFY_NONE, nullptr);
    } else {
      SSL_set1_host(ssl_, cfg.host.c_str()); // 证书主机名校验（默认必开）
    }
    // 阻塞握手受 SO_RCVTIMEO 约束（超时返回 ≤0——如实注释：S/R 超时
    // 对握手不保证精确，属已知粗粒度，v1 接受）
    if (SSL_connect(ssl_) != 1) {
      err = "TLS 握手失败: " + cfg.host + ":" + port;
      return false;
    }
    return true;
  }

  // StartTLS（RFC 4511 §4.14）：明连→扩展请求→同一 fd 升级 TLS→再 bind。
  bool starttls(const LdapConfig& cfg, std::string& err) {
    const std::string oid = "1.3.6.1.4.1.1466.20037"; // starttls 扩展 OID
    if (!write_all(
            ldap_message(1, 0x77 /*extendedReq*/, ber_tlv(0x80, oid)))) {
      err = "StartTLS 请求发送失败";
      return false;
    }
    std::string buf;
    BerNode msg;
    std::size_t pos = 0;
    if (!read_message(buf, err) || !ber_read_node(buf, pos, msg) ||
        msg.tag != 0x30) {
      if (err.empty()) err = "StartTLS 应答格式错";
      return false;
    }
    const auto kids = ber_children(msg.content);
    if (kids.size() < 2 || kids[1].tag != 0x78 /*extendedResp*/) {
      err = "应答非 StartTLS 结果";
      return false;
    }
    const auto resp = ber_children(kids[1].content);
    int code = -1;
    if (resp.empty() || !ber_enum_value(resp[0], code) || code != 0) {
      err = "目录拒绝 StartTLS code=" + std::to_string(code);
      return false;
    }
    return upgrade_tls(cfg, err);
  }

  bool write_all(const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
      const int n = ssl_ ? SSL_write(ssl_, data.data() + off,
                                     static_cast<int>(data.size() - off))
                         : ::send(fd_, data.data() + off, data.size() - off, 0);
      if (n <= 0) return false;
      off += static_cast<std::size_t>(n);
    }
    return true;
  }

  bool read_message(std::string& out, std::string& err) {
    // 消息头：tag(1)＋首长度字节(1)；长格式再补字节数。LDAP 报文小，
    // 上限 4MB 防对端疯填长度。
    constexpr std::size_t kMax = 4u * 1024 * 1024;
    char head[2] = {0, 0};
    if (!read_exact(head, 2)) {
      err = "读消息头失败（超时/断连）";
      return false;
    }
    const auto first = static_cast<unsigned char>(head[1]);
    std::size_t len = 0;
    if (first < 0x80) {
      len = first;
    } else {
      const int n = first & 0x7F;
      if (n == 0 || n > 4) {
        err = "BER 长度非法";
        return false;
      }
      char lb[4] = {0, 0, 0, 0};
      if (!read_exact(lb, static_cast<std::size_t>(n))) {
        err = "读长度失败";
        return false;
      }
      for (int i = 0; i < n; ++i)
        len = (len << 8) | static_cast<unsigned char>(lb[i]);
    }
    if (len > kMax) {
      err = "报文超上限";
      return false;
    }
    out.clear();
    out.assign(head, head + 2);
    std::string body(static_cast<std::size_t>(len), '\0');
    if (len > 0 && !read_exact(body.data(), len)) {
      err = "读消息体失败（超时/断连）";
      return false;
    }
    out += body;
    return true;
  }

  void close() {
    if (ssl_) {
      SSL_shutdown(ssl_);
      SSL_free(ssl_);
      ssl_ = nullptr;
    }
    if (ssl_ctx_) {
      SSL_CTX_free(ssl_ctx_);
      ssl_ctx_ = nullptr;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

private:
  bool read_exact(char* dst, std::size_t n) {
    std::size_t off = 0;
    while (off < n) {
      const int got =
          ssl_
              ? SSL_read(ssl_, dst + off, static_cast<int>(n - off))
              : static_cast<int>(
                    ::recv(fd_, dst + off, n - off, 0));
      if (got <= 0) return false;
      off += static_cast<std::size_t>(got);
    }
    return true;
  }

  int fd_{-1};
  SSL* ssl_{nullptr};
  SSL_CTX* ssl_ctx_{nullptr};
};

// 一次「bind → 收 bindResponse → resultCode」往返；true＝收到应答
// （code 出参），false＝网络层失败。
bool do_bind(LdapConn& conn, std::int64_t& msg_id, const std::string& dn,
             const std::string& password, int& code, std::string& diag,
             std::string& err) {
  if (!conn.write_all(
          ldap_message(msg_id++, 0x60 /*bindRequest*/, bind_request(dn, password)))) {
    err = "bind 发送失败";
    return false;
  }
  std::string buf;
  BerNode msg;
  for (;;) {
    if (!conn.read_message(buf, err)) return false;
    std::size_t pos = 0;
    if (!ber_read_node(buf, pos, msg) || msg.tag != 0x30) {
      err = "消息非 SEQUENCE";
      return false;
    }
    auto kids = ber_children(msg.content);
    if (kids.size() >= 2 && kids[0].tag == 0x02) {
      int id = 0;
      ber_enum_value(BerNode{0x0A, kids[0].content}, id);
      if (id != msg_id - 1) continue; // 非本次应答（协议上罕见），丢弃
    }
    if (kids.size() < 2 || kids[1].tag != 0x61 /*bindResponse*/) {
      err = "应答非 bindResponse";
      return false;
    }
    const auto resp = ber_children(kids[1].content);
    if (resp.empty() || !ber_enum_value(resp[0], code)) {
      err = "bindResponse 缺 resultCode";
      return false;
    }
    if (resp.size() >= 3) diag = resp[2].content; // diagnosticMessage
    return true;
  }
}

// 一次 search 往返：返回条目（每条 = DN＋属性表），done 后回收。
struct SearchResultEntry {
  std::string dn;
  std::vector<std::pair<std::string, std::vector<std::string>>> attrs;
};

bool do_search(LdapConn& conn, std::int64_t& msg_id, const std::string& req_op,
               std::vector<SearchResultEntry>& entries, int& code,
               std::string& err) {
  if (!conn.write_all(
          ldap_message(msg_id++, 0x63 /*searchRequest*/, req_op))) {
    err = "search 发送失败";
    return false;
  }
  const int want_id = msg_id - 1;
  entries.clear();
  code = -1;
  for (;;) {
    std::string buf;
    if (!conn.read_message(buf, err)) return false;
    std::size_t pos = 0;
    BerNode msg;
    if (!ber_read_node(buf, pos, msg) || msg.tag != 0x30) {
      err = "消息非 SEQUENCE";
      return false;
    }
    const auto kids = ber_children(msg.content);
    if (kids.size() < 2 || kids[0].tag != 0x02) {
      err = "消息缺 messageID";
      return false;
    }
    int id = 0;
    ber_enum_value(BerNode{0x0A, kids[0].content}, id);
    if (id != want_id) continue;
    if (kids[1].tag == 0x64 /*searchResEntry*/) {
      SearchResultEntry e;
      const auto parts = ber_children(kids[1].content);
      if (!parts.empty()) e.dn = parts[0].content; // objectName
      if (parts.size() >= 2) {
        for (const auto& pa : ber_children(parts[1].content)) {
          std::pair<std::string, std::vector<std::string>> attr;
          const auto fields = ber_children(pa.content);
          if (fields.empty()) continue;
          attr.first = fields[0].content;
          if (fields.size() >= 2)
            for (const auto& v : ber_children(fields[1].content))
              attr.second.push_back(v.content);
          e.attrs.push_back(attr);
        }
      }
      entries.push_back(std::move(e));
      continue;
    }
    if (kids[1].tag == 0x65 /*searchResDone*/) {
      const auto resp = ber_children(kids[1].content);
      if (resp.empty() || !ber_enum_value(resp[0], code)) {
        err = "searchResDone 缺 resultCode";
        return false;
      }
      return true;
    }
    err = "应答非 searchResEntry/Done";
    return false;
  }
}

} // namespace

// —— 配置就绪与脱敏 ——
bool LdapConfig::ready() const {
  return !host.empty() && !user_base.empty() &&
         (!bind_dn_template.empty() ||
          (!bind_dn.empty() && !bind_password.empty()));
}

std::string LdapConfig::tls_posture_error() const {
  if (!use_ssl && !starttls && !allow_plain)
    return "密码明网明文传输：须 --ldap-ssl 或 --ldap-starttls；"
           "仅限测试/受控内网可显式 --ldap-allow-plain";
  if (use_ssl && starttls) return "ldaps 与 StartTLS 互斥（同一连接不能两次升级）";
  return std::string();
}

std::string LdapConfig::placeholder() const {
  LdapConfig c = *this;
  if (!c.bind_password.empty()) c.bind_password = "******";
  return "ldap host=" + c.host + ":" + std::to_string(c.port) +
         (c.use_ssl ? " (ldaps)" : "") + " user_base=" + c.user_base +
         " user_attr=" + c.user_attr +
         (c.bind_dn_template.empty() ? " bind_dn=" + c.bind_dn
                                     : " template=" + c.bind_dn_template) +
         " memberof=" + c.memberof_attr + " groups=" +
         std::to_string(group_roles.size());
}

// —— 客户端主体 ——
LdapDirectory::LdapDirectory(LdapConfig cfg) : cfg_(std::move(cfg)) {}

LdapVerdict LdapDirectory::verify_user(const std::string& username,
                                       const std::string& password) {
  LdapVerdict v;
  if (!cfg_.ready()) {
    v.outcome = LdapVerdict::Outcome::NotConfigured;
    v.detail = "LDAP 配置不齐";
    return v;
  }
  if (!precheck(v)) return v;
  v = do_verify_user(username, password);
  if (v.outcome == LdapVerdict::Outcome::Unavailable)
    unavailable_until_ms_ = now_ms() + cooldown_ms_; // 熔断（collab io
                                                     // 单线程保护——
                                                     // 不可达时免每登录
                                                     // 拖一个超时）
  return v;
}

bool LdapDirectory::precheck(LdapVerdict& v) {
  if (!cfg_.ready()) {
    v.outcome = LdapVerdict::Outcome::NotConfigured;
    v.detail = "LDAP 配置不齐";
    return false;
  }
  const std::int64_t now = now_ms();
  if (now < unavailable_until_ms_) {
    v.outcome = LdapVerdict::Outcome::Unavailable;
    v.detail = "目录熔断冷却中（上次不可达）";
    return false;
  }
  return true;
}

// 拆出实现（verify_user 外包熔断；此函数直连网络）
LdapVerdict LdapDirectory::do_verify_user(const std::string& username,
                                          const std::string& password) {
  LdapVerdict v;
  auto unavailable = [&v](const std::string& why) {
    v.outcome = LdapVerdict::Outcome::Unavailable;
    v.detail = why;
    return v;
  };
  LdapConn conn;
  std::string err;
  if (!conn.connect(cfg_, err)) return unavailable(err);
  if (cfg_.starttls && !conn.starttls(cfg_, err)) return unavailable(err);

  std::int64_t msg_id = 1;
  int code = -1;
  std::string diag;
  std::string user_dn;
  SearchResultEntry user_entry;

  const bool template_mode = !cfg_.bind_dn_template.empty();
  if (template_mode) {
    user_dn = replace_u(cfg_.bind_dn_template, escape_dn_value(username));
    v.user_dn = user_dn; // 模板 DN＝绑定 subject（首登自动绑定用）
    if (!do_bind(conn, msg_id, user_dn, password, code, diag, err))
      return unavailable(err);
    if (code == 49) {
      v.outcome = LdapVerdict::Outcome::BadCredentials;
      v.detail = "目录绑定拒绝（口令不符）";
      return v;
    }
    if (code != 0)
      return unavailable("目录绑定失败 code=" + std::to_string(code) +
                         (diag.empty() ? "" : " " + diag));
  } else {
    // 服务账号先查后绑（AD：DN 拼不出）
    if (!do_bind(conn, msg_id, cfg_.bind_dn, cfg_.bind_password, code, diag,
                 err))
      return unavailable(err);
    if (code != 0)
      return unavailable("服务账号绑定失败 code=" + std::to_string(code));
    const std::string filter =
        filter_eq(cfg_.user_attr, escape_filter_value(username));
    const std::string req = search_request(cfg_.user_base, /*subtree=*/2,
                                           /*size_limit=*/2, filter,
                                           {cfg_.memberof_attr});
    std::vector<SearchResultEntry> entries;
    if (!do_search(conn, msg_id, req, entries, code, err))
      return unavailable(err);
    if (code != 0)
      return unavailable("用户定位失败 code=" + std::to_string(code));
    if (entries.empty()) {
      v.outcome = LdapVerdict::Outcome::BadCredentials;
      v.detail = "目录无此用户";
      return v;
    }
    if (entries.size() > 1)
      return unavailable("用户定位命中多条（user_attr 配置可疑）");
    user_dn = entries[0].dn;
    v.user_dn = user_dn; // 目录定位 DN＝绑定 subject
    user_entry = entries[0];
  }

  // 禁用检测（可选配置；服务模式在用户绑定前＝服务权限口径，模板模式
  // 在用户绑定后＝自读口径）
  if (!cfg_.disabled_attr.empty()) {
    const std::string req = search_request(
        user_dn, /*base=*/0, /*size_limit=*/1,
        filter_ext(cfg_.disabled_attr, cfg_.disabled_rule, cfg_.disabled_value),
        {});
    std::vector<SearchResultEntry> entries;
    if (!do_search(conn, msg_id, req, entries, code, err))
      return unavailable(err);
    if (code != 0)
      return unavailable("禁用检测失败 code=" + std::to_string(code));
    if (!entries.empty()) {
      v.outcome = LdapVerdict::Outcome::Disabled;
      v.detail = "目录侧已禁用（禁用检测命中）";
      return v;
    }
  }

  if (!template_mode) {
    // 用户口令校验（服务模式：定位/禁用过了才绑用户）
    if (!do_bind(conn, msg_id, user_dn, password, code, diag, err))
      return unavailable(err);
    if (code == 49) {
      v.outcome = LdapVerdict::Outcome::BadCredentials;
      v.detail = "目录绑定拒绝（口令不符）";
      return v;
    }
    if (code != 0)
      return unavailable("目录绑定失败 code=" + std::to_string(code));
  }

  // 组：模板模式自读一次；服务模式定位时已带回 memberOf
  if (template_mode) {
    const std::string req =
        search_request(user_dn, /*base=*/0, /*size_limit=*/1,
                       filter_present("objectClass"), {cfg_.memberof_attr});
    std::vector<SearchResultEntry> entries;
    if (!do_search(conn, msg_id, req, entries, code, err))
      return unavailable(err);
    if (code == 0 && entries.size() == 1) user_entry = entries[0];
  }
  for (const auto& [type, vals] : user_entry.attrs)
    if (type == cfg_.memberof_attr) v.groups = vals;

  // 礼貌 unbind（失败不回改判定——服务端半途断开无碍）
  conn.write_all(ldap_message(msg_id++, 0x42 /*unbind*/, ""));
  v.outcome = LdapVerdict::Outcome::Ok;
  return v;
}

// —— 存量会话复检（目录禁用策略）——
LdapVerdict LdapDirectory::check_disabled(const std::string& subject) {
  LdapVerdict v;
  if (!precheck(v)) return v;
  if (cfg_.disabled_attr.empty()) {
    v.outcome = LdapVerdict::Outcome::Ok;
    v.detail = "未配禁用检测，存量会话不复检";
    return v;
  }
  if (cfg_.bind_dn.empty() || cfg_.bind_password.empty()) {
    v.outcome = LdapVerdict::Outcome::Ok;
    v.detail = "模板模式无服务凭据，存量会话不复检";
    return v;
  }
  LdapConn conn;
  std::string err;
  if (!conn.connect(cfg_, err)) {
    v.outcome = LdapVerdict::Outcome::Unavailable;
    v.detail = err;
    unavailable_until_ms_ = now_ms() + cooldown_ms_;
    return v;
  }
  if (cfg_.starttls && !conn.starttls(cfg_, err)) {
    v.outcome = LdapVerdict::Outcome::Unavailable;
    v.detail = err;
    unavailable_until_ms_ = now_ms() + cooldown_ms_;
    return v;
  }
  std::int64_t msg_id = 1;
  int code = -1;
  std::string diag;
  if (!do_bind(conn, msg_id, cfg_.bind_dn, cfg_.bind_password, code, diag,
               err) ||
      code != 0) {
    v.outcome = LdapVerdict::Outcome::Unavailable;
    v.detail = err.empty() ? "服务账号绑定失败 code=" + std::to_string(code)
                           : err;
    unavailable_until_ms_ = now_ms() + cooldown_ms_;
    return v;
  }
  // subject 即用户 DN（绑定落库时存的就是 DN）——base 检索禁用三元组
  const std::string req =
      search_request(subject, /*base=*/0, /*size_limit=*/1,
                     filter_ext(cfg_.disabled_attr, cfg_.disabled_rule,
                                cfg_.disabled_value),
                     {});
  std::vector<SearchResultEntry> entries;
  if (!do_search(conn, msg_id, req, entries, code, err)) {
    v.outcome = LdapVerdict::Outcome::Unavailable;
    v.detail = err;
    unavailable_until_ms_ = now_ms() + cooldown_ms_;
    return v;
  }
  if (code != 0) {
    // 查询失败≠禁用——宁可不踢也不能误伤（directory 故障不算账）
    v.outcome = LdapVerdict::Outcome::Unavailable;
    v.detail = "禁用复检失败 code=" + std::to_string(code);
    return v; // 不进熔断（单次查询错≠目录整体不可达）
  }
  v.outcome = entries.empty() ? LdapVerdict::Outcome::Ok
                              : LdapVerdict::Outcome::Disabled;
  v.detail = entries.empty() ? "目录侧正常" : "目录侧已禁用（复检命中）";
  return v;
}

// —— 绑定查找（issuer：显式 "ldap" 或自动绑定落库的 "ldap://host"）——
std::optional<ServerStore::IdentityBinding> find_ldap_binding(
    ServerStore& store, const std::string& account) {
  for (const auto& b : store.identity_list(account)) {
    if (b.issuer == "ldap" || b.issuer.rfind("ldap://", 0) == 0) return b;
  }
  return std::nullopt;
}

// —— 共享判登路由 ——
// 口径（默认全关＝显式绑定）：
// - 已绑定：目录判登（fail-closed，密码只进 bind 报文）；
// - 未绑定＋账号存在＋auto_bind：目录认证成功即自动绑定
//   （issuer=ldap://host，subject=用户 DN）；
// - 未绑定＋账号不存在＋auto_provision：目录认证成功即建影子账号
//   （本地口令=随机不可知＝LDAP-only）；
// - 其余一律 Local（目录不触达——未配 auto_* 时不做任何探测）。
AuthRoute route_authentication(ServerStore& store, LdapDirectory* ldap,
                               const std::string& account,
                               const std::string& password) {
  AuthRoute r;
  const auto binding = find_ldap_binding(store, account);
  if (!ldap) {
    // 绑了但服务端没起目录面：fail-closed——回落本地＝绕过目录
    if (binding) r.path = AuthRoute::Path::LdapUnavailable;
    return r;
  }
  if (!binding) {
    const bool exists = store.find_account(account).has_value();
    if (!exists && !ldap->config().auto_provision) return r; // Local→账号不存在
    if (exists && !ldap->config().auto_bind) return r;       // Local→本地口令
  }
  const auto v = ldap->verify_user(binding ? binding->subject : account,
                                   password);
  switch (v.outcome) {
  case LdapVerdict::Outcome::Ok: {
    if (!binding) {
      // 首登落账/落绑定（identity_bind：账号须存在、issuer+subject 唯一
      // ——已绑到他人＝冲突，fail-closed 拒登）
      if (!store.find_account(account).has_value()) {
        // 影子账号：口令随机不可知（本地判登实际封死＝LDAP-only）；
        // display_name 暂用登录名（目录显示属性读取留后续）
        if (!store.create_account(account, random_salt_hex(), account)) {
          r.path = AuthRoute::Path::LdapUnavailable;
          return r;
        }
      }
      if (store.identity_bind(account, "ldap://" + ldap->config().host,
                              v.user_dn, now_ms()) == 0) {
        r.path = AuthRoute::Path::LdapUnavailable;
        return r;
      }
    }
    r.path = AuthRoute::Path::LdapOk;
    r.roles = ldap->map_roles(v.groups);
    break;
  }
  case LdapVerdict::Outcome::BadCredentials:
    r.path = AuthRoute::Path::LdapBad;
    break;
  case LdapVerdict::Outcome::Disabled:
    r.path = AuthRoute::Path::LdapDisabled;
    break;
  case LdapVerdict::Outcome::Unavailable:
  case LdapVerdict::Outcome::NotConfigured: // 配置半齐＝不可用，不绕
  default:
    r.path = AuthRoute::Path::LdapUnavailable;
    break;
  }
  return r;
}

// —— 组→角色落库（幂等：在窗授权已存在则跳过）——
int sync_ldap_roles(ServerStore& store, const std::string& account,
                    const std::vector<std::string>& roles,
                    std::int64_t now_ms) {
  if (roles.empty()) return 0;
  const auto existing = store.role_assignments(account);
  int granted = 0;
  for (const auto& role : roles) {
    bool active = false;
    for (const auto& a : existing) {
      if (a.role != role || !a.scope.empty()) continue;
      if ((a.valid_from_ms == 0 || a.valid_from_ms <= now_ms) &&
          (a.valid_until_ms == 0 || now_ms < a.valid_until_ms)) {
        active = true;
        break;
      }
    }
    if (active) continue;
    // 永久授权（until=0）；granted_by 可查来源。角色来自显式映射表——
    // 表里不配 admin 就永不授管理员（「不自动授管理员」红线）。
    if (store.role_grant(account, role, "", now_ms, 0, "ldap-connector",
                         now_ms) > 0)
      ++granted;
  }
  return granted;
}

std::vector<std::string> LdapDirectory::map_roles(
    const std::vector<std::string>& groups) const {
  std::vector<std::string> roles;
  auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  for (const auto& g : groups) {
    const std::string gl = lower(g);
    for (const auto& [group, role] : cfg_.group_roles) {
      if (lower(group) == gl &&
          std::find(roles.begin(), roles.end(), role) == roles.end())
        roles.push_back(role);
    }
  }
  return roles;
}

} // namespace memex::server
