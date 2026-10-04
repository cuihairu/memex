import SwiftUI
import MemexKit

/// 初始化向导（R17）：设置服务器地址并连通校验，校验通过才允许保存。
/// 无跳过路径——校验不过不能离开向导（对齐 Android InitActivity）。
struct InitView: View {
    @EnvironmentObject private var model: AppModel
    @State private var addressInput = ""
    @State private var busy = false
    @State private var error: String?

    var body: some View {
        VStack(spacing: 20) {
            Spacer()
            Text("设置服务器")
                .font(.title2)
                .fontWeight(.semibold)
            Text("填写部署的 Memex 服务器地址，校验连通后进入登录。")
                .font(.subheadline)
                .foregroundColor(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 32)

            TextField("例如 memex.example 或 memex.example:24360", text: $addressInput)
                .textFieldStyle(.roundedBorder)
                .keyboardType(.URL)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .padding(.horizontal, 32)

            if let error {
                Text(error)
                    .font(.footnote)
                    .foregroundColor(.red)
            }

            Button {
                verify()
            } label: {
                Text(busy ? "校验中…" : "校验并保存")
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 6)
            }
            .buttonStyle(.borderedProminent)
            .tint(.indigo)
            .disabled(busy)
            .padding(.horizontal, 32)

            Spacer()
        }
        .onAppear {
            if let existing = model.settings.serverAddress {
                addressInput = existing.display()
            }
        }
    }

    private func verify() {
        guard case .ok(let address) = ServerAddress.parse(addressInput) else {
            error = "地址格式不正确（支持 host、host:port、[IPv6]:port）"
            return
        }
        busy = true
        error = nil
        DispatchQueue.global().async {
            let result = MemexClient().probe(address: address)
            DispatchQueue.main.async {
                busy = false
                switch result {
                case .ok:
                    model.settings.save(address: address)
                    model.refresh()
                case .unreachable:
                    error = "无法连接服务器，请检查地址与网络"
                case .timeout:
                    error = "连接超时，请稍后重试"
                case .notMemex:
                    error = "该地址不是 Memex 服务器"
                }
            }
        }
    }
}