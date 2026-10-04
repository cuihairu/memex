/**
 * 服务器地址（R17 初始化向导第一步的输入域）。
 * 语义镜像 apps/android ServerAddress.kt：
 * 接受「host」「host:port」「[IPv6]:port」三种形态；容忍「http://host:port/」
 * 带前缀粘贴；端口缺省取服务端默认监听 24360。非法输入返回原因（不抛）。
 */

export const DEFAULT_PORT = 24360;
export const PORT_MAX = 65535;

// 主机名/IPv4：字母数字开头结尾，中间允许点、横线、下划线
const HOST_RE = /^[A-Za-z0-9_](?:[A-Za-z0-9._-]*[A-Za-z0-9_])?$/;
// 方括号内裸 IPv6：十六进制与冒号
const IPV6_RE = /^[0-9A-Fa-f:.]+$/;

export type ParseReason = 'EMPTY' | 'BAD_HOST' | 'BAD_PORT' | 'SCHEME' | 'IPV6_FORM';

export type ParseResult =
  | { ok: true; address: ServerAddress }
  | { ok: false; reason: ParseReason };

export class ServerAddress {
  readonly host: string;
  readonly port: number;

  constructor(host: string, port: number) {
    if (port < 1 || port > PORT_MAX) throw new Error('端口越界：' + port);
    this.host = host;
    this.port = port;
  }

  /** 展示形态：IPv6 加方括号，其余 host:port。 */
  display(): string {
    return this.host.indexOf(':') >= 0 ? '[' + this.host + ']:' + this.port : this.host + ':' + this.port;
  }

  static parse(raw: string): ParseResult {
    let s = raw.trim();
    if (s.length === 0) return { ok: false, reason: 'EMPTY' };

    const schemeAt = s.indexOf('://');
    if (schemeAt >= 0) {
      const scheme = s.substring(0, schemeAt).toLowerCase();
      // 只认 http（用户粘贴习惯）；其余前缀（ftp:// 等）明确报错
      if (scheme !== 'http') return { ok: false, reason: 'SCHEME' };
      s = s.substring(schemeAt + 3);
    }
    let cut = s.length;
    for (let i = 0; i < s.length; i++) {
      const ch = s.charAt(i);
      if (ch === '/' || ch === '?' || ch === '#') {
        cut = i;
        break;
      }
    }
    if (cut >= 0) s = s.substring(0, cut);
    if (s.length === 0) return { ok: false, reason: 'EMPTY' };

    let host: string;
    let portStr: string | null = null;
    if (s.charAt(0) === '[') {
      // [IPv6] 或 [IPv6]:port
      const close = s.indexOf(']');
      if (close <= 1) return { ok: false, reason: 'IPV6_FORM' };
      host = s.substring(1, close);
      const rest = s.substring(close + 1);
      if (rest.length > 0) {
        if (rest.charAt(0) !== ':' || rest.length === 1) {
          return { ok: false, reason: 'IPV6_FORM' };
        }
        portStr = rest.substring(1);
      }
      if (!IPV6_RE.test(host)) return { ok: false, reason: 'BAD_HOST' };
    } else {
      const firstColon = s.indexOf(':');
      const lastColon = s.lastIndexOf(':');
      if (lastColon >= 0) {
        // 多于一个冒号＝裸 IPv6 没包方括号
        if (firstColon !== lastColon) return { ok: false, reason: 'IPV6_FORM' };
        host = s.substring(0, lastColon);
        portStr = s.substring(lastColon + 1);
      } else {
        host = s;
      }
      if (host.length === 0 || !HOST_RE.test(host)) {
        return { ok: false, reason: 'BAD_HOST' };
      }
    }

    let port = DEFAULT_PORT;
    if (portStr !== null) {
      const n = Number(portStr);
      if (!/^\d+$/.test(portStr) || n < 1 || n > PORT_MAX) {
        return { ok: false, reason: 'BAD_PORT' };
      }
      port = n;
    }
    return { ok: true, address: new ServerAddress(host, port) };
  }
}