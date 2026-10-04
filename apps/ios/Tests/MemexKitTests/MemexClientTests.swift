import Foundation
import XCTest
@testable import MemexKit

/// 协作态客户端：探测与登录（对齐 Android MemexClientTest 核心链路）
final class MemexClientTests: XCTestCase {

    private func address(_ server: FakeMemexServer) -> ServerAddress {
        ServerAddress(host: "127.0.0.1", port: server.port)
    }

    func testProbePingPong() throws {
        let server = try FakeMemexServer()
        defer { server.close() }
        server.onEnvelope = { env, c in
            if env.type == .ping {
                server.send(c, Memex_Protocol_V1_Envelope.with { e in
                    e.type = .pong
                    e.seq = 1
                    e.from = "server"
                    e.to = env.from
                    e.tsMs = Int64(Date().timeIntervalSince1970 * 1000)
                })
            }
        }
        let result = MemexClient().probe(address: address(server))
        guard case .ok = result else { return XCTFail("期望 .ok，实得 \(result)") }
        XCTAssertEqual(server.received.first?.type, .ping)
    }

    func testProbeNonMemexReply() throws {
        let server = try FakeMemexServer()
        defer { server.close() }
        server.onEnvelope = { env, c in
            if env.type == .ping {
                // 应答类型不符：不是 PONG
                server.send(c, Memex_Protocol_V1_Envelope.with { e in
                    e.type = .loginResult
                    e.seq = 1
                    e.from = "server"
                    e.to = env.from
                    e.tsMs = Int64(Date().timeIntervalSince1970 * 1000)
                    e.loginResult = Memex_Protocol_V1_LoginResult.with { r in r.ok = true }
                })
            }
        }
        let result = MemexClient().probe(address: address(server))
        guard case .notMemex = result else { return XCTFail("期望 .notMemex，实得 \(result)") }
    }

    func testLoginRejectedWithReason() throws {
        let server = try FakeMemexServer()
        defer { server.close() }
        server.onEnvelope = { env, c in
            if env.type == .login {
                server.send(c, Memex_Protocol_V1_Envelope.with { e in
                    e.type = .loginResult
                    e.seq = 1
                    e.from = "server"
                    e.to = env.login?.account ?? ""
                    e.tsMs = Int64(Date().timeIntervalSince1970 * 1000)
                    e.loginResult = Memex_Protocol_V1_LoginResult.with { r in
                        r.ok = false
                        r.reason = "账号不存在"
                    }
                })
            }
        }
        let outcome = MemexClient().login(
            address: address(server), account: "alice", password: "pw",
            deviceFingerprint: "fp", deviceName: "iPhone", clientVersion: "0.1.0"
        )
        XCTAssertEqual(outcome, .rejected(reason: "账号不存在"))
    }

    func testLoginSuccessReturnsDisplayName() throws {
        let server = try FakeMemexServer()
        defer { server.close() }
        let outcome = MemexClient().login(
            address: address(server), account: "alice", password: "pw",
            deviceFingerprint: "fp", deviceName: "iPhone", clientVersion: "0.1.0"
        )
        XCTAssertEqual(outcome, .success(displayName: "张三"))
    }
}