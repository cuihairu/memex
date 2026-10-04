import Foundation

/// 服务器地址（R17 初始化向导第一步的输入域）。
///
/// 接受「host」「host:port」「[IPv6]:port」三种形态；也容忍用户粘贴
/// 「http://host:port/」这类带前缀的完整地址（剥掉 scheme 与路径）。
/// 端口缺省取服务端默认监听 24360（server/src/main.cpp kDefaultPort）。
public struct ServerAddress: Equatable {
    public let host: String
    public let port: Int

    public static let defaultPort = 24360
    static let portMax = 65535

    public init(host: String, port: Int) {
        precondition((1...ServerAddress.portMax).contains(port), "端口越界：\(port)")
        self.host = host
        self.port = port
    }

    /// 展示形态：IPv6 加方括号，其余 host:port
    public func display() -> String {
        host.contains(":") ? "[\(host)]:\(port)" : "\(host):\(port)"
    }

    public enum Reason: Equatable {
        case empty, badHost, badPort, scheme, ipv6Form
    }

    public enum Parsed: Equatable {
        case ok(ServerAddress)
        case err(Reason)
    }

    // 主机名/IPv4：字母数字开头结尾，中间允许点、横线、下划线
    // （不用 /…/ 正则字面量：部分工具链按除号解析，字符串构造无歧义）
    private static let hostRegex = try! Regex(
        "^[A-Za-z0-9_](?:[A-Za-z0-9._-]*[A-Za-z0-9_])?$"
    )
    // 方括号内裸 IPv6：十六进制与冒号
    private static let ipv6Regex = try! Regex("^[0-9A-Fa-f:.]+$")

    public static func parse(_ raw: String) -> Parsed {
        var s = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if s.isEmpty { return .err(.empty) }

        if let schemeAt = s.range(of: "://") {
            let scheme = s[..<schemeAt.lowerBound].lowercased()
            // 只认 http（用户粘贴习惯）；其余前缀（ftp:// 等）明确报错
            if scheme != "http" { return .err(.scheme) }
            s = String(s[schemeAt.upperBound...])
        }
        if let cut = s.firstIndex(where: { $0 == "/" || $0 == "?" || $0 == "#" }) {
            s = String(s[..<cut])
        }
        if s.isEmpty { return .err(.empty) }

        let host: String
        var portStr: String? = nil
        if s.hasPrefix("[") {
            // [IPv6] 或 [IPv6]:port
            guard let close = s.firstIndex(of: "]") else { return .err(.ipv6Form) }
            host = String(s[s.index(after: s.startIndex)..<close])
            let rest = s[s.index(after: close)...]
            if !rest.isEmpty {
                if !rest.hasPrefix(":") || rest.count == 1 { return .err(.ipv6Form) }
                portStr = String(rest.dropFirst())
            }
            if host.wholeMatch(of: ipv6Regex) == nil { return .err(.badHost) }
        } else {
            let colon = s.lastIndex(of: ":")
            if let colon {
                // 多于一个冒号＝裸 IPv6 没包方括号
                if s.firstIndex(of: ":") != colon { return .err(.ipv6Form) }
                host = String(s[..<colon])
                portStr = String(s[s.index(after: colon)...])
            } else {
                host = s
            }
            if host.isEmpty || host.wholeMatch(of: hostRegex) == nil {
                return .err(.badHost)
            }
        }

        let port: Int
        switch portStr {
        case nil:
            port = ServerAddress.defaultPort
        case let p?:
            guard let parsed = Int(p), (1...ServerAddress.portMax).contains(parsed) else {
                return .err(.badPort)
            }
            port = parsed
        }
        return .ok(ServerAddress(host: host, port: port))
    }
}