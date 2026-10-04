package com.memex.im.core

// javalite 按文件名生成外层类 Memex，消息与枚举为其嵌套类
import memex.protocol.v1.Memex.Envelope
import memex.protocol.v1.Memex.Login
import memex.protocol.v1.Memex.MsgType
import java.io.Closeable
import java.io.EOFException
import java.io.IOException
import java.net.ConnectException
import java.net.InetSocketAddress
import java.net.NoRouteToHostException
import java.net.Socket
import java.net.SocketTimeoutException
import java.net.UnknownHostException
import java.util.ArrayDeque

/** 连通性校验结果（R17：校验通过才允许保存并放行） */
sealed class ProbeResult {
    /** 握手成功：PING→PONG，附往返时延 */
    data class Ok(val rttMs: Long) : ProbeResult()

    /** 连不上（拒绝/不可达/域名解析失败） */
    data class Unreachable(val detail: String) : ProbeResult()

    /** 连上了但时限内无应答 */
    data class Timeout(val detail: String) : ProbeResult()

    /** 有应答但不是 Memex 协议（类型不符/帧非法/载荷解析失败） */
    data class NotMemex(val detail: String) : ProbeResult()
}

/** 登录结果 */
sealed class LoginOutcome {
    data class Success(val displayName: String) : LoginOutcome()

    /** 服务端明确拒绝（LOGIN_RESULT.ok=false，含失败原因） */
    data class Rejected(val reason: String) : LoginOutcome()

    data class Unreachable(val detail: String) : LoginOutcome()
    data class Timeout(val detail: String) : LoginOutcome()
    data class NotMemex(val detail: String) : LoginOutcome()
}

/**
 * 协作态客户端（阻塞式；UI 层放后台线程调用，单测直接在测试线程跑）。
 *
 * 协议：帧＝4 字节大端长度前缀 + Envelope（memex.proto 单一事实源）。
 * 连通性校验＝PING/PONG（服务端对未登录连接同样应答，server/src/session.cpp）。
 */
class MemexClient(
    private val connectTimeoutMs: Int = 5_000,
    private val readTimeoutMs: Int = 5_000,
) {
    fun probe(address: ServerAddress): ProbeResult {
        val start = System.nanoTime()
        return try {
            Wire(address).use { wire ->
                wire.send(
                    Envelope.newBuilder()
                        .setType(MsgType.PING)
                        .setSeq(1)
                        .setFrom("mobile-setup")
                        .setTo("server")
                        .setTsMs(System.currentTimeMillis())
                        .build()
                )
                val reply = wire.readEnvelope()
                if (reply.type == MsgType.PONG) {
                    ProbeResult.Ok((System.nanoTime() - start) / 1_000_000)
                } else {
                    ProbeResult.NotMemex("期望 PONG，收到 ${reply.type}")
                }
            }
        } catch (e: SocketTimeoutException) {
            ProbeResult.Timeout(e.message ?: "timeout")
        } catch (e: FrameCodec.ProtocolViolation) {
            ProbeResult.NotMemex(e.message ?: "协议不符")
        } catch (e: EOFException) {
            ProbeResult.NotMemex("对端在应答前关闭连接")
        } catch (e: UnknownHostException) {
            ProbeResult.Unreachable("域名无法解析：${e.message}")
        } catch (e: ConnectException) {
            ProbeResult.Unreachable(e.message ?: "连接被拒绝")
        } catch (e: NoRouteToHostException) {
            ProbeResult.Unreachable(e.message ?: "不可达")
        } catch (e: IOException) {
            ProbeResult.Unreachable(e.message ?: "IO 错误")
        }
    }

    fun login(
        address: ServerAddress,
        account: String,
        password: String,
        deviceFingerprint: String,
        deviceName: String,
        clientVersion: String,
    ): LoginOutcome = try {
        Wire(address).use { wire ->
            wire.send(
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
                            // 服务端口径（server.cpp kind_name）："mobile"＝移动端，
                            // 与桌面端按类型互踢、跨类型并存（T2.1）
                            .setDeviceKind("mobile")
                            .setDeviceName(deviceName)
                            .setClientVersion(clientVersion)
                            .build()
                    )
                    .build()
            )
            // 登录广播（PRESENCE_DATA）可能先于 LOGIN_RESULT 到达，跳过直到目标帧
            val deadline = System.currentTimeMillis() + readTimeoutMs
            while (true) {
                val reply = wire.readEnvelope()
                if (reply.type == MsgType.LOGIN_RESULT) {
                    if (!reply.hasLoginResult()) {
                        return LoginOutcome.NotMemex("LOGIN_RESULT 缺载荷")
                    }
                    val r = reply.loginResult
                    return if (r.ok) {
                        LoginOutcome.Success(r.displayName.ifEmpty { account })
                    } else {
                        LoginOutcome.Rejected(r.reason.ifEmpty { "登录被拒绝" })
                    }
                }
                if (System.currentTimeMillis() > deadline) {
                    return LoginOutcome.Timeout("等待 LOGIN_RESULT 超时")
                }
            }
            @Suppress("UNREACHABLE_CODE")
            LoginOutcome.Timeout("unreachable")
        }
    } catch (e: SocketTimeoutException) {
        LoginOutcome.Timeout(e.message ?: "timeout")
    } catch (e: FrameCodec.ProtocolViolation) {
        LoginOutcome.NotMemex(e.message ?: "协议不符")
    } catch (e: EOFException) {
        LoginOutcome.Timeout("对端在应答前关闭连接")
    } catch (e: UnknownHostException) {
        LoginOutcome.Unreachable("域名无法解析：${e.message}")
    } catch (e: ConnectException) {
        LoginOutcome.Unreachable(e.message ?: "连接被拒绝")
    } catch (e: NoRouteToHostException) {
        LoginOutcome.Unreachable(e.message ?: "不可达")
    } catch (e: IOException) {
        LoginOutcome.Unreachable(e.message ?: "IO 错误")
    }

    /** 单条 TCP 连接：帧编解码 + 收发 Envelope */
    private inner class Wire(address: ServerAddress) : Closeable {
        private val socket = Socket()
        private val decoder = FrameCodec.Decoder()
        private val pending = ArrayDeque<ByteArray>()
        private val input get() = socket.getInputStream()

        init {
            socket.connect(InetSocketAddress(address.host, address.port), connectTimeoutMs)
            socket.soTimeout = readTimeoutMs
            socket.tcpNoDelay = true
        }

        fun send(msg: Envelope) {
            val frame = FrameCodec.encode(msg.toByteArray())
            socket.getOutputStream().apply {
                write(frame)
                flush()
            }
        }

        /** 读一帧并解析；读超时抛 SocketTimeoutException，对端关闭抛 EOFException，帧/载荷非法抛 ProtocolViolation */
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
                socket.close()
            } catch (_: IOException) {
            }
        }
    }
}
