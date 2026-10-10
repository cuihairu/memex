package com.memex.im.core

import memex.protocol.v1.Memex.Envelope
import memex.protocol.v1.Memex.Login
import memex.protocol.v1.Memex.LoginResult
import memex.protocol.v1.Memex.MsgType
import java.io.EOFException
import java.io.IOException
import java.net.ConnectException
import java.net.InetSocketAddress
import java.net.NoRouteToHostException
import java.net.Socket
import java.net.SocketTimeoutException
import java.net.UnknownHostException
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/**
 * 协作态长连接会话（T6.3 会话列表与收发）。
 *
 * 对齐桌面端 CollabEngine 语义（collab_engine.cpp）：
 * - 登录后保持单条 TCP 连接；后台读线程分帧分发；
 * - send_text：本端 seq 自增（seqLedger 提供时发号即写、重登续位——BUG-007
 *   §4.1 对齐桌面平台-9），帧 = Envelope{TEXT, seq, from, to, ts_ms, text}；
 * - 收到 TEXT：按 peer 落库（群消息 peer=to 的 "group:N"，单聊 peer=from），
 *   若带 msg_id 回 ACK(msg_id)（服务端按 msg_id 清离线队列，重复投递由本地
 *   msg_id 去重）；自己发的消息本地立即落库（msg_id 为空，等受理回执）；
 * - ACK(seq)：发送方受理回执 → sent 回调；ACK(msg_id) 为接收方已收取，仅清理
 *   本地 pending（移动端首块不维护 inflight 重传，重传随断线重连块）；
 * - KICK：服务端单点互踢 → 收到即断开并回调 onKicked；
 * - 断线：onDisconnected 回调，由上层决定重连（首块不做自动重连）。
 *
 * 线程模型：单连接后台读线程＋单条发送线程（外发帧一律排队经发送线程写
 * socket——调用方常为主线程，Android 严格模式禁主线程网络，BUG-006；
 * 并发写共享输出流也由此串行化）；回调全部投递到构造时传入的 executor
 * （UI 线程）。
 */
