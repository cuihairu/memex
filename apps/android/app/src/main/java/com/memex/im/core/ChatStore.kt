package com.memex.im.core

/**
 * 会话消息模型（T6.3 会话列表与收发）。
 *
 * 语义对齐桌面端 local_store：
 * - peer 为会话维度（单聊=对端账号，群聊="group:<群号>"），与消息 frame 的
 *   from/to 无关（自己是发件人时 to 才是 peer）；
 * - source="collab"（移动端全部为协作态，R18）；
 * - msg_id 服务端生成（sha256(from:seq)）用于收方去重与 ACK；
 * - recalled 仅展示层，原文保留（桌面端同语义）。
 */
data class StoredMessage(
    val id: Long,
    val peer: String,
    val from: String,
    val to: String,
    val seq: Long,
    val tsMs: Long,
    val text: String,
    val source: String,
    val msgId: String,
    val recalled: Boolean,
)

/** 会话列表条目（按 peer 聚合：最后一条消息预览＋未读数） */
data class Conversation(
    val peer: String,
    val lastTsMs: Long,
    val lastText: String,
    val unread: Int,
)

/**
 * 本地消息存储接口。UI 与收发逻辑只依赖本接口；
 * Android 实现走 SQLite（data/SqliteChatStore），JVM 单测用 InMemoryChatStore。
 */
interface ChatStore {
    /** 幂等追加（msg_id 重复忽略）。返回是否真正插入（false=重复消息）。 */
    fun append(msg: StoredMessage): Boolean

    /** 某会话的本地历史：最近 limit 条按时间正序（聊天窗渲染方向）。 */
    fun history(peer: String, limit: Int = 200): List<StoredMessage>

    /** 会话列表：有历史的对端按最近消息时间倒序（含未读数）。source 过滤留接口。 */
    fun conversations(): List<Conversation>

    /** 置已读（清零未读，仅本端展示用）。 */
    fun markRead(peer: String)

    /** 按 msg_id 置撤回标记（RECALL 事件到达后调用）。 */
    fun markRecalled(msgId: String): Boolean

    /** 本端单调 seq（服务端起源消息用的本地序号分配，对齐桌面 next_local_seq）。 */
    fun nextLocalSeq(from: String): Long
}

/**
 * 内存实现：JVM 单测用；与 SQLite 实现共用全部聚合/去重逻辑口径。
 * 消息列表按 (peer, tsMs, id) 保序。
 */
class InMemoryChatStore : ChatStore {
    private val store = LinkedHashMap<String, MutableList<StoredMessage>>()
    private val unread = HashMap<String, Int>()
    private val msgIds = HashSet<String>()
    private var nextId = 1L
    private val localSeqs = HashMap<String, Long>()

    override fun append(msg: StoredMessage): Boolean {
        if (msg.msgId.isNotEmpty() && !msgIds.add(msg.msgId)) return false
        // id 由存储分配（SQLite 实现为自增主键），调用方传 0 无意义
        val stored = msg.copy(id = nextId++)
        val list = store.getOrPut(msg.peer) { mutableListOf() }
        list.add(stored)
        // 未读只对「收到的消息」计（带服务端 msg_id；自己刚发的 msg_id 为空）
        if (msg.msgId.isNotEmpty()) unread[msg.peer] = (unread[msg.peer] ?: 0) + 1
        return true
    }

    override fun history(peer: String, limit: Int): List<StoredMessage> {
        val list = store[peer] ?: return emptyList()
        // 先按 (tsMs,id) 全序（容忍乱序到达），取最近 limit 条并保持正序
        return list.sortedWith(compareBy({ it.tsMs }, { it.id })).takeLast(limit)
    }

    override fun conversations(): List<Conversation> =
        store.map { (peer, list) ->
            val last = list.maxWithOrNull(compareBy({ it.tsMs }, { it.id }))!!
            Conversation(peer, last.tsMs, last.text, unread[peer] ?: 0)
        }.sortedByDescending { it.lastTsMs }

    override fun markRead(peer: String) {
        unread[peer] = 0
    }

    override fun markRecalled(msgId: String): Boolean {
        if (msgId.isEmpty()) return false
        var found = false
        for (list in store.values) {
            for (idx in list.indices) {
                if (list[idx].msgId == msgId && !list[idx].recalled) {
                    list[idx] = list[idx].copy(recalled = true)
                    found = true
                }
            }
        }
        return found
    }

    override fun nextLocalSeq(from: String): Long =
        (localSeqs[from] ?: 0L).let { cur -> localSeqs[from] = cur + 1; cur + 1 }
}