package com.memex.im.core

import memex.protocol.v1.Memex.Envelope
import memex.protocol.v1.Memex.LoginResult
import memex.protocol.v1.Memex.MsgType
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.IOException
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

/** T6.3 长连接会话：线格式与分发语义（真 socket＋假服务端，对齐 MemexClientTest） */
class ChatSessionTest {

    private class FakeMemexServer {
        private val serverSocket = ServerSocket(0, 8, InetAddress.getLoopbackAddress())
        val port: Int = serverSocket.localPort
        val received = CopyOnWriteArrayList<Envelope>()   // 客户端发来的全部帧
        val sent = CopyOnWriteArrayList<Envelope>()        // 服务端发帧（按序）
        val connections = CopyOnWriteArrayList<Socket>()   // 存活连接（测试主动注入帧用）

        /** handler(envelope, serverSocket) 决定应答；默认登录即回 LOGIN_RESULT(ok) */
        fun serve(handler: (Envelope, Socket) -> Unit = ::defaultHandler) {
            thread(isDaemon = true, name = "fake-memex") {
                try {
                    while (!serverSocket.isClosed) {
                        val conn = serverSocket.accept()
                        connections.add(conn)
                        thread(isDaemon = true) {
                            conn.use { c ->
                                c.soTimeout = 5_000
                                readLoop(c, handler)
                            }
                        }
                    }
                } catch (_: IOException) {
                }
            }
        }

        private fun defaultHandler(req: Envelope, c: Socket) {
            when (req.type) {
                MsgType.LOGIN -> send(
                    c,
                    Envelope.newBuilder()
                        .setType(MsgType.LOGIN_RESULT)
                        .setSeq(1)
                        .setFrom("server")
                        .setTo(req.login.account)
                        .setTsMs(System.currentTimeMillis())
                        .setLoginResult(LoginResult.newBuilder().setOk(true).setDisplayName("张三").build())
                        .build()
                )
                else -> Unit
            }
        }

