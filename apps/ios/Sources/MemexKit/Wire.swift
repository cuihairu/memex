import Foundation
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif

/// Socket 层错误：连接阶段与协议阶段分开表达，便于上层归类。
enum SocketError: Error {
    case dnsFailed(String)
    case connectFailed(String)
    case timedOut
    case closedByPeer
    case ioError(String)
}

/// 单条 TCP 连接：帧编解码 + 收发 Envelope（对齐 Android 端 Wire 语义）。
/// 读超时：readTimeoutMs <= 0 表示无限阻塞（长连接空闲不断）；写为阻塞写。
final class Wire {
    private let fd: Int32
    private let readTimeoutMs: Int
    private var decoder = FrameCodec.Decoder()
    private var pending: [[UInt8]] = []
    private let sendLock = NSLock()

    init(host: String, port: Int, connectTimeoutMs: Int, readTimeoutMs: Int) throws {
        self.readTimeoutMs = readTimeoutMs
        var hints = addrinfo()
        hints.ai_family = AF_UNSPEC
        hints.ai_socktype = SOCK_STREAM
        var res: UnsafeMutablePointer<addrinfo>?
        let rc = getaddrinfo(host, String(port), &hints, &res)
        guard rc == 0, let addr = res else {
            throw SocketError.dnsFailed(String(cString: gai_strerror(rc)))
        }
        defer { freeaddrinfo(res) }
        let s = socket(addr.pointee.ai_family, addr.pointee.ai_socktype, addr.pointee.ai_protocol)
        guard s >= 0 else {
            throw SocketError.ioError("socket 创建失败：\(String(cString: strerror(errno)))")
        }
        do {
            try Self.connectWithTimeout(fd: s, addr: addr.pointee, timeoutMs: connectTimeoutMs)
        } catch {
            Self.closeFD(s)
            throw error
        }
        fd = s
        var one: Int32 = 1
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size))
    }

    deinit {
        Self.closeFD(fd)
    }

    /// 显式关闭（幂等：重复关闭对已关闭 fd 只返回错误，无副作用）。
    func close() {
        Self.closeFD(fd)
    }

    func send(_ message: Memex_Protocol_V1_Envelope) throws {
        let payload = try message.serializedData()
        let frame = try FrameCodec.encode(Array(payload))
        sendLock.lock()
        defer { sendLock.unlock() }
        var written = 0
        while written < frame.count {
            let n = frame.withUnsafeBytes { buf -> Int in
                write(fd, buf.baseAddress!.advanced(by: written), frame.count - written)
            }
            if n < 0 {
                if errno == EINTR { continue }
                throw SocketError.ioError("写失败：\(String(cString: strerror(errno)))")
            }
            if n == 0 { throw SocketError.closedByPeer }
            written += n
        }
    }

    /// 读一帧并解析；超时抛 timedOut，对端关闭抛 closedByPeer，
    /// 帧/载荷非法抛 ProtocolViolation。
    func readEnvelope() throws -> Memex_Protocol_V1_Envelope {
        while pending.isEmpty {
            if readTimeoutMs > 0 {
                try waitReadable(timeoutMs: readTimeoutMs)
            }
            var buf = [UInt8](repeating: 0, count: 16 * 1024)
            let n = buf.withUnsafeMutableBytes { raw -> Int in
                read(fd, raw.baseAddress, raw.count)
            }
            if n < 0 {
                if errno == EINTR { continue }
                throw SocketError.ioError("读失败：\(String(cString: strerror(errno)))")
            }
            if n == 0 { throw SocketError.closedByPeer }
            let result = decoder.feed(Array(buf[0..<n]))
            if result.status == .zeroLength || result.status == .tooLarge {
                throw FrameCodec.ProtocolViolation("非法帧：\(result.status)")
            }
            pending.append(contentsOf: result.frames)
        }
        return try Memex_Protocol_V1_Envelope(serializedData: Data(pending.removeFirst()))
    }

    /// poll 等可读（select 的 fd_set/FD_ZERO/FD_SET 在 Darwin Swift 侧不暴露，poll 两平台通吃）
    private func waitReadable(timeoutMs: Int) throws {
        var pfd = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
        let r = poll(&pfd, 1, Int32(timeoutMs))
        if r < 0 {
            if errno == EINTR { return try waitReadable(timeoutMs: timeoutMs) } // EINTR 未消耗时间预算
            throw SocketError.ioError("poll 失败：\(String(cString: strerror(errno)))")
        }
        if r == 0 { throw SocketError.timedOut }
    }

    /// 非阻塞 connect + poll 等可写（POSIX 平台通用）。
    private static func connectWithTimeout(
        fd: Int32, addr: addrinfo, timeoutMs: Int
    ) throws {
        let flags = fcntl(fd, F_GETFL, 0)
        _ = fcntl(fd, F_SETFL, flags | O_NONBLOCK)
        defer { _ = fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) }
        let rc = addr.ai_addr.withMemoryRebound(to: sockaddr.self, capacity: 1) { ptr in
            connect(fd, ptr, addr.ai_addrlen)
        }
        if rc != 0 {
            if errno != EINPROGRESS {
                throw SocketError.connectFailed(strerrorText(errno))
            }
            var pfd = pollfd(fd: fd, events: Int16(POLLOUT), revents: 0)
            let s = poll(&pfd, 1, Int32(timeoutMs))
            if s < 0 {
                throw SocketError.connectFailed(strerrorText(errno))
            }
            if s == 0 { throw SocketError.timedOut }
            var err: Int32 = 0
            var len = socklen_t(MemoryLayout<Int32>.size)
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len)
            if err != 0 { throw SocketError.connectFailed(strerrorText(err)) }
        }
    }

    private static func strerrorText(_ err: Int32) -> String {
        String(cString: strerror(err))
    }

    private static func closeFD(_ s: Int32) {
        #if canImport(Darwin)
        Darwin.close(s)
        #else
        Glibc.close(s)
        #endif
    }
}