import Foundation
import MemexKit

/// 初始化态（R17）：服务器地址持久化；进程重启后登录态不保留，
/// 初始化态保留（对齐 Android PrefsInitStore）。
final class ServerSettings {
    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    private enum Keys {
        static let initialized = "memex.initialized"
        static let serverAddress = "memex.server.address"
    }

    var isInitialized: Bool {
        defaults.bool(forKey: Keys.initialized)
    }

    var serverAddress: ServerAddress? {
        guard let raw = defaults.string(forKey: Keys.serverAddress) else { return nil }
        if case .ok(let address) = ServerAddress.parse(raw) { return address }
        return nil
    }

    /// 校验通过后保存（对齐 R17：校验不过不许保存）
    func save(address: ServerAddress) {
        defaults.set(address.display(), forKey: Keys.serverAddress)
        defaults.set(true, forKey: Keys.initialized)
    }

    func clear() {
        defaults.removeObject(forKey: Keys.serverAddress)
        defaults.set(false, forKey: Keys.initialized)
    }
}