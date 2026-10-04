import Foundation

/// 会话消息模型（T6.3/T6.4 会话列表与收发）。
///
/// 语义对齐桌面端 local_store：
/// - peer 为会话维度（单聊=对端账号，群聊="group:<群号>"），与消息 frame 的
///   from/to 无关（自己是发件人时 to 才是 peer）；
/// - source="collab"（移动端全部为协作态，R18）；
/// - msg_id 服务端生成（sha256(from:seq)）用于收方去重与 ACK；
/// - recalled 仅展示层，原文保留（桌面端同语义）。
public struct StoredMessage: Equatable {
    public let id: Int64        // 本地聚合递增（SQLite 为自增主键；调用方传 0）
    public let peer: String
    public let from: String
    public let to: String
    public let seq: UInt64
    public let tsMs: Int64
    public let text: String
    public let source: String
    public let msgId: String
    public let recalled: Bool

    public init(
        id: Int64, peer: String, from: String, to: String, seq: UInt64,
        tsMs: Int64, text: String, source: String, msgId: String, recalled: Bool
    ) {
        self.id = id
        self.peer = peer
        self.from = from
        self.to = to
        self.seq = seq
        self.tsMs = tsMs
        self.text = text
        self.source = source
        self.msgId = msgId
        self.recalled = recalled
    }
}

/// 会话列表条目（按 peer 聚合：最后一条消息预览＋未读数）
public struct Conversation: Equatable {
    public let peer: String
    public let lastTsMs: Int64
    public let lastText: String
    public let unread: Int

    public init(peer: String, lastTsMs: Int64, lastText: String, unread: Int) {
        self.peer = peer
        self.lastTsMs = lastTsMs
        self.lastText = lastText
        self.unread = unread
    }
}

/// 本地消息存储接口。UI 与收发逻辑只依赖本接口；
/// iOS 实现走 SQLite（后续块），单测用 InMemoryChatStore。
public protocol ChatStore: AnyObject {
    /// 幂等追加（msg_id 重复忽略）。返回是否真正插入（false=重复消息）。
    @discardableResult
    func append(_ msg: StoredMessage) -> Bool

    /// 某会话的本地历史：最近 limit 条按时间正序（聊天窗渲染方向）。
    /// （协议要求不设默认参数，便捷调用走下方 extension。）
    func history(peer: String, limit: Int) -> [StoredMessage]

    /// 会话列表：有历史的对端按最近消息时间倒序（含未读数）。
    func conversations() -> [Conversation]

    /// 置已读（清零未读，仅本端展示用）。
    func markRead(peer: String)

    /// 按 msg_id 置撤回标记（RECALL 事件到达后调用）。
    @discardableResult
    func markRecalled(msgId: String) -> Bool

    /// 本端单调 seq（服务端起源消息用的本地序号分配，对齐桌面 next_local_seq）。
    func nextLocalSeq(_ from: String) -> Int64
}

public extension ChatStore {
    /// 便捷调用：默认最近 200 条（协议要求不能带默认参数，放 extension）。
    func history(peer: String) -> [StoredMessage] { history(peer: peer, limit: 200) }
}

/// 内存实现：单测与 App 首块用；与 SQLite 实现共用全部聚合/去重逻辑口径。
/// 消息列表按 (peer, tsMs, id) 保序；NSLock 保护（读线程与发送线程并发）。
public final class InMemoryChatStore: ChatStore {
    private let lock = NSLock()
    private var store: [String: [StoredMessage]] = [:]
    private var unread: [String: Int] = [:]
    private var msgIds: Set<String> = []
    private var nextId: Int64 = 1
    private var localSeqs: [String: Int64] = [:]

    public init() {}

    @discardableResult
    public func append(_ msg: StoredMessage) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        if !msg.msgId.isEmpty && !msgIds.insert(msg.msgId).inserted { return false }
        // id 由存储分配，调用方传 0 无意义
        let stored = StoredMessage(
            id: nextId, peer: msg.peer, from: msg.from, to: msg.to, seq: msg.seq,
            tsMs: msg.tsMs, text: msg.text, source: msg.source,
            msgId: msg.msgId, recalled: msg.recalled
        )
        nextId += 1
        store[msg.peer, default: []].append(stored)
        // 未读只对「收到的消息」计（带服务端 msg_id；自己刚发的 msg_id 为空）
        if !msg.msgId.isEmpty {
            unread[msg.peer, default: 0] += 1
        }
        return true
    }

    public func history(peer: String, limit: Int) -> [StoredMessage] {
        lock.lock()
        defer { lock.unlock() }
        guard let list = store[peer] else { return [] }
        // 先按 (tsMs,id) 全序（容忍乱序到达），取最近 limit 条并保持正序
        return Array(list
            .sorted { l, r in l.tsMs != r.tsMs ? l.tsMs < r.tsMs : l.id < r.id }
            .suffix(limit))
    }

    public func conversations() -> [Conversation] {
        lock.lock()
        defer { lock.unlock() }
        return store
            .map { peer, list in
                let last = list.max { l, r in l.tsMs != r.tsMs ? l.tsMs < r.tsMs : l.id < r.id }!
                return Conversation(
                    peer: peer, lastTsMs: last.tsMs,
                    lastText: last.text, unread: unread[peer] ?? 0
                )
            }
            .sorted { $0.lastTsMs > $1.lastTsMs }
    }

    public func markRead(peer: String) {
        lock.lock()
        defer { lock.unlock() }
        unread[peer] = 0
    }

    @discardableResult
    public func markRecalled(msgId: String) -> Bool {
        if msgId.isEmpty { return false }
        lock.lock()
        defer { lock.unlock() }
        var found = false
        for peer in store.keys {
            guard var list = store[peer] else { continue }
            for idx in list.indices where list[idx].msgId == msgId && !list[idx].recalled {
                let m = list[idx]
                list[idx] = StoredMessage(
                    id: m.id, peer: m.peer, from: m.from, to: m.to, seq: m.seq,
                    tsMs: m.tsMs, text: m.text, source: m.source,
                    msgId: m.msgId, recalled: true
                )
                found = true
            }
            store[peer] = list
        }
        return found
    }

    public func nextLocalSeq(_ from: String) -> Int64 {
        lock.lock()
        defer { lock.unlock() }
        let cur = localSeqs[from] ?? 0
        localSeqs[from] = cur + 1
        return cur + 1
    }
}