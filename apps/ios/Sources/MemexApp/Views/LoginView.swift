import SwiftUI
import MemexKit

/// 登录页（R18）：移动端全部为协作态，必须登录后使用。
/// 登录验证与长连接建立一次完成（AppChatManager.attach），成功即移交会话列表，
/// 避免「验证连接＋常连接」双连接互踢（对齐 Android LoginActivity）。
struct LoginView: View {
    @EnvironmentObject private var model: AppModel
    @State private var account = ""
    @State private var password = ""
    @State private var busy = false
    @State private var error: String?

    var body: some View {
        VStack(spacing: 20) {
            Spacer()
            Text("登录 Memex")
                .font(.title2)
                .fontWeight(.semibold)

            VStack(spacing: 12) {
                TextField("账号", text: $account)
                    .textFieldStyle(.roundedBorder)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                SecureField("密码", text: $password)
                    .textFieldStyle(.roundedBorder)
            }
            .padding(.horizontal, 32)

            if let error {
                Text(error)
                    .font(.footnote)
                    .foregroundColor(.red)
            }

            Button {
                login()
            } label: {
                Text(busy ? "登录中…" : "登录")
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 6)
            }
            .buttonStyle(.borderedProminent)
            .disabled(busy)
            .padding(.horizontal, 32)

            Button("修改服务器地址") {
                model.settings.clear()
                model.refresh()
            }
            .font(.footnote)

            Spacer()

            if let server = model.settings.serverAddress {
                Text(server.display())
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
        }
    }

    private func login() {
        guard let address = model.settings.serverAddress else { return }
        let name = account.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else {
            error = "请输入账号"
            return
        }
        busy = true
        error = nil
        DispatchQueue.global().async { [model] in
            let outcome = model.chat.attach(
                address: address,
                password: password,
                account: name,
                displayName: name,
                deviceFingerprint: DeviceIdentity.fingerprint(
                    "ios:\(UIDevice.current.identifierForVendor?.uuidString ?? "unknown"):\(UIDevice.current.model)"
                ),
                deviceName: UIDevice.current.name,
                clientVersion: "0.1.0"
            )
            DispatchQueue.main.async {
                busy = false
                switch outcome {
                case .ok:
                    model.loginState.signIn(account: name, displayName: name)
                    model.refresh()
                case .rejected(let reason):
                    error = reason
                case .unreachable:
                    error = "无法连接服务器"
                case .timeout:
                    error = "登录超时，请稍后重试"
                case .notMemex:
                    error = "服务器应答异常"
                }
            }
        }
    }
}