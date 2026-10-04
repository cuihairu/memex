import Foundation
import XCTest
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif
@testable import MemexKit

/// T6.4 长连接会话：线格式与分发语义（真 socket＋假服务端，对齐 Android ChatSessionTest）
final class ChatSessionTests: XCTestCase {

    // MARK: - 回调记录

    private final class RecordingListener: ChatSessionListener {
        private let lock = NSLock()
        private var _messages: [(peer: String, msgId: String, mine: Bool)] = []
        private var _sent: [UInt64] = []
        private var _kicked: [String] = []
        private var _disconnected: [String] = []
        let onMessage = DispatchSemaphore(value: 0)
        let onSent = DispatchSemaphore(value: 0)
        let onKicked = DispatchSemaphore(value: 0)

        var messages: [(peer: String, msgId: String, mine: Bool)] {
            lock.lock(); defer { lock.unlock() }; return _messages
        }
        var sentSeqs: [UInt64] {
            lock.lock(); defer { lock.unlock() }; return _sent
        }
        var kicked: [String] {
            lock.lock(); defer { lock.unlock() }; return _kicked
        }
        var disconnected: [String] {
            lock.lock(); defer { lock.unlock() }; return _disconnected
        }

        func onMessage(peer: String, msgId: String, mine: Bool) {
            lock.lock(); _messages.append((peer, msgId, mine)); lock.unlock()
            onMessage.signal()
        }

        func onSent(seq: UInt64) {
            lock.lock(); _sent.append(seq); lock.unlock()
            onSent.signal()
        }

        func onDisconnected(cause: String) {
            lock.lock(); _disconnected.append(cause); lock.unlock()
        }

        func onKicked(reason: String) {
            lock.lock(); _kicked.append(reason); lock.unlock()
            onKicked.signal()
        }
    }

    // MARK: - 测试脚手架

    private func connect(
        _ server: FakeMemexServer,
        store: ChatStore = InMemoryChatStore(),
        listener: ChatSessionListener = RecordingListener()
    ) -> (ChatSession, ChatSession.ConnectOutcome) {
        let session = ChatSession(store: store, account: "alice", displayName: "Alice")
        let outcome = session.connect(
            address: ServerAddress(host: "127.0.0.1", port: server.port),
            password: "pw",
            deviceFingerprint: "fp",
            deviceName: "Pixel",
            clientVersion: "0.1.0",
            listener: listener
        )
        return (session, outcome)
    }

    private func wait(
        _ sem: DispatchSemaphore,
        _ file: StaticString = #filePath, _ line: UInt = #line
    ) {
        XCTAssertEqual(
            sem.wait(timeout: .now() + 5), .success,
            "等待超时", file: file, line: line
        )
    }

    /// 有界轮询服务端已收帧（客户端 ACK 在读线程异步回、回调走独立队列，
    /// 事件信号不蕴含服务端已记账，需等一拍避免竞态）
    private func awaitReceived(
        _ server: FakeMemexServer,
        _ predicate: @escaping (Memex_Protocol_V1_Envelope) -> Bool,
        _ file: StaticString = #filePath, _ line: UInt = #line
    ) -> Memex_Protocol_V1_Envelope? {
        let deadline = Date().addingTimeInterval(5)
        while Date() < deadline {
            if let m = server.snapshotReceived().first(where: predicate) { return m }
            Thread.sleep(forTimeInterval: 0.02)
        }
        XCTFail("等待服务端收到匹配帧超时", file: file, line: line)
        return nil
    }

    // MARK: - 用例

    func testLoginSucceedsWithFullLoginFields() throws {
        let server = try FakeMemexServer()
        defer { server.close() }
        let seen = DispatchSemaphore(value: 0)
        var captured: Memex_Protocol_V1_Envelope?
        let captureLock = NSLock()
        server.onEnvelope = { env, c in
            if env.type == .login {
                captureLock.lock(); captured = env; captureLock.unlock()
                seen.signal()
                server.send(c, Memex_Protocol_V1_Envelope.with { e in
                    e.type = .loginResult
                    e.seq = 1
                    e.from = "server"
                    e.to = env.login.account
                    e.tsMs = Int64(Date().timeIntervalSince1970 * 1000)
                    e.loginResult = Memex_Protocol_V1_LoginResult.with { r in
                        r.ok = true
                        r.displayName = "张三"
                    }
                })
            }
        }
        let (session, outcome) = connect(server)
        defer { session.close() }
        wait(seen)
        guard case .ok = outcome else { return XCTFail("期望 .ok，实得 \(outcome)") }
        let login = captureLock.withLock { captured }!
        XCTAssertEqual(login.type, .login)
        XCTAssertEqual(login.login.account, "alice")
        XCTAssertEqual(login.login.deviceKind, "mobile")
    }

