import Foundation

/// 会话事件回调（在注入的 eventQueue 上投递；UI 侧传主队列刷新界面）。
public protocol ChatSessionListener: AnyObject {
    /// 收到一条新消息（已落库；msgId 空=自己刚发的受理暂存）
    func onMessage(peer: String, msgId: String, mine: Bool)

    /// 发送受理回执（服务端 ACK(seq)）
    func onSent(seq: UInt64)

    /// 连接断开（未主动 close）——上层决定重连
    func onDisconnected(cause: String)

    /// 被服务端互踢（KICK）
    func onKicked(reason: String)
}

/// 协作态长连接会话（T6.4 会话列表与收发）。
///
/// 对齐桌面端 CollabEngine 语义（collab_engine.cpp）与 Android 端实现：
/// - 登录后保持单条 TCP 连接；后台读线程分帧分发；
/// - sendText：本端 seq 内存自增，帧 = Envelope{TEXT, seq, from, to, ts_ms, text}；
/// - 收到 TEXT：按 peer 落库（群消息 peer=to 的 "group:N"，单聊 peer=from），
///   若带 msg_id 回 ACK(msg_id)（服务端按 msg_id 清离线队列，重复投递由本地
///   msg_id 去重）；自己发的消息本地立即落库（msg_id 为空，等受理回执）；
/// - ACK(seq)：发送方受理回执 → onSent；ACK(msg_id) 为接收方已收取，仅清理
///   本地 pending（移动端首块不维护 inflight 重传，重传随断线重连块）；
/// - KICK：服务端单点互踢 → 收到即断开并回调 onKicked；
/// - 断线：onDisconnected 回调，由上层决定重连（首块不做自动重连）。
///
/// 线程模型：单连接后台读线程；发送方为调用线程（UI 外部封装）；回调全部投递
/// 到构造时传入的 eventQueue。
public final class ChatSession {
    public enum ConnectOutcome: Equatable {
        case ok(rttMs: Int64)
        case rejected(reason: String)
        case unreachable(detail: String)
        case timeout(detail: String)
        case notMemex(detail: String)
    }

    private let store: ChatStore
    private let account: String
    private let displayName: String
    private let eventQueue: DispatchQueue
    private let lock = NSLock()
    private var wire: Wire?
    private var closed = false
    private var seqGen: UInt64 = 1
    private weak var listener: ChatSessionListener?
    private var readThread: Thread?

    public init(
        store: ChatStore,
        account: String,
        displayName: String,
        eventQueue: DispatchQueue = DispatchQueue(label: "com.memex.events")
    ) {
        self.store = store
        self.account = account
        self.displayName = displayName
        self.eventQueue = eventQueue
    }

    private var nowMs: Int64 {
        Int64(Date().timeIntervalSince1970 * 1000)
    }

    /// 同步登录并起读循环。成功返回 .ok；失败返回对应结果且连接已关闭。
    /// 单测可直接在测试线程调用；UI 侧放后台线程。
    public func connect(
        address: ServerAddress,
        password: String,
        deviceFingerprint: String,
        deviceName: String,
        clientVersion: String,
        listener: ChatSessionListener
    ) -> ConnectOutcome {
        lock.lock()
        self.listener = listener
        lock.unlock()
        let start = DispatchTime.now()
        do {
            let w = try Wire(
                host: address.host, port: address.port,
                connectTimeoutMs: 8_000, readTimeoutMs: 0
            )
            lock.lock()
            wire = w
            lock.unlock()
            try w.send(MemexProto_Envelope.with { e in
                e.type = .login
                e.seq = 1
                e.from = account
                e.to = "server"
                e.tsMs = nowMs
                e.login = MemexProto_Login.with { l in
                    l.account = account
                    l.password = password
                    l.deviceFingerprint = deviceFingerprint
                    l.deviceKind = "mobile"
                    l.deviceName = deviceName
                    l.clientVersion = clientVersion
                }
            })
            // 登录广播可能先于 LOGIN_RESULT 到达（PRESENCE_DATA 等）
            while true {
                let reply = try w.readEnvelope()
                if reply.type == .loginResult {
                    guard let r = reply.loginResult else {
                        safeClose()
                        return .notMemex(detail: "LOGIN_RESULT 缺载荷")
                    }
                    if !r.ok {
                        safeClose()
                        return .rejected(reason: r.reason.isEmpty ? "登录被拒绝" : r.reason)
                    }
                    break
                }
            }
            closed = false
            let thread = Thread { [weak self] in
                self?.readLoop(w)
            }
            thread.name = "memex-chat-read"
            readThread = thread
            thread.start()
            let rtt = Int64(DispatchTime.now().uptimeNanoseconds - start.uptimeNanoseconds) / 1_000_000
            return .ok(rttMs: rtt)
        } catch {
            safeClose()
            return classify(error: error)
        }
    }

