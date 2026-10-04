import XCTest
@testable import MemexKit

/// 路由守卫与登录态（对齐 Android RouteGuardTest 核心组合）
final class AppStateTests: XCTestCase {

    func testEntryMatrix() {
        XCTAssertEqual(RouteGuard.entry(initialized: false, loggedIn: false), .setup)
        XCTAssertEqual(RouteGuard.entry(initialized: false, loggedIn: true), .setup) // 初始化未做谈不上登录
        XCTAssertEqual(RouteGuard.entry(initialized: true, loggedIn: false), .login)
        XCTAssertEqual(RouteGuard.entry(initialized: true, loggedIn: true), .main)
    }

    func testSignInAndOut() {
        let state = LoginState()
        XCTAssertFalse(state.isLoggedIn)
        state.signIn(account: "alice", displayName: "Alice")
        XCTAssertTrue(state.isLoggedIn)
        XCTAssertEqual(state.account, "alice")
        XCTAssertEqual(state.displayName, "Alice")
        state.signOut()
        XCTAssertFalse(state.isLoggedIn)
    }
}