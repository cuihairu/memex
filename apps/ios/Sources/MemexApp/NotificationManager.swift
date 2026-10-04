import Foundation
import UserNotifications
import UIKit

/// 消息推送（T6.4，对齐桌面端 T4.10 三级通知语义）：
/// - normal 站内：仅应用内列表与角标，不打扰；
/// - important 重要：系统横幅＋声音（App 前台同样以横幅展示）；
/// - urgent 紧急：横幅＋声音＋需交互确认（category MEMEX_URGENT，点按/解锁确认）。
/// 载荷字段对齐 webhooks 通知（urgency: normal|important|urgent）。
final class NotificationManager: NSObject {
    static let shared = NotificationManager()

    /// APNs 设备令牌（登录成功后随设备台账上报服务端路径 T2.1/T3.3）
    var deviceToken: Data?

    private override init() {
        super.init()
        UNUserNotificationCenter.current().delegate = self
        registerUrgentCategory()
    }

    func requestAuthorization() {
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .badge, .sound]) { granted, _ in
            // 拒绝时逐级降级：横幅→应用内；本块不强制引导设置
            if !granted {
                print("通知权限被拒：降级为应用内提醒")
            }
        }
    }

    /// App 内/本地横幅（模拟消息到达的展示层；后台到达依赖 APNs 推送）
    func showBanner(title: String, body: String, urgency: String?, threadIdentifier: String = "memex") {
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        content.threadIdentifier = threadIdentifier
        content.sound = .default
        if urgency == "urgent" {
            content.categoryIdentifier = "MEMEX_URGENT"
        }
        let request = UNNotificationRequest(
            identifier: UUID().uuidString, content: content, trigger: nil
        )
        UNUserNotificationCenter.current().add(request) { error in
            if let error {
                print("本地通知投递失败: \(error.localizedDescription)")
            }
        }
    }

    /// 紧急确认动作（对齐桌面「紧急横幅需确认」）：确认即视为已读并跳转
    private func registerUrgentCategory() {
        let confirm = UNNotificationAction(identifier: "confirm", title: "确认", options: [.authenticationRequired])
        let category = UNNotificationCategory(
            identifier: "MEMEX_URGENT", actions: [confirm],
            intentIdentifiers: [], options: []
        )
        UNUserNotificationCenter.current().setNotificationCategories([category])
    }
}

extension NotificationManager: UNUserNotificationCenterDelegate {
    /// 前台到达：按重要度展示横幅（important/urgent 发声＋横幅；normal 仅列表）
    func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        willPresent notification: UNNotification,
        withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void
    ) {
        let urgency = notification.request.content.userInfo["urgency"] as? String
        if urgency == "normal" {
            completionHandler([.list]) // 站内列表，不弹横幅
        } else {
            completionHandler([.banner, .sound, .list])
        }
    }

    func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        didReceive response: UNNotificationResponse,
        withCompletionHandler completionHandler: @escaping () -> Void
    ) {
        // 点击横幅/紧急确认：跳转会话（首块仅前置占位，跳转路径随通知中心块）
        completionHandler()
    }
}