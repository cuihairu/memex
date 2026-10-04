package com.memex.im.core

import memex.protocol.v1.Memex.Envelope
import memex.protocol.v1.Memex.LoginResult
import memex.protocol.v1.Memex.MsgType
import memex.protocol.v1.Memex.PresenceData
import java.io.IOException
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import kotlin.concurrent.thread
import org.junit.Test

/**
 * 连通性校验与登录的线格式全链路（真 socket＋假服务端）：
 * 验证 Android 端编出的帧能被同协议实现对端理解，反之亦然。
 */
class MemexClientTest {

    /** 假 Memex 服务端：按 handler 决定每条请求是否回帧、回什么帧 */
    private class FakeMemexServer {
        private val serverSocket = ServerSocket(0, 8, InetAddress.getLoopbackAddress())
        val port: Int = serverSocket.localPort

        fun serve(handler: (Envelope, Socket) -> Unit) {
            thread(isDaemon = true, name = "fake-memex-server") {
                try {
                    while (!serverSocket.isClosed) {
                        val conn = serverSocket.accept()
                        thread(isDaemon = true) {
                            conn.use { c ->
                                c.soTimeout = 5_000
                                handleConnection(c, handler)
                            }
                        }
                    }
                } catch (_: IOException) {
                }
            }
        }

