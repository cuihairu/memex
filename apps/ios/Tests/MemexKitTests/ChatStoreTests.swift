import XCTest
@testable import MemexKit

/// 会话存储聚合语义（对齐 Android ChatStoreTest 7 用例与桌面 local_store）
final class ChatStoreTests: XCTestCase {

    private func msg(
        peer: String, from: String, to: String, seq: UInt64 = 1, tsMs: Int64,
        text: String, msgId: String = ""
    ) -> StoredMessage {
        StoredMessage(
            id: 0, peer: peer, from: from, to: to, seq: seq,
            tsMs: tsMs, text: text, source: "collab", msgId: msgId, recalled: false
        )
    }

    private func msgTo(_ peer: String, tsMs: Int64, text: String, msgId: String = "") -> StoredMessage {
        msg(peer: peer, from: "bob", to: peer, tsMs: tsMs, text: text, msgId: msgId)
    }

    func testConversationsSortedByLatestMessageDesc() {
        let store = InMemoryChatStore()
        store.append(msgTo("alice", tsMs: 1000, text: "早"))
        store.append(msgTo("group:1", tsMs: 3000, text: "群"))
        store.append(msgTo("carol", tsMs: 2000, text: "晚"))

        let convs = store.conversations()
        XCTAssertEqual(convs.map { $0.peer }, ["group:1", "carol", "alice"])
        XCTAssertEqual(convs[0].lastText, "群")
        XCTAssertEqual(convs[1].lastText, "晚")
        XCTAssertEqual(convs[2].lastText, "早")
    }

    func testUnreadCountsOnlyReceivedAndMarkReadClears() {
        let me = "alice"
        let store = InMemoryChatStore()
        // 自己发（msgId 空、from=自己）
        store.append(msg(peer: "bob", from: me, to: "bob", seq: 1, tsMs: 1000, text: "我在问"))
        // 对方回（msgId 非空）
        store.append(msg(peer: "bob", from: "bob", to: me, seq: 2, tsMs: 2000, text: "回了", msgId: "m1"))

        let conv = store.conversations().single()
        XCTAssertEqual(conv.unread, 1)
        store.markRead(peer: "bob")
        XCTAssertEqual(store.conversations().single().unread, 0)
    }

    func testMsgIdDedupAndUnreadOnce() {
        let store = InMemoryChatStore()
        // 离线补投 + 在线即投各来一次（桌面端同场景）
        XCTAssertTrue(store.append(msgTo("bob", tsMs: 1000, text: "你好", msgId: "m1")))
        XCTAssertFalse(store.append(msgTo("bob", tsMs: 1000, text: "你好", msgId: "m1")))

        XCTAssertEqual(store.history(peer: "bob").count, 1)
        XCTAssertEqual(store.conversations().single().unread, 1)
    }

    func testHistoryTakesRecentLimitAscending() {
        let store = InMemoryChatStore()
        for i in 1...5 {
            store.append(msgTo("bob", tsMs: 1000 * Int64(i), text: "第\(i)条"))
        }
        XCTAssertEqual(store.history(peer: "bob", limit: 3).map { $0.text }, ["第3条", "第4条", "第5条"])
        XCTAssertEqual(store.history(peer: "bob").count, 5)
    }

    func testGroupMessageRoutedToGroupPeer() {
        let me = "alice"
        let store = InMemoryChatStore()
        // 服务端扇出的群消息：from=群成员、to=group:7（对齐 collab_engine peer 规则）
        store.append(msg(peer: "group:7", from: "bob", to: "group:7", seq: 3, tsMs: 2000, text: "群内容", msgId: "g1"))
        // 自己往群里发：peer=to
        store.append(msg(peer: "group:7", from: me, to: "group:7", seq: 4, tsMs: 3000, text: "我发的"))

        let hist = store.history(peer: "group:7")
        XCTAssertEqual(hist.count, 2)
        XCTAssertEqual(hist.map { $0.text }, ["群内容", "我发的"])
    }

    func testRecallMarksDisplayOnlyKeepingText() {
        let store = InMemoryChatStore()
        store.append(msgTo("bob", tsMs: 1000, text: "要撤回", msgId: "m9"))
        XCTAssertTrue(store.markRecalled(msgId: "m9"))
        let hist = store.history(peer: "bob")
        XCTAssertEqual(hist.count, 1)
        XCTAssertTrue(hist[0].recalled)
        XCTAssertEqual(hist[0].text, "要撤回")
        // 重复撤回同 id 不再算命中
        XCTAssertFalse(store.markRecalled(msgId: "m9"))
        // 空 id 不动作
        XCTAssertFalse(store.markRecalled(msgId: ""))
    }

    func testLocalSeqMonotonicPerPeer() {
        let store = InMemoryChatStore()
        XCTAssertEqual(store.nextLocalSeq("alice"), 1)
        XCTAssertEqual(store.nextLocalSeq("alice"), 2)
        XCTAssertEqual(store.nextLocalSeq("bob"), 1)
    }
}

private extension Array where Element: Equatable {
    func single() -> Element {
        assert(count == 1, "期望恰好一个元素，实得 \(count)")
        return first!
    }
}