    func testLoginRejectedClosesAndKeepsReason() throws {
        let server = try FakeMemexServer()
        defer { server.close() }
        server.onEnvelope = { env, c in
            if env.type == .login {
                server.send(c, Memex_Protocol_V1_Envelope.with { e in
                    e.type = .loginResult
                    e.seq = 1
                    e.from = "server"
                    e.to = env.login.account
                    e.tsMs = Int64(Date().timeIntervalSince1970 * 1000)
                    e.loginResult = Memex_Protocol_V1_LoginResult.with { r in
                        r.ok = false
                        r.reason = "账号不存在"
                    }
                })
            }
        }
        let (_, outcome) = connect(server)
        XCTAssertEqual(.rejected(reason: "账号不存在"), outcome)
    }

    func testTextFrameWireFormatMatchesDesktop() throws {
        let server = try FakeMemexServer()
        defer { server.close() }
        let textSeen = DispatchSemaphore(value: 0)
        var textEnvelope: Memex_Protocol_V1_Envelope?
        let textLock = NSLock()
        server.onEnvelope = { env, c in
            switch env.type {
            case .text:
                textLock.lock(); textEnvelope = env; textLock.unlock()
                textSeen.signal()
                // 服务端受理回执（对齐 session.cpp：ack 带原 seq、to=发送方）
                server.send(c, Memex_Protocol_V1_Envelope.with { e in
                    e.type = .ack
                    e.seq = env.seq
                    e.from = "server"
                    e.to = env.from
                    e.tsMs = Int64(Date().timeIntervalSince1970 * 1000)
                })
            case .login:
                server.send(c, Memex_Protocol_V1_Envelope.with { e in
                    e.type = .loginResult
                    e.seq = 1
                    e.from = "server"
                    e.to = env.login.account
                    e.tsMs = Int64(Date().timeIntervalSince1970 * 1000)
                    e.loginResult = Memex_Protocol_V1_LoginResult.with { r in r.ok = true }
                })
            default:
                break
            }
        }
        let listener = RecordingListener()
        let (session, outcome) = connect(server, listener: listener)
        defer { session.close() }
        guard case .ok = outcome else { return XCTFail("期望 .ok，实得 \(outcome)") }

        let seq = session.sendText(to: "bob", text: "你好")
        XCTAssertTrue(seq > 0, "seq 应为正，实得 \(seq)")
        wait(textSeen)
        wait(listener.onSent)

        let env = textLock.withLock { textEnvelope }!
        XCTAssertEqual(env.type, .text)
        XCTAssertEqual(env.seq, seq)
        XCTAssertEqual(env.from, "alice")
        XCTAssertEqual(env.to, "bob")
        XCTAssertEqual(env.text.text, "你好")
        XCTAssertEqual(listener.sentSeqs, [seq])
    }

