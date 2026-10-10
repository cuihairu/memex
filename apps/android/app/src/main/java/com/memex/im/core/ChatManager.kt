package com.memex.im.core

import android.os.Handler
import android.os.Looper

/**
 * 应用级会话管理器（T6.3）：持有本地存储与长连接，把事件投递到 UI 线程。
 * 登录页 attach（登录验证与长连接一次握手完成）→ 界面订阅 listeners →
 * 注销/被踢时 logoutAndClear。
 *
 * 线程模型：ChatSession 后台读线程执行连接/收发；事件在此转 [uiHandler]
 * 投递 UI 线程，UI 只做展示。
 */
class ChatManager(
    val store: ChatStore,
    private val uiHandler: Handler = Handler(Looper.getMainLooper()),
    private val seqLedger: SeqLedger? = null, // BUG-007 §4.1：重登续位台账
) : ChatSession.Listener {

    private val sessionListeners = LinkedHashSet<Listener>()
    @Volatile
    private var session: ChatSession? = null

    /** UI 订阅回调（UI 线程投递）。 */
    interface Listener {
        /** 新消息（peer 维度；mine=自己刚发的受理暂存；通知也走本回调刷新列表） */
        fun onNewMessage(peer: String)

        /** 站内通知（NOTICE 三级推送；peer 已落库，弹窗裁决由实现决定） */
        fun onNotice(
            peer: String,
            grade: NoticeGrade,
            title: String,
            content: String,
            jumpUrl: String,
            msgId: String,
        )

        /** 发送已受理 */
        fun onSent(seq: Long)

        /** 连接断开 */
        fun onOffline(cause: String)

        /** 被互踢 */
        fun onKicked(reason: String)
    }

    /** 登录验证＋长连接建立（阻塞调用，放后台线程）；成功即可开始收发。 */
    fun attach(
        address: ServerAddress,
        password: String,
        account: String,
        displayName: String,
        deviceFingerprint: String,
        deviceName: String,
        clientVersion: String,
    ): ChatSession.ConnectOutcome {
        logoutAndClear()
        val s = ChatSession(
            store = store,
            account = account,
            displayName = displayName,
            executor = { r -> uiHandler.post(r) },
            seqLedger = seqLedger,
        )
        val outcome = s.connect(
            address = address,
            password = password,
            deviceFingerprint = deviceFingerprint,
            deviceName = deviceName,
            clientVersion = clientVersion,
            listener = this,
        )
        if (outcome is ChatSession.ConnectOutcome.Ok) {
            session = s
        } else {
            s.close()
        }
        return outcome
    }

    /** 发送文本。seq=0 表示未连接。阻塞写走会话发送线程（主线程调用安全，BUG-006）。 */
    fun sendText(to: String, text: String): Long = session?.sendText(to, text) ?: 0

    fun addListener(l: Listener) {
        sessionListeners.add(l)
    }

    fun removeListener(l: Listener) {
        sessionListeners.remove(l)
    }

    /** 注销/退出登录：先 LOGOUT 再断连并清听众。 */
    fun logoutAndClear() {
        session?.logout()
        session = null
        sessionListeners.clear()
    }

    // —— ChatSession.Listener（读线程进入，转 UI 线程）——

    override fun onMessage(peer: String, msgId: String, mine: Boolean) {
        uiHandler.post {
            for (l in sessionListeners.toList()) l.onNewMessage(peer)
        }
    }

    override fun onNotice(
        peer: String,
        grade: NoticeGrade,
        title: String,
        content: String,
        jumpUrl: String,
        msgId: String,
    ) {
        uiHandler.post {
            for (l in sessionListeners.toList()) {
                l.onNewMessage(peer) // 通知已落库，列表/聊天窗照常刷新
                l.onNotice(peer, grade, title, content, jumpUrl, msgId)
            }
        }
    }

    override fun onSent(seq: Long) {
        uiHandler.post {
            for (l in sessionListeners.toList()) l.onSent(seq)
        }
    }

    override fun onDisconnected(cause: String) {
        uiHandler.post {
            for (l in sessionListeners.toList()) l.onOffline(cause)
        }
    }

    override fun onKicked(reason: String) {
        uiHandler.post {
            session = null
            for (l in sessionListeners.toList()) l.onKicked(reason)
        }
    }
}