        private fun handleConnection(c: Socket, handler: (Envelope, Socket) -> Unit) {
            val decoder = FrameCodec.Decoder()
            val input = c.getInputStream()
            val buf = ByteArray(16 * 1024)
            try {
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) return
                    val res = decoder.feed(buf.copyOf(n))
                    for (frame in res.frames) {
                        handler(Envelope.parseFrom(frame), c)
                    }
                }
            } catch (_: IOException) {
            }
        }

        fun close() {
            serverSocket.close()
        }
    }

    private fun reply(c: Socket, msg: Envelope) {
        c.getOutputStream().apply { write(FrameCodec.encode(msg.toByteArray())); flush() }
    }

    private fun envelope(type: MsgType, seq: Long, from: String, to: String) =
        Envelope.newBuilder()
            .setType(type).setSeq(seq).setFrom(from).setTo(to)
            .setTsMs(System.currentTimeMillis())
            .build()

    // ---------- 探测 ----------

    @Test
    fun `探测成功 PING 收 PONG`() {
        val server = FakeMemexServer()
        server.serve { req, c -> reply(c, envelope(MsgType.PONG, req.seq, "server", req.from)) }
        val result = MemexClient(readTimeoutMs = 2_000).probe(ServerAddress("127.0.0.1", server.port))
        server.close()
        org.junit.Assert.assertTrue("期望 Ok，实得 $result", result is ProbeResult.Ok)
    }

    @Test
    fun `对端应答类型不符 → 非本协议服务端`() {
        val server = FakeMemexServer()
        server.serve { req, c -> reply(c, envelope(MsgType.TEXT, req.seq, "server", req.from)) }
        val result = MemexClient(readTimeoutMs = 2_000).probe(ServerAddress("127.0.0.1", server.port))
        server.close()
        org.junit.Assert.assertTrue("期望 NotMemex，实得 $result", result is ProbeResult.NotMemex)
    }

    @Test
    fun `对端应答非协议字节 → 非本协议服务端`() {
        val server = FakeMemexServer()
        server.serve { _, c ->
            c.getOutputStream().apply { write("HTTP/1.1 400 Bad Request\r\n\r\n".toByteArray()); flush() }
        }
        val result = MemexClient(readTimeoutMs = 2_000).probe(ServerAddress("127.0.0.1", server.port))
        server.close()
        org.junit.Assert.assertTrue("期望 NotMemex，实得 $result", result is ProbeResult.NotMemex)
    }

    @Test
    fun `对端不应答 → 超时`() {
        val server = FakeMemexServer()
        server.serve { _, _ -> /* 收下不回 */ }
        val result = MemexClient(readTimeoutMs = 300).probe(ServerAddress("127.0.0.1", server.port))
        server.close()
        org.junit.Assert.assertTrue("期望 Timeout，实得 $result", result is ProbeResult.Timeout)
    }

    @Test
    fun `端口无人监听 → 连不上`() {
        // 占一个端口再关掉，制造确定无监听的端口
        val probe = ServerSocket(0)
        val port = probe.localPort
        probe.close()
        val result = MemexClient(connectTimeoutMs = 2_000).probe(ServerAddress("127.0.0.1", port))
        org.junit.Assert.assertTrue("期望 Unreachable，实得 $result", result is ProbeResult.Unreachable)
    }

    @Test
    fun `域名无法解析 → 连不上`() {
        val result = MemexClient(connectTimeoutMs = 2_000).probe(ServerAddress("nonexistent.invalid", 24360))
        org.junit.Assert.assertTrue("期望 Unreachable，实得 $result", result is ProbeResult.Unreachable)
    }

    // ---------- 登录 ----------

    @Test
    fun `登录成功（登录广播先到也拿得到 LOGIN_RESULT）`() {
        val server = FakeMemexServer()
        server.serve { req, c ->
            if (req.type == MsgType.LOGIN) {
                // 真实顺序：PRESENCE_DATA 可能先于 LOGIN_RESULT（桌面端引擎同样如此处理）
                reply(
                    c,
                    Envelope.newBuilder()
                        .setType(MsgType.PRESENCE_DATA)
                        .setSeq(1).setFrom("server").setTo(req.login.account)
                        .setTsMs(System.currentTimeMillis())
                        .setPresenceData(PresenceData.newBuilder().addAccounts("someone"))
                        .build()
                )
                reply(
                    c,
                    Envelope.newBuilder()
                        .setType(MsgType.LOGIN_RESULT)
                        .setSeq(2).setFrom("server").setTo(req.login.account)
                        .setTsMs(System.currentTimeMillis())
                        .setLoginResult(
                            LoginResult.newBuilder().setOk(true).setDisplayName("张三").build()
                        )
                        .build()
                )
            }
        }
        val outcome = MemexClient(readTimeoutMs = 2_000).login(
            address = ServerAddress("127.0.0.1", server.port),
            account = "zhangsan",
            password = "pw",
            deviceFingerprint = "fp",
            deviceName = "Pixel",
            clientVersion = "test",
        )
        server.close()
        org.junit.Assert.assertEquals(LoginOutcome.Success::class.simpleName, outcome::class.simpleName)
        org.junit.Assert.assertEquals("张三", (outcome as LoginOutcome.Success).displayName)
    }

    @Test
    fun `登录被拒带原因`() {
        val server = FakeMemexServer()
        server.serve { req, c ->
            if (req.type == MsgType.LOGIN) {
                reply(
                    c,
                    Envelope.newBuilder()
                        .setType(MsgType.LOGIN_RESULT)
                        .setSeq(1).setFrom("server").setTo(req.login.account)
                        .setTsMs(System.currentTimeMillis())
                        .setLoginResult(
                            LoginResult.newBuilder().setOk(false).setReason("账号不存在").build()
                        )
                        .build()
                )
            }
        }
        val outcome = MemexClient(readTimeoutMs = 2_000).login(
            address = ServerAddress("127.0.0.1", server.port),
            account = "ghost",
            password = "pw",
            deviceFingerprint = "fp",
            deviceName = "Pixel",
            clientVersion = "test",
        )
        server.close()
        org.junit.Assert.assertEquals("账号不存在", (outcome as LoginOutcome.Rejected).reason)
    }

    @Test
    fun `登录帧线格式与 LOGIN 载荷字段完整`() {
        lateinit var captured: Envelope
        val server = FakeMemexServer()
        server.serve { req, c ->
            captured = req
            reply(
                c,
                Envelope.newBuilder()
                    .setType(MsgType.LOGIN_RESULT)
                    .setSeq(1).setFrom("server").setTo(req.login.account)
                    .setTsMs(System.currentTimeMillis())
                    .setLoginResult(LoginResult.newBuilder().setOk(true).build())
                    .build()
            )
        }
        MemexClient(readTimeoutMs = 2_000).login(
            address = ServerAddress("127.0.0.1", server.port),
            account = "zhangsan",
            password = "pw",
            deviceFingerprint = "fingerprint-hex",
            deviceName = "Pixel 8",
            clientVersion = "0.1.0",
        )
        server.close()
        org.junit.Assert.assertEquals(MsgType.LOGIN, captured.type)
        org.junit.Assert.assertEquals("server", captured.to)
        org.junit.Assert.assertEquals("zhangsan", captured.login.account)
        org.junit.Assert.assertEquals("pw", captured.login.password)
        org.junit.Assert.assertEquals("fingerprint-hex", captured.login.deviceFingerprint)
        org.junit.Assert.assertEquals("mobile", captured.login.deviceKind)
        org.junit.Assert.assertEquals("Pixel 8", captured.login.deviceName)
        org.junit.Assert.assertEquals("0.1.0", captured.login.clientVersion)
    }
}
