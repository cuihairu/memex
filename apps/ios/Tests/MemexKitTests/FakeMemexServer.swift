import Foundation
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif
@testable import MemexKit

/// 假 Memex 服务端（loopback TCP，线格式与 server 端一致）：登录应答、
/// 记录收发帧、支持测试主动注入帧。对齐 Android ChatSessionTest 的
/// FakeMemexServer 模式。
final class FakeMemexServer {
    let port: Int
    private let listenFd: Int32
    private let lock = NSLock()
    private(set) var received: [Memex_Protocol_V1_Envelope] = []
    private(set) var sent: [Memex_Protocol_V1_Envelope] = []
    private(set) var connections: [Int32] = []
    private var running = true
    /// 每帧应答钩子（nil=默认：LOGIN→LOGIN_RESULT ok）
    var onEnvelope: ((Memex_Protocol_V1_Envelope, Int32) -> Void)?

    init() throws {
        // 全程用局部 fd：init 内 withUnsafePointer 闭包若引用成员会在
        // 全部成员初始化完成前捕获 self（Swift 不允许）
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else {
            throw NSError(domain: "fake-server", code: 1, userInfo: [NSLocalizedDescriptionKey: "socket 失败"])
        }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        addr.sin_port = 0
        let bindRC = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bindRC == 0, listen(fd, 8) == 0 else {
            close(fd)
            throw NSError(domain: "fake-server", code: 2, userInfo: [NSLocalizedDescriptionKey: "bind/listen 失败"])
        }
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        withUnsafeMutablePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                getsockname(fd, $0, &len)
            }
        }
        listenFd = fd
        port = Int(addr.sin_port.bigEndian)
        start()
    }

    func start() {
        let t = Thread { [weak self] in self?.acceptLoop() }
        t.name = "fake-memex"
        t.start()
    }

    private func acceptLoop() {
        while running {
            let c = accept(listenFd, nil, nil)
            if c < 0 { break }
            lock.lock()
            connections.append(c)
            lock.unlock()
            Thread { [weak self] in self?.serve(c) }.start()
        }
    }

    private func serve(_ c: Int32) {
        var decoder = FrameCodec.Decoder()
        var buf = [UInt8](repeating: 0, count: 16 * 1024)
        while running {
            let n = buf.withUnsafeMutableBytes { raw in read(c, raw.baseAddress, raw.count) }
            if n <= 0 { break }
            let res = decoder.feed(Array(buf[0..<n]))
            for frame in res.frames {
                guard let env = try? Memex_Protocol_V1_Envelope(serializedData: Data(frame)) else { continue }
                lock.lock()
                received.append(env)
                lock.unlock()
                respond(env, conn: c)
            }
        }
    }

    private func respond(_ env: Memex_Protocol_V1_Envelope, conn: Int32) {
        if let handler = onEnvelope {
            handler(env, conn)
            return
        }
        if env.type == .login {
            send(conn, loginOk(to: env.login.account))
        }
    }

    func loginOk(to: String, ok: Bool = true, reason: String = "") -> Memex_Protocol_V1_Envelope {
        Memex_Protocol_V1_Envelope.with { e in
            e.type = .loginResult
            e.seq = 1
            e.from = "server"
            e.to = to
            e.tsMs = Int64(Date().timeIntervalSince1970 * 1000)
            e.loginResult = Memex_Protocol_V1_LoginResult.with { r in
                r.ok = ok
                r.reason = reason
                r.displayName = ok ? "张三" : ""
            }
        }
    }

    func send(_ conn: Int32, _ env: Memex_Protocol_V1_Envelope) {
        lock.lock()
        sent.append(env)
        lock.unlock()
        guard let payload = try? env.serializedData(),
              let frame = try? FrameCodec.encode(Array(payload))
        else { return }
        var written = 0
        while written < frame.count {
            let n = frame.withUnsafeBytes { raw in
                write(conn, raw.baseAddress!.advanced(by: written), frame.count - written)
            }
            if n <= 0 { break }
            written += n
        }
    }

    func close() {
        running = false
        #if canImport(Darwin)
        Darwin.close(listenFd)
        #else
        Glibc.close(listenFd)
        #endif
    }

    /// 当前存活连接（用于主动注入帧）
    func lastConnection() -> Int32? {
        lock.lock()
        defer { lock.unlock() }
        return connections.last
    }
}