    func testReceiveTextAcksAndDedupsDuplicates() throws {
        let server = try FakeMemexServer()
        defer { server.close() }
        let store = InMemoryChatStore()
        let listener = RecordingListener()
        let (session, outcome) = connect(server, store: store, listener: listener)
        defer { session.close() }
        guard case .ok = outcome else { return XCTFail("期望 .ok，实得 \(outcome)") }

        // 带 msg_id 的推送（在线即投 / 离线补投共用同帧）
        let textFrame = Memex_Protocol_V1_Envelope.with { e in
            e.type = .text
            e.seq = 7
            e.from = "bob"
            e.to = "alice"
            e.tsMs = Int64(Date().timeIntervalSince1970 * 1000)
            e.msgID = "sha256:bob:7"
            e.text = Memex_Protocol_V1_Text.with { $0.text = "在吗" }
        }
        let conn = server.lastConnection()!
        server.send(conn, textFrame)
        wait(listener.onMessage)

        // 已回 ACK(msg_id)
        let ackFrame = awaitReceived(server) { $0.type == .ack }
        XCTAssertEqual(ackFrame?.ack.msgID, "sha256:bob:7")
        XCTAssertEqual(ackFrame?.to, "server")

        // 落库且已回调
        XCTAssertEqual(listener.messages.map { "\($0.peer):\($0.msgId)" }, ["bob:sha256:bob:7"])
        let hist = store.history(peer: "bob")
        XCTAssertEqual(hist.count, 1)
        XCTAssertEqual(hist[0].text, "在吗")
        XCTAssertEqual(hist[0].msgId, "sha256:bob:7")

        // 重复补投：去重不重复落库不重复回调
        server.send(conn, textFrame)
        Thread.sleep(forTimeInterval: 0.3)
        XCTAssertEqual(store.history(peer: "bob").count, 1)
        XCTAssertEqual(listener.messages.count, 1)
    }

    func testGroupMessageRoutesToGroupPeer() throws {
        let server = try FakeMemexServer()
        defer { server.close() }
        let store = InMemoryChatStore()
        let listener = RecordingListener()
        let (session, outcome) = connect(server, store: store, listener: listener)
        defer { session.close() }
        guard case .ok = outcome else { return XCTFail("期望 .ok，实得 \(outcome)") }

        let conn = server.lastConnection()!
        server.send(conn, Memex_Protocol_V1_Envelope.with { e in
            e.type = .text
            e.seq = 3
            e.from = "bob"
            e.to = "group:9"
            e.tsMs = Int64(Date().timeIntervalSince1970 * 1000)
            e.msgID = "g1"
            e.text = Memex_Protocol_V1_Text.with { $0.text = "群消息" }
        })
        wait(listener.onMessage)
        XCTAssertEqual(store.history(peer: "group:9").map { $0.peer }, ["group:9"])
        XCTAssertEqual(store.history(peer: "group:9").map { $0.text }, ["群消息"])
        XCTAssertEqual(awaitReceived(server) { $0.type == .ack }?.ack.msgID, "g1")
    }

    func testKickDisconnectsAndStopsSends() throws {
        let server = try FakeMemexServer()
        defer { server.close() }
        let listener = RecordingListener()
        let (session, outcome) = connect(server, listener: listener)
        guard case .ok = outcome else { return XCTFail("期望 .ok，实得 \(outcome)") }

        let conn = server.lastConnection()!
        server.send(conn, Memex_Protocol_V1_Envelope.with { e in
            e.type = .kick
            e.seq = 2
            e.from = "server"
            e.to = "alice"
            e.tsMs = Int64(Date().timeIntervalSince1970 * 1000)
            e.kick = Memex_Protocol_V1_Kick.with { k in
                k.reason = "账号已在其他设备登录"
                k.replacedBy = "Pixel 9"
            }
        })
        wait(listener.onKicked)
        XCTAssertEqual(listener.kicked, ["账号已在其他设备登录"])
        // 关闭后发送不再受理
        XCTAssertEqual(session.sendText(to: "bob", text: "x"), 0)
    }

    func testOwnMessageStoredLocallyWithZeroUnread() throws {
        let server = try FakeMemexServer()
        defer { server.close() }
        let store = InMemoryChatStore()
        let listener = RecordingListener()
        let (session, outcome) = connect(server, store: store, listener: listener)
        defer { session.close() }
        guard case .ok = outcome else { return XCTFail("期望 .ok，实得 \(outcome)") }

        session.sendText(to: "bob", text: "我发的")
        let convs = store.conversations()
        XCTAssertEqual(convs.map { $0.peer }, ["bob"])
        XCTAssertEqual(convs[0].lastText, "我发的")
        XCTAssertEqual(convs[0].unread, 0) // 自己发的不计未读
    }
}

// NSLock.withLock 在 iOS/macOS 旧 SDK 无此便捷方法，补一个文件级实现
private extension NSLock {
    @discardableResult
    func withLock<T>(_ body: () -> T) -> T {
        lock()
        defer { unlock() }
        return body()
    }
}