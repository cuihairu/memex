package com.memex.im.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** T6.3 会话存储聚合语义（对齐桌面端 local_store 的 peers/history 口径） */
class ChatStoreTest {

    private fun msg(
        peer: String, from: String, to: String, seq: Long, tsMs: Long,
        text: String, msgId: String = "",
    ) = StoredMessage(
        id = 0, peer = peer, from = from, to = to, seq = seq,
        tsMs = tsMs, text = text, source = "collab", msgId = msgId, recalled = false,
    )

    private fun msgTo(peer: String, tsMs: Long, text: String, msgId: String = "") =
        msg(peer, "bob", peer, seq = 1, tsMs = tsMs, text = text, msgId = msgId)

    @Test
    fun `会话列表按最后消息时间倒序`() {
        val store = InMemoryChatStore()
        store.append(msgTo("alice", 1000, "早"))
        store.append(msgTo("group:1", 3000, "群"))
        store.append(msgTo("carol", 2000, "晚"))

        val convs = store.conversations()
        assertEquals(listOf("group:1", "carol", "alice"), convs.map { it.peer })
        assertEquals("群", convs[0].lastText)
        assertEquals("晚", convs[1].lastText)
        assertEquals("早", convs[2].lastText)
    }

    @Test
    fun `自己发的消息不计未读 收到的消息计`() {
        val me = "alice"
        val store = InMemoryChatStore()
        // 自己发（msgId 空、from=自己）
        store.append(msg("bob", me, "bob", seq = 1, tsMs = 1000, text = "我在问"))
        // 对方回（msgId 非空）
        store.append(msg("bob", "bob", me, seq = 2, tsMs = 2000, text = "回了", msgId = "m1"))

        val conv = store.conversations().single()
        assertEquals(1, conv.unread)
        store.markRead("bob")
        assertEquals(0, store.conversations().single().unread)
    }

    @Test
    fun `msg_id 重复投递去重且不重复计未读`() {
        val store = InMemoryChatStore()
        // 离线补投 + 在线即投各来一次（桌面端同场景）
        assertTrue(store.append(msgTo("bob", 1000, "你好", msgId = "m1")))
        assertFalse(store.append(msgTo("bob", 1000, "你好", msgId = "m1")))

        assertEquals(1, store.history("bob").size)
        assertEquals(1, store.conversations().single().unread)
    }

    @Test
    fun `历史取最近 limit 条按时间正序`() {
        val store = InMemoryChatStore()
        for (i in 1..5) store.append(msgTo("bob", 1000L * i, "第${i}条"))
        val hist = store.history("bob", limit = 3)
        assertEquals(listOf("第3条", "第4条", "第5条"), hist.map { it.text })
        // 完整历史
        assertEquals(5, store.history("bob").size)
    }

    @Test
    fun `群消息归到群会话`() {
        val me = "alice"
        val store = InMemoryChatStore()
        // 服务端扇出的群消息：from=群成员、to=group:7（对齐 collab_engine peer 规则）
        store.append(msg("group:7", "bob", "group:7", seq = 3, tsMs = 2000, text = "群内容", msgId = "g1"))
        // 自己往群里发：peer=to
        store.append(msg("group:7", me, "group:7", seq = 4, tsMs = 3000, text = "我发的"))

        val hist = store.history("group:7")
        assertEquals(2, hist.size)
        assertEquals(listOf("群内容", "我发的"), hist.map { it.text })
    }

    @Test
    fun `撤回只标记展示层 原文保留`() {
        val store = InMemoryChatStore()
        store.append(msgTo("bob", 1000, "要撤回", msgId = "m9"))
        assertTrue(store.markRecalled("m9"))
        val hist = store.history("bob")
        assertEquals(1, hist.size)
        assertTrue(hist[0].recalled)
        assertEquals("要撤回", hist[0].text)
        // 重复撤回同 id 不再算命中
        assertFalse(store.markRecalled("m9"))
        // 空 id 不动作
        assertFalse(store.markRecalled(""))
    }

    @Test
    fun `本地 seq 单调分配（服务端起源消息）`() {
        val store = InMemoryChatStore()
        assertEquals(1L, store.nextLocalSeq("alice"))
        assertEquals(2L, store.nextLocalSeq("alice"))
        assertEquals(1L, store.nextLocalSeq("bob"))
    }
}