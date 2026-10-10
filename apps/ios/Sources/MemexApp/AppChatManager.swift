import Foundation
import MemexKit

/// seq 持久化台账（BUG-007 §4.1）：UserDefaults 落盘（per-account 键，取 max
/// 合并防乱序回写；随 AppChatManager 存活——跨重新登录续位）。
final class UserDefaultsSeqLedger: SeqLedger {
    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    func loadSeq(account: String) -> UInt64 {
        UInt64(defaults.integer(forKey: "seq_" + account))
    }

    func saveSeq(account: String, seq: UInt64) {
        if seq > loadSeq(account: account) {
            defaults.set(Int(seq), forKey: "seq_" + account)
        }
    }
}

/// 会话管理器（对齐 Android ChatManager）：持有长连接会话与本地存储，
/// 事件回调统一投递主队列刷新 UI；KICK 时经 kickHandler 交上层退回登录页。
final class AppChatManager: ObservableObject {
    @Published private(set) var conversations: [Conversation] = []

    /// KICK 回调（主队列）：上层负责登出回登录页
    var kickHandler: ((String) -> Void)?
    private(set) var disconnectCause: String?

    private let store = InMemoryChatStore()
    private let seqLedger = UserDefaultsSeqLedger()
    private var session: ChatSession?
    private var account = ""

    /// 同步登录并建长连接（阻塞；UI 层放后台队列调用）。
    /// 成功返回 .ok；失败已清理连接。
    func attach(
        address: ServerAddress,
        password: String,
        account: String,
        displayName: String,
        deviceFingerprint: String,
        deviceName: String,
        clientVersion: String
    ) -> ChatSession.ConnectOutcome {
        logoutAndClear()
        self.account = account
        let session = ChatSession(
            store: store, account: account, displayName: displayName,
            eventQueue: .main, seqLedger: seqLedger
        )
        self.session = session
        let outcome = session.connect(
            address: address, password: password,
            deviceFingerprint: deviceFingerprint,
            deviceName: deviceName, clientVersion: clientVersion,
            listener: SessionBridge(self)
        )
        if case .ok = outcome {
            DispatchQueue.main.async { [weak self] in
                self?.render()
            }
        }
        return outcome
    }

    @discardableResult
    func sendText(to: String, text: String) -> UInt64 {
        guard let session else { return 0 }
        let seq = session.sendText(to: to, text: text)
        render()
        return seq
    }

    func history(peer: String, limit: Int = 200) -> [StoredMessage] {
        store.history(peer: peer, limit: limit)
    }

    func markRead(peer: String) {
        store.markRead(peer: peer)
        render()
    }

    func logoutAndClear() {
        session?.logout()
        session = nil
        conversations = []
    }

    private func render() {
        conversations = store.conversations()
    }

    /// 桥接 ChatSessionListener（回调已在主队列）到 @MainActor manager
    private final class SessionBridge: ChatSessionListener {
        private weak var manager: AppChatManager?

        init(_ manager: AppChatManager) {
            self.manager = manager
        }

        func onMessage(peer: String, msgId: String, mine: Bool) {
            manager?.render()
        }

        func onSent(seq: UInt64) {}

        func onDisconnected(cause: String) {
            manager?.disconnectCause = cause
        }

        func onKicked(reason: String) {
            manager?.kickHandler?(reason)
        }
    }
}