        private fun readLoop(c: Socket, handler: (Envelope, Socket) -> Unit) {
            val decoder = FrameCodec.Decoder()
            val input = c.getInputStream()
            val buf = ByteArray(16 * 1024)
            try {
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) return
                    val res = decoder.feed(buf.copyOf(n))
                    for (frame in res.frames) {
                        val env = Envelope.parseFrom(frame)
                        received.add(env)
                        handler(env, c)
                    }
                }
            } catch (_: IOException) {
            }
        }

        fun send(c: Socket, msg: Envelope) {
            sent.add(msg)
            c.getOutputStream().apply { write(FrameCodec.encode(msg.toByteArray())); flush() }
        }

        fun close() {
            serverSocket.close()
        }
    }

    private class RecordingListener : ChatSession.Listener {
        val messages = CopyOnWriteArrayList<Pair<String, String>>() // peer, msgId
        val notices = CopyOnWriteArrayList<Triple<String, NoticeGrade, String>>() // peer, grade, title
        val sent = CopyOnWriteArrayList<Long>()
        val kicked = CopyOnWriteArrayList<String>()
        val disconnected = CopyOnWriteArrayList<String>()
        // 只等首次事件；重复去重断言用集合 size 验证，不靠 latch 计数
        val onMessage = CountDownLatch(1)
        val onNotice = CountDownLatch(1)
        val onSent = CountDownLatch(1)
        val onKicked = CountDownLatch(1)

        override fun onMessage(peer: String, msgId: String, mine: Boolean) {
            messages.add(peer to msgId)
            onMessage.countDown()
        }

        override fun onNotice(
            peer: String,
            grade: NoticeGrade,
            title: String,
            content: String,
            jumpUrl: String,
            msgId: String,
        ) {
            notices.add(Triple(peer, grade, title))
            onNotice.countDown()
        }

        override fun onSent(seq: Long) {
            sent.add(seq)
            onSent.countDown()
        }

        override fun onDisconnected(cause: String) {
            disconnected.add(cause)
        }

        override fun onKicked(reason: String) {
            kicked.add(reason)
            onKicked.countDown()
        }
    }

    private fun connect(
        server: FakeMemexServer, store: ChatStore = InMemoryChatStore(),
        listener: ChatSession.Listener = RecordingListener(),
    ): Pair<ChatSession, ChatSession.ConnectOutcome> {
        val session = ChatSession(
            store = store,
            account = "alice",
            displayName = "Alice",
        )
        val outcome = session.connect(
            address = ServerAddress("127.0.0.1", server.port),
            password = "pw",
            deviceFingerprint = "fp",
            deviceName = "Pixel",
            clientVersion = "0.1.0",
            listener = listener,
        )
        return session to outcome
    }

    private fun waitLatch(l: CountDownLatch) {
        assertTrue("等待超时", l.await(5, TimeUnit.SECONDS))
    }

    @Test
    fun `登录成功且 LOGIN 帧字段完整`() {
        val server = FakeMemexServer()
        lateinit var loginEnvelope: Envelope
        val loginSeen = CountDownLatch(1)
        server.serve { req, c ->
            if (req.type == MsgType.LOGIN) {
                loginEnvelope = req
                loginSeen.countDown()
                server.send(
                    c,
                    Envelope.newBuilder()
                        .setType(MsgType.LOGIN_RESULT)
                        .setSeq(1).setFrom("server").setTo(req.login.account)
                        .setTsMs(System.currentTimeMillis())
                        .setLoginResult(LoginResult.newBuilder().setOk(true).setDisplayName("张三").build())
                        .build()
                )
            }
        }
        val (session, outcome) = connect(server)
        waitLatch(loginSeen)
        assertTrue("期望 Ok，实得 $outcome", outcome is ChatSession.ConnectOutcome.Ok)
        org.junit.Assert.assertEquals(MsgType.LOGIN, loginEnvelope.type)
        assertEquals("alice", loginEnvelope.login.account)
        assertEquals("mobile", loginEnvelope.login.deviceKind)
        session.close()
        server.close()
    }

    @Test
    fun `登录被拒带原因且连接关闭`() {
        val server = FakeMemexServer()
        server.serve { req, c ->
            if (req.type == MsgType.LOGIN) {
                server.send(
                    c,
                    Envelope.newBuilder()
                        .setType(MsgType.LOGIN_RESULT)
                        .setSeq(1).setFrom("server").setTo(req.login.account)
                        .setTsMs(System.currentTimeMillis())
                        .setLoginResult(LoginResult.newBuilder().setOk(false).setReason("账号不存在").build())
                        .build()
                )
            }
        }
        val (_, outcome) = connect(server)
        assertEquals(
            ChatSession.ConnectOutcome.Rejected("账号不存在"),
            outcome,
        )
        server.close()
    }

    @Test
    fun `发送文本帧线格式对齐桌面端`() {
        val server = FakeMemexServer()
        val textLatch = CountDownLatch(1)
        var textEnvelope: Envelope? = null
        server.serve { req, c ->
            if (req.type == MsgType.TEXT) {
                textEnvelope = req
                textLatch.countDown()
                // 服务端受理回执（对齐 session.cpp：ack 带原 seq、to=发送方）
                server.send(
                    c,
                    Envelope.newBuilder()
                        .setType(MsgType.ACK)
                        .setSeq(req.seq)
                        .setFrom("server")
                        .setTo(req.from)
                        .setTsMs(System.currentTimeMillis())
                        .build()
                )
            } else if (req.type == MsgType.LOGIN) {
                server.send(
                    c,
                    Envelope.newBuilder()
                        .setType(MsgType.LOGIN_RESULT)
                        .setSeq(1).setFrom("server").setTo(req.login.account)
                        .setTsMs(System.currentTimeMillis())
                        .setLoginResult(LoginResult.newBuilder().setOk(true).build())
                        .build()
                )
            }
        }
        val listener = RecordingListener()
        val (session, outcome) = connect(server, listener = listener)
        assertTrue(outcome is ChatSession.ConnectOutcome.Ok)

        val seq = session.sendText("bob", "你好")
        assertTrue("seq 应为正，实得 $seq", seq > 0)
        if (!textLatch.await(3, TimeUnit.SECONDS)) {
            org.junit.Assert.fail("TEXT 帧未到达服务端；服务端收到: ${server.received.map { it.type }}")
        }
        waitLatch(listener.onSent)

        val env = textEnvelope!!
        assertEquals(MsgType.TEXT, env.type)
        assertEquals(seq, env.seq)
        assertEquals("alice", env.from)
        assertEquals("bob", env.to)
        assertEquals("你好", env.text.text)
        assertEquals(listOf(seq), listener.sent)

        session.close()
        server.close()
    }

    @Test
    fun `收到 TEXT 回 ACK(msg_id) 落库并回调 重复补投去重`() {
        val server = FakeMemexServer()
        server.serve { req, c ->
            if (req.type == MsgType.LOGIN) {
                server.send(
                    c,
                    Envelope.newBuilder()
                        .setType(MsgType.LOGIN_RESULT)
                        .setSeq(1).setFrom("server").setTo(req.login.account)
                        .setTsMs(System.currentTimeMillis())
                        .setLoginResult(LoginResult.newBuilder().setOk(true).build())
                        .build()
                )
            }
        }
        val store = InMemoryChatStore()
        val listener = RecordingListener()
        val (session, outcome) = connect(server, store = store, listener = listener)
        assertTrue(outcome is ChatSession.ConnectOutcome.Ok)

        // 带 msg_id 的推送（在线即投 / 离线补投共用同帧）
        val textFrame = Envelope.newBuilder()
            .setType(MsgType.TEXT)
            .setSeq(7)
            .setFrom("bob")
            .setTo("alice")
            .setTsMs(System.currentTimeMillis())
            .setMsgId("sha256:bob:7")
            .setText(memex.protocol.v1.Memex.Text.newBuilder().setText("在吗"))
            .build()
        val conn = lastServerConnection(server)
        server.send(conn!!, textFrame)
        waitLatch(listener.onMessage)

        // 已回 ACK(msg_id)
        val ackFrame = server.received.firstOrNull { it.type == MsgType.ACK }
        assertEquals("sha256:bob:7", ackFrame!!.ack.msgId)
        assertEquals("server", ackFrame.to)

        // 落库且已回调
        assertEquals(listOf("bob" to "sha256:bob:7"), listener.messages)
        val hist = store.history("bob")
        assertEquals(1, hist.size)
        assertEquals("在吗", hist[0].text)
        assertEquals("sha256:bob:7", hist[0].msgId)

        // 重复补投：去重不重复落库不重复回调
        server.send(conn, textFrame)
        Thread.sleep(300)
        assertEquals(1, store.history("bob").size)
        assertEquals(1, listener.messages.size)

        session.close()
        server.close()
    }

    @Test
    fun `群消息按 to 归到群会话并回 ACK`() {
        val server = FakeMemexServer()
        server.serve { req, c ->
            if (req.type == MsgType.LOGIN) {
                server.send(
                    c,
                    Envelope.newBuilder()
                        .setType(MsgType.LOGIN_RESULT)
                        .setSeq(1).setFrom("server").setTo(req.login.account)
                        .setTsMs(System.currentTimeMillis())
                        .setLoginResult(LoginResult.newBuilder().setOk(true).build())
                        .build()
                )
            }
        }
        val store = InMemoryChatStore()
        val listener = RecordingListener()
        val (session, outcome) = connect(server, store = store, listener = listener)
        assertTrue(outcome is ChatSession.ConnectOutcome.Ok)

        val conn = lastServerConnection(server)
        server.send(
            conn!!,
            Envelope.newBuilder()
                .setType(MsgType.TEXT)
                .setSeq(3)
                .setFrom("bob")
                .setTo("group:9")
                .setTsMs(System.currentTimeMillis())
                .setMsgId("g1")
                .setText(memex.protocol.v1.Memex.Text.newBuilder().setText("群消息"))
                .build()
        )
        waitLatch(listener.onMessage)
        assertEquals("group:9", store.history("group:9").single().peer)
        assertEquals("群消息", store.history("group:9").single().text)
        assertEquals(server.received.firstOrNull { it.type == MsgType.ACK }!!.ack.msgId, "g1")
        session.close()
        server.close()
    }

    @Test
    fun `KICK 踢下线回调并关闭 且 READ 前不处理后续帧`() {
        val server = FakeMemexServer()
        server.serve { req, c ->
            if (req.type == MsgType.LOGIN) {
                server.send(
                    c,
                    Envelope.newBuilder()
                        .setType(MsgType.LOGIN_RESULT)
                        .setSeq(1).setFrom("server").setTo(req.login.account)
                        .setTsMs(System.currentTimeMillis())
                        .setLoginResult(LoginResult.newBuilder().setOk(true).build())
                        .build()
                )
            }
        }
        val listener = RecordingListener()
        val (session, outcome) = connect(server, listener = listener)
        assertTrue(outcome is ChatSession.ConnectOutcome.Ok)

        val conn = lastServerConnection(server)
        server.send(
            conn!!,
            Envelope.newBuilder()
                .setType(MsgType.KICK)
                .setSeq(2).setFrom("server").setTo("alice")
                .setTsMs(System.currentTimeMillis())
                .setKick(memex.protocol.v1.Memex.Kick.newBuilder().setReason("账号已在其他设备登录").setReplacedBy("Pixel 9"))
                .build()
        )
        waitLatch(listener.onKicked)
        assertEquals(listOf("账号已在其他设备登录"), listener.kicked)
        // 关闭后发送不再受理
        val seq = session.sendText("bob", "x")
        assertEquals(0L, seq)
        server.close()
    }

    @Test
    fun `自己发的消息本地立即落库且计入会话`() {
        val server = FakeMemexServer()
        server.serve() // 默认 handler：LOGIN → LOGIN_RESULT(ok)（漏掉则无人应答、登录永远挂起）
        val store = InMemoryChatStore()
        val listener = RecordingListener()
        val (session, outcome) = connect(server, store = store, listener = listener)
        assertTrue(outcome is ChatSession.ConnectOutcome.Ok)

        session.sendText("bob", "我发的")
        val convs = store.conversations()
        assertEquals(listOf("bob"), convs.map { it.peer })
        assertEquals("我发的", convs[0].lastText)
        assertEquals(0, convs[0].unread) // 自己发的不计未读

        session.close()
        server.close()
    }

    /** 取假服务端当前存活连接（用于主动注入帧）；serve 后 accept 过才有 */
    private fun lastServerConnection(server: FakeMemexServer): Socket? =
        server.connections.firstOrNull { !it.isClosed }

    /** 组一帧服务端通知（对齐 webhook deliver_notice：from=「通知」、msg_id 必带、seq=0） */
    private fun noticeFrame(
        to: String,
        msgId: String,
        title: String,
        content: String,
        urgency: memex.protocol.v1.Memex.Notice.Urgency =
            memex.protocol.v1.Memex.Notice.Urgency.IMPORTANT,
        jumpUrl: String = "",
    ): Envelope = Envelope.newBuilder()
        .setType(MsgType.NOTICE)
        .setSeq(0)
        .setFrom("通知")
        .setTo(to)
        .setTsMs(System.currentTimeMillis())
        .setMsgId(msgId)
        .setNotice(
            memex.protocol.v1.Memex.Notice.newBuilder()
                .setTitle(title)
                .setContent(content)
                .setUrgency(urgency)
                .setJumpUrl(jumpUrl)
                .build()
        )
        .build()

    @Test
    fun `个人通知落库归档态回 ACK 并回调分级`() {
        val server = FakeMemexServer()
        server.serve()
        val store = InMemoryChatStore()
        val listener = RecordingListener()
        val (session, outcome) = connect(server, store = store, listener = listener)
        assertTrue(outcome is ChatSession.ConnectOutcome.Ok)

        val conn = lastServerConnection(server)!!
        server.send(conn, noticeFrame(to = "alice", msgId = "n1", title = "发布", content = "新版本上线"))
        waitLatch(listener.onNotice)

        // 归档形态「标题：正文」（compose_notice_text 同源）；peer=from「通知」
        val hist = store.history("通知")
        assertEquals(1, hist.size)
        assertEquals("发布：新版本上线", hist[0].text)
        assertEquals("n1", hist[0].msgId)
        assertEquals(NoticeGrade.IMPORTANT, listener.notices.single().second)
        // 已回 ACK(msg_id) 清服务端离线队列；通知不走 onMessage（列表刷新由上层桥接）
        assertEquals("n1", server.received.first { it.type == MsgType.ACK }.ack.msgId)
        assertTrue(listener.messages.isEmpty())

        session.close()
        server.close()
    }

    @Test
    fun `群通知按 to 归会话 跳转随文留痕 未指定紧急度按普通`() {
        val server = FakeMemexServer()
        server.serve()
        val store = InMemoryChatStore()
        val listener = RecordingListener()
        val (session, outcome) = connect(server, store = store, listener = listener)
        assertTrue(outcome is ChatSession.ConnectOutcome.Ok)

        val conn = lastServerConnection(server)!!
        server.send(
            conn,
            noticeFrame(
                to = "group:9", msgId = "g9", title = "会议", content = "十点开始",
                urgency = memex.protocol.v1.Memex.Notice.Urgency.URGENCY_UNSPECIFIED,
                jumpUrl = "https://example.com/a",
            ),
        )
        waitLatch(listener.onNotice)

        assertEquals("group:9", store.history("group:9").single().peer)
        assertEquals("会议：十点开始 https://example.com/a", store.history("group:9").single().text)
        assertEquals(NoticeGrade.NORMAL, listener.notices.single().second)

        session.close()
        server.close()
    }

    @Test
    fun `紧急通知分级映射与去重不重复回调但照回 ACK`() {
        val server = FakeMemexServer()
        server.serve()
        val store = InMemoryChatStore()
        val listener = RecordingListener()
        val (session, outcome) = connect(server, store = store, listener = listener)
        assertTrue(outcome is ChatSession.ConnectOutcome.Ok)

        val conn = lastServerConnection(server)!!
        val frame = noticeFrame(
            to = "alice", msgId = "u1", title = "安全",
            content = "异常登录", urgency = memex.protocol.v1.Memex.Notice.Urgency.URGENT,
        )
        server.send(conn, frame)
        waitLatch(listener.onNotice)
        server.send(conn, frame) // 重复补投
        Thread.sleep(300)

        assertEquals(1, listener.notices.size)
        assertEquals(NoticeGrade.URGENT, listener.notices.single().second)
        assertEquals(1, store.history("通知").size)
        // 去重后照回 ACK（服务端按账号清离线队列，重复 ACK 无害）
        assertEquals(2, server.received.count { it.type == MsgType.ACK })

        session.close()
        server.close()
    }

    @Test
    fun `通知分级映射未识别按普通`() {
        assertEquals(NoticeGrade.NORMAL, NoticeGrade.fromProtoNumber(0))
        assertEquals(NoticeGrade.NORMAL, NoticeGrade.fromProtoNumber(1))
        assertEquals(NoticeGrade.IMPORTANT, NoticeGrade.fromProtoNumber(2))
        assertEquals(NoticeGrade.URGENT, NoticeGrade.fromProtoNumber(3))
        assertEquals(NoticeGrade.NORMAL, NoticeGrade.fromProtoNumber(99))
        assertEquals("发布：新版本", composeNoticeText("发布", "新版本", ""))
        assertEquals("发布：新版本 https://e.cn/a", composeNoticeText("发布", "新版本", "https://e.cn/a"))
    }
}