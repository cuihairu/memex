import SwiftUI
import UIKit
import MemexKit

/// 聊天页（T6.4 单聊/群聊收发）：消息列表＋输入发送。
/// 自己发的消息右侧显示「我」，对方消息左侧显示发送方（群聊场景）；
/// 已撤回消息展示层置灰并保留原文（对齐桌面端语义）。
struct ChatView: View {
    let peer: String
    @EnvironmentObject private var model: AppModel
    @State private var input = ""
    @State private var messages: [StoredMessage] = []

    private static let shortDate: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "MM-dd HH:mm"
        return f
    }()

    private var title: String {
        peer.hasPrefix("group:") ? "群 \(peer.dropFirst("group:".count))" : peer
    }

    var body: some View {
        VStack(spacing: 0) {
            ScrollViewReader { proxy in
                List(messages, id: \.id) { message in
                    MessageRow(
                        message: message,
                        isMine: message.from == model.loginState.account,
                        timeText: Self.timeText(message.tsMs)
                    )
                    .id(message.id)
                    .listRowSeparator(.hidden)
                }
                .listStyle(.plain)
                .onChange(of: messages.count) { _ in
                    if let last = messages.last {
                        withAnimation(.easeOut(duration: 0.2)) {
                            proxy.scrollTo(last.id, anchor: .bottom)
                        }
                    }
                }
            }

            Divider()
            HStack(spacing: 8) {
                TextField("输入消息", text: $input)
                    .textFieldStyle(.roundedBorder)
                    .padding(.vertical, 6)
                Button("发送") { send() }
                    .buttonStyle(.borderedProminent)
                    .disabled(input.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            }
            .padding(.horizontal, 12)
            .padding(.bottom, 6)
            .safeAreaInset(edge: .bottom, spacing: 0) { EmptyView() }
        }
        .navigationTitle(title)
        .navigationBarTitleDisplayMode(.inline)
        .onAppear {
            reload()
            model.chat.markRead(peer: peer)
        }
    }

    private func reload() {
        messages = model.chat.history(peer: peer)
    }

    private func send() {
        let text = input.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }
        model.chat.sendText(to: peer, text: text)
        input = ""
        reload()
    }

    private static func timeText(_ tsMs: Int64) -> String {
        shortDate.string(from: Date(timeIntervalSince1970: TimeInterval(tsMs) / 1000))
    }
}

private struct MessageRow: View {
    let message: StoredMessage
    let isMine: Bool
    let timeText: String

    var body: some View {
        HStack(alignment: .bottom) {
            if isMine { Spacer(minLength: 48) }
            VStack(alignment: isMine ? .trailing : .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(isMine ? "我" : senderName)
                        .font(.caption)
                        .foregroundColor(.secondary)
                    Text(timeText)
                        .font(.caption2)
                        // 部署目标 iOS 16：Color.tertiary 是 iOS 17+
                        .foregroundColor(Color(UIColor.tertiaryLabel))
                }
                Text(message.recalled ? "（已撤回）" : message.text)
                    .font(.body)
                    .foregroundColor(message.recalled ? .secondary : .primary)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .background(
                        isMine ? Color.indigo.opacity(0.15) : Color(UIColor.systemGray6),
                        in: RoundedRectangle(cornerRadius: 14)
                    )
            }
            if !isMine { Spacer(minLength: 48) }
        }
    }

    private var senderName: String {
        // 群聊场景显示发送方账号；单聊即会话对端
        message.from
    }
}