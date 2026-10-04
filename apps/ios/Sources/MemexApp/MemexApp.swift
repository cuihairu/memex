import SwiftUI
import UserNotifications
import MemexKit

/// Memex iOS 客户端入口（T6.4）。
/// R17/R18：唯一入口经 RootView 守卫——未初始化→设置向导；未登录→登录页；
/// 登录成功→会话列表。全应用无免登录/匿名入口（移动端只有协作态）。
@main
struct MemexApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(model)
        }
    }
}

final class AppDelegate: NSObject, UIApplicationDelegate {
    func application(
        _ application: UIApplication,
        didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil
    ) -> Bool {
        // APNs 权限预申请；注册设备推送（T4.10 移动端横幅/震动面，见 NotificationManager）
        NotificationManager.shared.requestAuthorization()
        application.registerForRemoteNotifications()
        return true
    }

    func application(
        _ application: UIApplication,
        didRegisterForRemoteNotificationsWithDeviceToken deviceToken: Data
    ) {
        NotificationManager.shared.deviceToken = deviceToken
        // 设备 token 上报到服务端（登录成功后随 LOGIN 帧的设备台账路径 T2.1/T3.3）
    }

    func application(
        _ application: UIApplication,
        didFailToRegisterForRemoteNotificationsWithError error: Error
    ) {
        // 无 APNs 环境（模拟器/未配置签名）时静默降级为应用内横幅
        print("APNs 注册失败（降级为本地通知）: \(error.localizedDescription)")
    }
}