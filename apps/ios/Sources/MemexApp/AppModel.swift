import Foundation
import MemexKit

/// 应用级状态机：入口路由（RouteGuard）、初始化设置、登录态、会话管理器。
/// UI 事件天然在主线程；后台连接完成后的状态变更统一投递主队列（见 Attach）。
final class AppModel: ObservableObject {
    @Published private(set) var entry: AppEntry
    @Published var kickMessage: String?

    let settings = ServerSettings()
    let loginState = LoginState()
    let chat = AppChatManager()

    init() {
        entry = RouteGuard.entry(
            initialized: settings.isInitialized,
            loggedIn: loginState.isLoggedIn
        )
        chat.kickHandler = { [weak self] reason in
            self?.kickMessage = reason
            self?.signOutToLogin()
        }
    }

    /// 依据守卫重算入口（登录成功/登出/初始化完成后调用）
    func refresh() {
        entry = RouteGuard.entry(
            initialized: settings.isInitialized,
            loggedIn: loginState.isLoggedIn
        )
    }

    func signOutToLogin() {
        chat.logoutAndClear()
        loginState.signOut()
        refresh()
    }
}