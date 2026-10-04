import Foundation

/// 连通性校验结果（R17：校验通过才允许保存并放行）
public enum ProbeResult: Equatable {
    /// 握手成功：PING→PONG，附往返时延
    case ok(rttMs: Int64)
    /// 连不上（拒绝/不可达/域名解析失败）
    case unreachable(detail: String)
    /// 连上了但时限内无应答
    case timeout(detail: String)
    /// 有应答但不是 Memex 协议（类型不符/帧非法/载荷解析失败）
    case notMemex(detail: String)
}

/// 登录结果
public enum LoginOutcome: Equatable {
    case success(displayName: String)
    /// 服务端明确拒绝（LOGIN_RESULT.ok=false，含失败原因）
    case rejected(reason: String)
    case unreachable(detail: String)
    case timeout(detail: String)
    case notMemex(detail: String)
}

/// 协作态客户端（阻塞式；UI 层放后台线程调用，单测直接在测试线程跑）。
///
/// 协议：帧＝4 字节大端长度前缀 + Envelope（memex.proto 单一事实源）。
/// 连通性校验＝PING/PONG（服务端对未登录连接同样应答，server/src/session.cpp）。
public final class MemexClient {
    private let connectTimeoutMs: Int
    private let readTimeoutMs: Int

    public init(connectTimeoutMs: Int = 5_000, readTimeoutMs: Int = 5_000) {
        self.connectTimeoutMs = connectTimeoutMs
        self.readTimeoutMs = readTimeoutMs
    }

    private var nowMs: Int64 {
        Int64(Date().timeIntervalSince1970 * 1000)
    }

    public func probe(address: ServerAddress) -> ProbeResult {
        let start = DispatchTime.now()
        do {
            let wire = try Wire(
                host: address.host, port: address.port,
                connectTimeoutMs: connectTimeoutMs, readTimeoutMs: readTimeoutMs
            )
            defer { wire.close() }
            try wire.send(MemexProto_Envelope.with { e in
                e.type = .ping
                e.seq = 1
                e.from = "mobile-setup"
                e.to = "server"
                e.tsMs = nowMs
            })
            let reply = try wire.readEnvelope()
            if reply.type == .pong {
                let rtt = Int64(DispatchTime.now().uptimeNanoseconds - start.uptimeNanoseconds) / 1_000_000
                return .ok(rttMs: rtt)
            }
            return .notMemex(detail: "期望 PONG，收到 \(reply.type)")
        } catch {
            return classify(error: error)
        }
    }

    public func login(
        address: ServerAddress,
        account: String,
        password: String,
        deviceFingerprint: String,
        deviceName: String,
        clientVersion: String
    ) -> LoginOutcome {
        do {
            let wire = try Wire(
                host: address.host, port: address.port,
                connectTimeoutMs: connectTimeoutMs, readTimeoutMs: readTimeoutMs
            )
            defer { wire.close() }
            try wire.send(MemexProto_Envelope.with { e in
                e.type = .login
                e.seq = 1
                e.from = account
                e.to = "server"
                e.tsMs = nowMs
                e.login = MemexProto_Login.with { l in
                    l.account = account
                    l.password = password
                    l.deviceFingerprint = deviceFingerprint
                    // 服务端口径（server.cpp kind_name）："mobile"＝移动端，
                    // 与桌面端按类型互踢、跨类型并存（T2.1）
                    l.deviceKind = "mobile"
                    l.deviceName = deviceName
                    l.clientVersion = clientVersion
                }
            })
            // 登录广播（PRESENCE_DATA）可能先于 LOGIN_RESULT 到达，跳过直到目标帧
            let deadlineMs = nowMs + Int64(readTimeoutMs)
            while true {
                let reply = try wire.readEnvelope()
                if reply.type == .loginResult {
                    guard let r = reply.loginResult else {
                        return .notMemex(detail: "LOGIN_RESULT 缺载荷")
                    }
                    if r.ok {
                        return .success(displayName: r.displayName.isEmpty ? account : r.displayName)
                    }
                    return .rejected(reason: r.reason.isEmpty ? "登录被拒绝" : r.reason)
                }
                if nowMs > deadlineMs {
                    return .timeout(detail: "等待 LOGIN_RESULT 超时")
                }
            }
        } catch {
            return classify(error: error)
        }
    }

    private func classify(error: Error) -> ProbeResult {
        switch error {
        case let e as SocketError:
            switch e {
            case .dnsFailed, .connectFailed, .ioError:
                return .unreachable(detail: message(e))
            case .timedOut:
                return .timeout(detail: "timeout")
            case .closedByPeer:
                return .notMemex(detail: "对端在应答前关闭连接")
            }
        case let e as FrameCodec.ProtocolViolation:
            return .notMemex(detail: e.message)
        default:
            return .unreachable(detail: message(error))
        }
    }

    private func message(_ e: Error) -> String {
        if let err = e as? CustomStringConvertible { return err.description }
        return String(describing: e)
    }
}