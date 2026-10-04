import Foundation

/// 应用入口路由（对齐 Android RouteGuard 语义）：
/// 未初始化 → 设置向导；已初始化未登录 → 登录页；齐备 → 主界面。
/// 移动端全应用无免登录/匿名入口（R17/R18）。
public enum AppEntry: Equatable {
    case setup
    case login
    case main
}

public enum RouteGuard {
    public static func entry(initialized: Bool, loggedIn: Bool) -> AppEntry {
        if !initialized { return .setup }
        return loggedIn ? .main : .login
    }
}

/// 登录态（进程内存）；进程重启后须重新登录（初始化态持久化不受影响）
public final class LoginState {
    public private(set) var account: String?
    public private(set) var displayName: String?

    public init() {}

    public var isLoggedIn: Bool { account != nil }

    public func signIn(account: String, displayName: String) {
        self.account = account
        self.displayName = displayName
    }

    public func signOut() {
        account = nil
        displayName = nil
    }
}