class ChatSession(
    private val store: ChatStore,
    private val account: String,
    private val displayName: String,
    private val executor: (Runnable) -> Unit = { it.run() },
    private val connectTimeoutMs: Int = 8_000,
    private val readTimeoutMs: Int = 0, // 0=无限：长连接不因空闲断
    private val seqLedger: SeqLedger? = null, // BUG-007 §4.1：重登续位台账
) : AutoCloseable {
    /** UI 回调（均在 executor 线程投递） */
    interface Listener {
        /** 收到一条新消息（已落库；msgId 空=自己刚发的受理暂存） */
        fun onMessage(peer: String, msgId: String, mine: Boolean)

        /** 收到站内通知（NOTICE，T6.3 三级推送；peer 已落库，分级弹窗由上层裁决） */
        fun onNotice(
            peer: String,
            grade: NoticeGrade,
            title: String,
            content: String,
            jumpUrl: String,
            msgId: String,
        )

        /** 发送受理回执（服务端 ACK(seq)） */
        fun onSent(seq: Long)

        /** 连接断开（未主动 close）——上层决定重连 */
        fun onDisconnected(cause: String)

        /** 被服务端互踢（KICK） */
        fun onKicked(reason: String)
    }

    sealed class ConnectOutcome {
        data class Ok(val rttMs: Long) : ConnectOutcome()
        data class Rejected(val reason: String) : ConnectOutcome()
        data class Unreachable(val detail: String) : ConnectOutcome()
        data class Timeout(val detail: String) : ConnectOutcome()
        data class NotMemex(val detail: String) : ConnectOutcome()
    }

    private val closed = AtomicBoolean(false)

    /**
     * 发号器：构造期从台账续位（BUG-007 §4.1，对齐桌面平台-9——seq 每连接
     * 重置会让重登后的新消息与旧档撞 msg_id）。台账存最后已发号，续位从
     * 上界＋1 起；无台账/无记录从 1 起（单测/默认口径）。
     */
    private val seqGen = AtomicLong(run {
        val last = seqLedger?.load(account) ?: 0L
        if (last > 0) last + 1 else 1L
    })

    /** 发号串行锁：sendText/logout 调用线程与 sendAck 读线程并发，发号与
     *  落盘须同锁保序（乱序回写会让台账上界回退） */
    private val seqLock = Any()
    @Volatile private var readThread: Thread? = null
    private var listener: Listener? = null

    /** 发号＋发号即写（BUG-007 §4.1；调用方跨线程，同锁串行） */
    private fun issueSeq(): Long = synchronized(seqLock) {
        val seq = seqGen.getAndIncrement()
        seqLedger?.save(account, seq)
        seq
    }

    /**
     * 发送线程（单条，随会话存亡）：外发帧（TEXT/LOGOUT/ACK）一律排队经此
     * 写 socket。阻塞写不得落在调用线程——调用方常为主线程，Android 严格
     * 模式禁主线程网络（BUG-006 NetworkOnMainThreadException）；单线程串行
     * 同时消除多线程并发写共享输出流的帧交错。daemon 线程随进程退场。
     */
    private val sendExecutor = Executors.newSingleThreadExecutor { r ->
        Thread(r, "memex-chat-send").apply { isDaemon = true }
    }

    /** 输出流写锁：发送线程之外，connect 的登录帧同锁串行 */
    private val writeLock = Any()

    /**
     * 同步登录并起读循环。成功返回 Ok；失败返回对应结果且连接已关闭。
     * 单测可直接在测试线程调用；UI 侧放后台线程。
     */
    fun connect(
        address: ServerAddress,
        password: String,
        deviceFingerprint: String,
        deviceName: String,
        clientVersion: String,
        listener: Listener,
    ): ConnectOutcome {
        this.listener = listener
        val start = System.nanoTime()
        return try {
            val s = Socket()
            try {
                s.connect(InetSocketAddress(address.host, address.port), connectTimeoutMs)
            } catch (e: ConnectException) {
                return ConnectOutcome.Unreachable(e.message ?: "连接被拒绝")
            } catch (e: NoRouteToHostException) {
                return ConnectOutcome.Unreachable(e.message ?: "不可达")
            } catch (e: UnknownHostException) {
                return ConnectOutcome.Unreachable("域名无法解析：${e.message}")
            }
            s.tcpNoDelay = true
            s.soTimeout = readTimeoutMs
            wire = Wire(s)

            writeEnvelope(
                wire!!,
                Envelope.newBuilder()
                    .setType(MsgType.LOGIN)
                    .setSeq(1)
                    .setFrom(account)
                    .setTo("server")
                    .setTsMs(System.currentTimeMillis())
                    .setLogin(
                        Login.newBuilder()
                            .setAccount(account)
                            .setPassword(password)
                            .setDeviceFingerprint(deviceFingerprint)
                            .setDeviceKind("mobile")
                            .setDeviceName(deviceName)
                            .setClientVersion(clientVersion)
                            .build()
                    )
                    .build()
            )
            // 登录广播可能先于 LOGIN_RESULT 到达（PRESENCE_DATA 等）
            val deadline = System.currentTimeMillis() + readTimeoutMs
            var success = false
            while (true) {
                val reply = wire!!.readEnvelope()
                if (reply.type == MsgType.LOGIN_RESULT) {
                    val r = reply.loginResult
                    success = r.ok
                    if (!r.ok) {
                        safeClose()
                        return ConnectOutcome.Rejected(r.reason.ifEmpty { "登录被拒绝" })
                    }
                    break
                }
                if (readTimeoutMs > 0 && System.currentTimeMillis() > deadline) {
                    safeClose()
                    return ConnectOutcome.Timeout("等待 LOGIN_RESULT 超时")
                }
            }
            closed.set(false)
            readThread = Thread { readLoop() }.apply {
                name = "memex-chat-read"
                isDaemon = true
                start()
            }
            ConnectOutcome.Ok((System.nanoTime() - start) / 1_000_000)
        } catch (e: SocketTimeoutException) {
            safeClose()
            ConnectOutcome.Timeout(e.message ?: "timeout")
        } catch (e: FrameCodec.ProtocolViolation) {
            safeClose()
            ConnectOutcome.NotMemex(e.message ?: "协议不符")
        } catch (e: EOFException) {
            safeClose()
            ConnectOutcome.NotMemex("对端在应答前关闭连接")
        } catch (e: IOException) {
            safeClose()
            ConnectOutcome.Unreachable(e.message ?: "IO 错误")
        } catch (e: Exception) {
            safeClose()
            ConnectOutcome.NotMemex(e.message ?: "异常")
        }
    }

    /**
     * 发送文本。返回分配的 seq（0=未连接/空对象）。
     * 本地落库与回调仍在调用线程同步完成（「本地立即落库」语义不变）；
     * 阻塞的 socket 写移发送线程排队（BUG-006：调用方常为主线程），
     * 写失败走 onDisconnected 回调，调用方不感知写时延。
     */
    fun sendText(to: String, text: String): Long {
        val w = wire ?: return 0
        if (closed.get() || to.isEmpty()) return 0
        val seq = issueSeq()
        val ts = System.currentTimeMillis()
        val frame = Envelope.newBuilder()
            .setType(MsgType.TEXT)
            .setSeq(seq)
            .setFrom(account)
            .setTo(to)
            .setTsMs(ts)
            .setText(memex.protocol.v1.Memex.Text.newBuilder().setText(text))
            .build()
        // 本地立即落库（自己发的消息；msg_id 空，服务端受理后才有——对齐桌面）
        store.append(
            StoredMessage(
                id = 0, peer = to, from = account, to = to, seq = seq,
                tsMs = ts, text = text, source = "collab", msgId = "", recalled = false,
            )
        )
        dispatch { listener?.onMessage(to, "", mine = true) }
        enqueueSend(w, frame) { e -> notifyDisconnect(e.message ?: "发送失败") }
        return seq
    }

    /** 主动登出并断开（LOGOUT 后由服务端关连接，无需等待） */
    fun logout() {
        val w = wire
        if (w != null && !closed.get()) {
            val frame = Envelope.newBuilder()
                .setType(MsgType.LOGOUT)
                .setSeq(issueSeq())
                .setFrom(account)
                .setTo("server")
                .setTsMs(System.currentTimeMillis())
                .build()
            // LOGOUT 帧同样移发送线程（主线程调用同险），写完再关连接
            enqueueSend(w, frame)
            safeCloseAfterWrite()
            return
        }
        safeClose()
    }

    override fun close() = safeClose()

    private fun readLoop() {
        try {
            while (!closed.get()) {
                val env = wire?.readEnvelope() ?: return
                when (env.type) {
                    MsgType.TEXT -> onIncomingText(env)
                    MsgType.NOTICE -> onIncomingNotice(env)
                    MsgType.ACK -> onAck(env)
                    MsgType.KICK -> {
                        val reason = env.kick.reason.ifEmpty { "账号已在其他设备登录" }
                        dispatch { listener?.onKicked(reason) }
                        safeClose()
                        return
                    }
                    MsgType.PONG -> Unit
                    else -> Unit // 组织/群/已读等后续块处理
                }
            }
        } catch (e: Exception) {
            if (!closed.get()) notifyDisconnect(e.message ?: "连接中断")
        }
    }

    private fun onIncomingText(env: Envelope) {
        if (!env.hasText()) return
        // 群消息 peer=to 的 "group:N"；单聊 peer=from（对齐桌面 collab_engine）
        val peer = if (env.to.startsWith("group:")) env.to else env.from
        val inserted = store.append(
            StoredMessage(
                id = 0, peer = peer, from = env.from, to = env.to, seq = env.seq,
                tsMs = env.tsMs, text = env.text.text, source = "collab",
                msgId = env.msgId, recalled = false,
            )
        )
        // 已收取回执（msg_id 非空即回；离线补投去重后照回 ACK——服务端按
        // 账号清离线队列，重复 ACK 无害）
        if (env.msgId.isNotEmpty()) sendAck(env.msgId)
        if (inserted) dispatch { listener?.onMessage(peer, env.msgId, mine = false) }
    }

    /**
     * 收到站内通知（NOTICE；webhook 接入推送，T6.3 三级推送数据面）。
     * 语义对齐桌面 collab_engine.handle_notice：归档形态 compose_notice_text
     * 「标题：正文[ 跳转]」、peer 规则同 TEXT（群=to，个人=from「通知」）、
     * 服务端通知无会话 seq（恒 0）则本地分配单调 seq、回 ACK(msg_id) 清离线
     * 队列；分级弹窗（普通不弹/重要横幅强提醒/紧急需确认收悉）由上层裁决。
     */
    private fun onIncomingNotice(env: Envelope) {
        if (!env.hasNotice()) return
        val n = env.notice
        val ts = if (env.tsMs > 0) env.tsMs else System.currentTimeMillis()
        val peer = if (env.to.startsWith("group:")) env.to else env.from
        val inserted = store.append(
            StoredMessage(
                id = 0, peer = peer, from = env.from, to = env.to,
                seq = if (env.seq > 0) env.seq else store.nextLocalSeq(env.from),
                tsMs = ts, text = composeNoticeText(n.title, n.content, n.jumpUrl),
                source = "collab", msgId = env.msgId, recalled = false,
            )
        )
        if (env.msgId.isNotEmpty()) sendAck(env.msgId)
        if (inserted) {
            dispatch {
                listener?.onNotice(
                    peer, NoticeGrade.fromProtoNumber(n.urgencyValue),
                    n.title, n.content, n.jumpUrl, env.msgId,
                )
            }
        }
    }

    /** 已收取回执（读线程发起，同样移发送线程排队——共享输出流禁并发写） */
    private fun sendAck(msgId: String) {
        val w = wire ?: return
        enqueueSend(
            w,
            Envelope.newBuilder()
                .setType(MsgType.ACK)
                .setSeq(issueSeq())
                .setFrom(account)
                .setTo("server")
                .setTsMs(System.currentTimeMillis())
                .setAck(memex.protocol.v1.Memex.Ack.newBuilder().setMsgId(msgId))
                .build()
        )
    }

    private fun onAck(env: Envelope) {
        val seq = env.seq
        if (seq == 0L) return
        dispatch { listener?.onSent(seq) }
    }

    private fun notifyDisconnect(cause: String) {
        safeClose()
        dispatch { listener?.onDisconnected(cause) }
    }

    private fun dispatch(r: () -> Unit) {
        try {
            executor(r)
        } catch (_: Exception) {
        }
    }

    /** 出站帧排队（发送线程 FIFO 写，写前复查 closed）；[onFailure] 收写失败
     *  （主动关闭引发的写失败不算断连，与 readLoop 同口径不回调） */
    private fun enqueueSend(w: Wire, frame: Envelope, onFailure: (IOException) -> Unit = {}) {
        sendExecutor.execute {
            if (closed.get()) return@execute
            try {
                writeEnvelope(w, frame)
            } catch (e: IOException) {
                if (!closed.get()) onFailure(e)
            }
        }
    }

    /** 输出流写（connect 登录帧在调用线程直写，与此处同锁串行） */
    private fun writeEnvelope(w: Wire, msg: Envelope) {
        synchronized(writeLock) { w.send(msg) }
    }

    /** 发送队列排空后关连接（单线程 FIFO：在队尾追加 close 任务即「写完再关」） */
    private fun safeCloseAfterWrite() {
        sendExecutor.execute { safeClose() }
    }

    private fun safeClose() {
        if (!closed.compareAndSet(false, true)) return
        wire?.close()
        wire = null
    }

    private var wire: Wire? = null

    /** 单条 TCP 连接：帧编解码 + 收发 Envelope（复用 FrameCodec 与 MemexClient 同款） */
    private inner class Wire(private val inner: Socket) : AutoCloseable {
        private val decoder = FrameCodec.Decoder()
        private val pending = ArrayDeque<ByteArray>()
        private val input get() = inner.getInputStream()

        fun send(msg: Envelope) {
            val frame = FrameCodec.encode(msg.toByteArray())
            inner.getOutputStream().apply { write(frame); flush() }
        }

        fun readEnvelope(): Envelope {
            val buf = ByteArray(16 * 1024)
            while (pending.isEmpty()) {
                val n = input.read(buf)
                if (n < 0) throw EOFException("连接被对端关闭")
                val res = decoder.feed(buf.copyOf(n))
                if (res.status == FrameCodec.DecodeStatus.ZERO_LENGTH ||
                    res.status == FrameCodec.DecodeStatus.TOO_LARGE
                ) {
                    throw FrameCodec.ProtocolViolation("非法帧：${res.status}")
                }
                pending.addAll(res.frames)
            }
            val payload = pending.removeFirst()
            return try {
                Envelope.parseFrom(payload)
            } catch (e: IOException) {
                throw FrameCodec.ProtocolViolation("载荷不是合法的 Envelope 编码")
            }
        }

        override fun close() {
            try {
                inner.close()
            } catch (_: IOException) {
            }
        }
    }
}