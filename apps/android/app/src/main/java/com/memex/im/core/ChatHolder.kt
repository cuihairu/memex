package com.memex.im.core

/**
 * 应用级会话持有者（T6.3）：登录页建立连接后移交到此，主界面/聊天页消费。
 * 进程内存单例；被踢/注销时清空，重新登录再建。
 */
object ChatHolder {
    @Volatile
    var manager: ChatManager? = null
        private set

    /** 常驻通知器（T6.3 三级推送）；manager 重建（重新登录）后自动重挂。 */
    @Volatile
    private var notifier: ChatManager.Listener? = null

    /** 建立并挂载管理器（登录页调用；重复挂载先关旧的）。seqLedger 随
     *  管理器存活（BUG-007 §4.1：跨重新登录续位，管理器重建不重建台账）。 */
    fun establish(seqLedger: SeqLedger? = null): ChatManager {
        val prev = manager
        if (prev != null) {
            prev.logoutAndClear()
            manager = null
        }
        return ChatManager(ChatStoreFactory.memory(), seqLedger = seqLedger).also { m ->
            manager = m
            notifier?.let { m.addListener(it) }
        }
    }

    /** 应用启动时挂一次常驻通知器（logoutAndClear 清听众后由 establish 重挂）。 */
    fun attachNotifier(l: ChatManager.Listener) {
        notifier = l
        manager?.addListener(l)
    }

    fun clear() {
        manager?.logoutAndClear()
        manager = null
    }
}

/** 存储工厂：本块内存实现；SQLite 持久化随 T6.3 后续块。 */
object ChatStoreFactory {
    fun memory(): ChatStore = InMemoryChatStore()
}