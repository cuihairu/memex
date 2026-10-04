import SwiftUI
import MemexKit

/// 会话列表（T6.4 会话列表与收发）：未读角标＋最后消息预览＋时间，倒序；
/// 支持发起单聊/群聊（任意 peer，群用 "group:N"）；退出登录回落登录页。
struct ConversationListView: View {
    @EnvironmentObject private var model: AppModel
    @State private var showNewChat = false
    @State private var newPeer = ""
    @State private var path: [String] = []

    private static let shortDate: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "MM-dd HH:mm"
        return f
    }()

    var body: some View {
        NavigationStack(path: $path) {
            Group {
                if model.chat.conversations.isEmpty {
                    VStack(spacing: 8) {
                        Text("暂无会话")
                            .font(.headline)
                            .foregroundColor(.secondary)
                        Text("点右上角发起第一条消息")
                            .font(.footnote)
                            .foregroundColor(.secondary)
                    }
                } else {
                    List(model.chat.conversations, id: \.peer) { conv in
                        NavigationLink(value: conv.peer) {
                            ConversationRow(
                                conv: conv,
                                displayName: Self.displayName(for: conv.peer),
                                timeText: Self.timeText(conv.lastTsMs)
                            )
                        }
                    }
                }
            }
            .navigationTitle("会话")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarLeading) {
                    Button("退出") { model.signOutToLogin() }
                }
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button {
                        newPeer = ""
                        showNewChat = true
                    } label: {
                        Image(systemName: "square.and.pencil")
                    }
                }
            }
            .sheet(isPresented: $showNewChat) {
                NavigationStack {
                    VStack(spacing: 16) {
                        TextField("对方账号（群聊填 group:群号）", text: $newPeer)
                            .textFieldStyle(.roundedBorder)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                            .padding(.horizontal, 20)
                        Spacer()
                    }
                    .padding(.top, 24)
                    .navigationTitle("发起会话")
                    .navigationBarTitleDisplayMode(.inline)
                    .toolbar {
                        ToolbarItem(placement: .cancellationAction) {
                            Button("取消") { showNewChat = false }
                        }
                        ToolbarItem(placement: .confirmationAction) {
                            Button("发起") {
                                let peer = newPeer.trimmingCharacters(in: .whitespacesAndNewlines)
                                showNewChat = false
                                // 发起＝进入聊天页（对齐 Android 发起按钮语义，不发空消息）
                                if !peer.isEmpty { path.append(peer) }
                            }
                        }
                    }
                }
                .presentationDetents([.height(220)])
            }
            .navigationDestination(for: String.self) { peer in
                ChatView(peer: peer)
            }
            .onAppear {
                // 前台回归刷新（会话期间消息已由回调刷新；此处兜底）
                model.refresh()
            }
        }
    }

    private static func displayName(for peer: String) -> String {
        if peer.hasPrefix("group:") {
            return "群 \(peer.dropFirst("group:".count))"
        }
        return peer
    }

    private static func timeText(_ tsMs: Int64) -> String {
        shortDate.string(from: Date(timeIntervalSince1970: TimeInterval(tsMs) / 1000))
    }
}

private struct ConversationRow: View {
    let conv: Conversation
    let displayName: String
    let timeText: String

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .firstTextBaseline) {
                Text(displayName)
                    .font(.body)
                    .fontWeight(.medium)
                Spacer()
                Text(timeText)
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            HStack(alignment: .firstTextBaseline) {
                Text(conv.lastText.isEmpty ? "（尚无消息）" : conv.lastText)
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .lineLimit(1)
                Spacer()
                if conv.unread > 0 {
                    Text("\(conv.unread)")
                        .font(.caption2.bold())
                        .foregroundColor(.white)
                        .padding(.horizontal, 7)
                        .padding(.vertical, 2)
                        .background(Capsule().fill(.red))
                }
            }
        }
        .padding(.vertical, 2)
    }
}