    /// 发送文本。返回分配的 seq（0=未连接/已关闭/空对象）。
    @discardableResult
    public func sendText(to: String, text: String) -> UInt64 {
        lock.lock()
        guard let w = wire, !closed, !to.isEmpty else {
            lock.unlock()
            return 0
        }
        let seq = nextSeqLocked()
        lock.unlock()
        let ts = nowMs
        do {
            try w.send(MemexProto_Envelope.with { e in
                e.type = .text
                e.seq = seq
                e.from = account
                e.to = to
                e.tsMs = ts
                e.text = MemexProto_Text.with { $0.text = text }
            })
        } catch {
            notifyDisconnect(cause: "发送失败")
            return 0
        }
        // 本地立即落库（自己发的消息；msg_id 空，服务端受理后才有——对齐桌面）
        store.append(
            StoredMessage(
                id: 0, peer: to, from: account, to: to, seq: seq,
                tsMs: ts, text: text, source: "collab", msgId: "", recalled: false
            )
        )
        dispatch { [weak self] in
            self?.listener?.onMessage(peer: to, msgId: "", mine: true)
        }
        return seq
    }

    /// 主动登出并断开（LOGOUT 后由服务端关连接，无需等待）
    public func logout() {
        lock.lock()
        let w = wire
        let wasClosed = closed
        lock.unlock()
        if let w, !wasClosed {
            try? w.send(MemexProto_Envelope.with { e in
                e.type = .logout
                e.seq = nextSeq()
                e.from = account
                e.to = "server"
                e.tsMs = nowMs
            })
        }
        safeClose()
    }

    public func close() {
        safeClose()
    }

    // MARK: - 读循环与分发

    private func readLoop(_ w: Wire) {
        while true {
            do {
                let env = try w.readEnvelope()
                switch env.type {
                case .text:
                    onIncomingText(env)
                case .ack:
                    onAck(env)
                case .kick:
                    let reason: String
                    if let kick = env.kick, !kick.reason.isEmpty {
                        reason = kick.reason
                    } else {
                        reason = "账号已在其他设备登录"
                    }
                    dispatch { [weak self] in
                        self?.listener?.onKicked(reason: reason)
                    }
                    safeClose()
                    return
                case .pong:
                    break
                default:
                    break // 组织/群/已读等后续块处理
                }
            } catch {
                lock.lock()
                let wasClosed = closed
                lock.unlock()
                if !wasClosed { notifyDisconnect(cause: "连接中断") }
                return
            }
        }
    }

    private func onIncomingText(_ env: MemexProto_Envelope) {
        guard let text = env.text else { return }
        // 群消息 peer=to 的 "group:N"；单聊 peer=from（对齐桌面 collab_engine）
        let peer = env.to.hasPrefix("group:") ? env.to : env.from
        let inserted = store.append(
            StoredMessage(
                id: 0, peer: peer, from: env.from, to: env.to, seq: env.seq,
                tsMs: env.tsMs, text: text.text, source: "collab",
                msgId: env.msgID, recalled: false
            )
        )
        // 已收取回执（msg_id 非空即回；离线补投去重后照回 ACK——服务端按
        // 账号清离线队列，重复 ACK 无害）
        if !env.msgID.isEmpty, let w = wire {
            try? w.send(MemexProto_Envelope.with { e in
                e.type = .ack
                e.seq = nextSeq()
                e.from = account
                e.to = "server"
                e.tsMs = nowMs
                e.ack = MemexProto_Ack.with { $0.msgID = env.msgID }
            })
        }
        if inserted {
            dispatch { [weak self] in
                self?.listener?.onMessage(peer: peer, msgId: env.msgID, mine: false)
            }
        }
    }

    private func onAck(_ env: MemexProto_Envelope) {
        guard env.seq != 0 else { return }
        dispatch { [weak self] in
            self?.listener?.onSent(seq: env.seq)
        }
    }

    private func notifyDisconnect(cause: String) {
        safeClose()
        dispatch { [weak self] in
            self?.listener?.onDisconnected(cause: cause)
        }
    }

    private func dispatch(_ block: @escaping () -> Void) {
        eventQueue.async(execute: block)
    }

    private func safeClose() {
        lock.lock()
        defer { lock.unlock() }
        guard !closed else { return }
        closed = true
        wire?.close()
        wire = nil
    }

    private func nextSeq() -> UInt64 {
        lock.lock()
        defer { lock.unlock() }
        return nextSeqLocked()
    }

    /// 调用方必须已持有 lock
    private func nextSeqLocked() -> UInt64 {
        let s = seqGen
        seqGen += 1
        return s
    }

    private func classify(error: Error) -> ConnectOutcome {
        switch error {
        case let e as SocketError:
            switch e {
            case .dnsFailed, .connectFailed, .ioError:
                return .unreachable(detail: describe(e))
            case .timedOut:
                return .timeout(detail: "timeout")
            case .closedByPeer:
                return .notMemex(detail: "对端在应答前关闭连接")
            }
        case let e as FrameCodec.ProtocolViolation:
            return .notMemex(detail: e.message)
        default:
            return .notMemex(detail: "异常：\(describe(error))")
        }
    }

    private func describe(_ e: Error) -> String {
        if let c = e as? CustomStringConvertible { return c.description }
        return String(describing: e)
    }
}