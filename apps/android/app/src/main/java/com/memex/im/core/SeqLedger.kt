package com.memex.im.core

import android.content.Context

/**
 * 本端 seq 持久化台账（BUG-007 §4.1）：seqGen 每连接重置时，重登后新消息与
 * 旧档撞 msg_id（服务端已按内容比对消歧，重排为兜底）——本台账对齐桌面
 * 平台-9：账号维度单调续位、发号即写，让常规流保持 sha256(from:seq) 派生式。
 */
interface SeqLedger {
    /** 读账号已落盘 seq 上界（无记录返回 0）。 */
    fun load(account: String): Long

    /** 发号即写（发号方串行调用；实现取 max 合并防乱序回写）。 */
    fun save(account: String, seq: Long)
}

/** 内存实现：JVM 单测用；与 PrefsSeqLedger 共用 max 合并口径。 */
class InMemorySeqLedger : SeqLedger {
    private val seqs = HashMap<String, Long>()

    override fun load(account: String): Long = seqs[account] ?: 0L

    override fun save(account: String, seq: Long) {
        seqs.merge(account, seq) { cur, v -> maxOf(cur, v) }
    }
}

/** [SeqLedger] 的 Android 落盘实现：SharedPreferences（PrefsInitStore 同款机制） */
class PrefsSeqLedger(context: Context) : SeqLedger {
    private val prefs =
        context.getSharedPreferences("memex_seq", Context.MODE_PRIVATE)

    override fun load(account: String): Long = prefs.getLong(seqKey(account), 0L)

    override fun save(account: String, seq: Long) {
        if (seq > load(account)) {
            prefs.edit().putLong(seqKey(account), seq).apply()
        }
    }

    private fun seqKey(account: String) = "seq_$account"
}
