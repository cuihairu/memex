package com.memex.im.core

import java.io.InputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.CopyOnWriteArrayList
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Test

/**
 * 外网 uplink 客户端全链路（真 HTTP＋假 uplink 面）：验证 Android 端
 * 发出的请求形态与 server uplink 面口径一致（端点/鉴权/无 target/
 * 查询串/头/字节/scope 校验），以及响应解析（mine 纯记录、错误语义）。
 * 服务端行为（scope 两道闸/收件箱落点/审计）由桌面 ctest test_uplink
 * 真 TCP 双实例承担；这里只证客户端形态。
 */
class UplinkClientTest {

    /** 脚本化假 uplink 面：记录全部请求，按 handler 决定响应（同
     *  FilesClientTest 的手写 ServerSocket 假面口径） */
    private class FakeUplinkServer {
        data class Recorded(
            val method: String,
            val path: String, // 含 query
            val headers: Map<String, String>,
            val body: ByteArray,
        )

        private val server = ServerSocket(0, 50, InetAddress.getByName("127.0.0.1"))
        val port: Int get() = server.localPort
        val requests = CopyOnWriteArrayList<Recorded>()

        @Volatile
        var handler: (Recorded) -> Pair<Int, String> = {
            404 to """{"ok":false,"error":"未实现"}"""
        }

        private var acceptThread: Thread? = null

        @Volatile
        private var running = false

        fun start() {
            running = true
            acceptThread = Thread { loop() }.apply {
                isDaemon = true
                start()
            }
        }

        private fun loop() {
            while (running) {
                val sock = try {
                    server.accept()
                } catch (_: java.io.IOException) {
                    return
                }
                handle(sock)
            }
        }

        private fun handle(sock: Socket) {
            try {
                sock.use { s ->
                    val input = s.getInputStream()
                    val requestLine = readLine(input) ?: return
                    val headers = readHeaders(input)
                    val contentLength =
                        headers.entries.firstOrNull { it.key.equals("Content-Length", true) }
                            ?.value?.toIntOrNull() ?: 0
                    val body = ByteArray(contentLength)
                    var off = 0
                    while (off < contentLength) {
                        val n = input.read(body, off, contentLength - off)
                        if (n == -1) break
                        off += n
                    }
                    val parts = requestLine.split(" ")
                    val rec = Recorded(
                        method = parts.getOrElse(0) { "" },
                        path = parts.getOrElse(1) { "" },
                        headers = headers,
                        body = body,
                    )
                    requests += rec
                    val (status, text) = handler(rec)
                    val bytes = text.toByteArray(Charsets.UTF_8)
                    val head = "HTTP/1.1 $status ${reason(status)}\r\n" +
                        "Content-Type: application/json\r\n" +
                        "Content-Length: ${bytes.size}\r\nConnection: close\r\n\r\n"
                    s.getOutputStream().apply {
                        write(head.toByteArray(Charsets.UTF_8))
                        write(bytes)
                        flush()
                    }
                    s.shutdownOutput()
                }
            } catch (_: java.io.IOException) {
            }
        }

        fun stop() {
            running = false
            server.close()
            acceptThread?.join(1000)
        }

        /** 读一行（含结尾 CRLF）；流尽回 null。头是字节面——攒字节后按
         *  UTF-8 解码（线口径：X-File-Name 原始 UTF-8，与 FilesClientTest
         *  同款；逐字节 toChar 会把 UTF-8 按 Latin-1 解出乱码） */
        private fun readLine(input: InputStream): String? {
            val buf = ArrayList<Byte>()
            while (true) {
                val c = input.read()
                if (c == -1) return null
                buf.add(c.toByte())
                if (c == '\n'.code && buf.size >= 2 && buf[buf.size - 2] == '\r'.code.toByte()) {
                    return String(buf.toByteArray(), Charsets.UTF_8).trim()
                }
            }
        }

        private fun readHeaders(input: InputStream): Map<String, String> {
            val headers = mutableMapOf<String, String>()
            while (true) {
                val line = readLine(input) ?: break
                if (line.isEmpty()) break
                val idx = line.indexOf(':')
                if (idx > 0) headers[line.substring(0, idx).trim()] = line.substring(idx + 1).trim()
            }
            return headers
        }

        private fun reason(status: Int) = when (status) {
            200 -> "OK"; 401 -> "Unauthorized"; 403 -> "Forbidden"
            404 -> "Not Found"; 413 -> "Payload Too Large"
            503 -> "Service Unavailable"; else -> "Error"
        }
    }

    private lateinit var fake: FakeUplinkServer
    private lateinit var client: UplinkClient

    @Before
    fun setUp() {
        fake = FakeUplinkServer()
        fake.start()
        client = UplinkClient()
    }

    @After
    fun tearDown() {
        fake.stop()
    }

    private fun loginFirst() {
        fake.handler = {
            200 to """{"ok":true,"token":"tok-1","scope":"uplink"}"""
        }
        client.login("127.0.0.1", fake.port, "alice", "pw")
        assertTrue(client.isLoggedIn)
    }

    // —— 会话 ——

    @Test
    fun loginPostsUplinkSessionAndStoresToken() {
        loginFirst()
        val req = fake.requests[0]
        assertEquals("POST", req.method)
        assertEquals("/uplink/session", req.path.substringBefore('?'))
        assertNull(req.headers.entries.firstOrNull { it.key.equals("Authorization", true) })
        val body = String(req.body, Charsets.UTF_8)
        assertTrue(body.contains("\"account\":\"alice\""))
        assertTrue(body.contains("\"password\":\"pw\""))
        // 后续请求带 Bearer
        fake.handler = { 200 to """{"ok":true,"files":[]}""" }
        client.mine()
        assertEquals("Bearer tok-1", fake.requests[1].headers["Authorization"])
    }

    @Test
    fun loginRejectsWrongScopeNotStoringToken() {
        // 连错口（scope=internal＝内网令牌）：明示报错、不留 token
        fake.handler = { 200 to """{"ok":true,"token":"tok-x","scope":"internal"}""" }
        try {
            client.login("127.0.0.1", fake.port, "alice", "pw")
            fail("应抛 ApiException")
        } catch (e: UplinkClient.ApiException) {
            assertEquals("session.login", e.op)
            assertTrue(e.error.contains("scope"))
        }
        assertNull(client.token)
    }

    @Test
    fun login401CarriesStatusAndError() {
        fake.handler = { 401 to """{"error":"账号或口令不正确"}""" }
        try {
            client.login("127.0.0.1", fake.port, "alice", "bad")
            fail("应抛 ApiException")
        } catch (e: UplinkClient.ApiException) {
            assertEquals(401, e.status)
            assertEquals("账号或口令不正确", e.error)
        }
    }

    @Test
    fun unauthenticatedCallsRejectedLocally() {
        try {
            client.upload("x.txt", byteArrayOf(1))
            fail("upload 应本地拒")
        } catch (e: UplinkClient.ApiException) {
            assertEquals(0, e.status)
        }
        try {
            client.mine()
            fail("mine 应本地拒")
        } catch (e: UplinkClient.ApiException) {
            assertEquals(0, e.status)
        }
        try {
            client.delete(1)
            fail("delete 应本地拒")
        } catch (e: UplinkClient.ApiException) {
            assertEquals(0, e.status)
        }
        assertTrue(fake.requests.isEmpty()) // 一个请求都没发
    }

    // —— 上传（无 target：外网面无落点选择权） ——

    @Test
    fun uploadPostsOctetStreamWithoutTarget() {
        loginFirst()
        fake.handler = { 200 to """{"ok":true,"id":21,"second_transfer":true}""" }
        val payload = byteArrayOf(0x00, 0xFF.toByte(), 0x10, 0x7F)
        val r = client.upload("报告 终版.txt", payload)
        assertEquals(21, r.id)
        assertTrue(r.secondTransfer)
        val req = fake.requests[1]
        assertEquals("POST", req.method)
        assertEquals("/uplink/upload", req.path) // 路径上没有 target 参数
        assertEquals("application/octet-stream", req.headers["Content-Type"])
        assertEquals("报告 终版.txt", req.headers["X-File-Name"]) // 原始 UTF-8 上线
        assertArrayEquals(payload, req.body)
    }

    // —— 我的记录 ——

    @Test
    fun mineParsesRecordsAndPaging() {
        loginFirst()
        fake.handler = {
            200 to """
                {"ok":true,"files":[
                 {"id":5,"file_name":"b.bin","file_size":9,"file_hash":"h2","upload_ts":444},
                 {"id":4,"file_name":"外网件.txt","file_size":13,"file_hash":"h1","upload_ts":333}
                ]}
            """.trimIndent()
        }
        val records = client.mine(limit = 50, offset = 10)
        assertEquals("/uplink/mine?limit=50&offset=10", fake.requests[1].path)
        assertEquals(2, records.size)
        assertEquals("b.bin", records[0].fileName)
        assertEquals(9, records[0].fileSize)
        assertEquals("h2", records[0].fileHash)
        assertEquals(444, records[0].uploadTs)
        assertEquals("外网件.txt", records[1].fileName) // UTF-8 还原
    }

    // —— 删除 ——

    @Test
    fun deletePostsIdQuery() {
        loginFirst()
        fake.handler = { 200 to """{"ok":true}""" }
        client.delete(33)
        val req = fake.requests[1]
        assertEquals("POST", req.method)
        assertEquals("/uplink/delete?id=33", req.path)
    }

    // —— 失败明示 ——

    @Test
    fun errorStatusesCarryServerText() {
        loginFirst()
        listOf(
            403 to """{"error":"外网会话只能删除自己的上传"}""",
            503 to """{"error":"存储后端未配置"}""",
        ).forEach { (status, text) ->
            fake.handler = { status to text }
            try {
                client.upload("x", byteArrayOf(1))
                fail("$status 应抛 ApiException")
            } catch (e: UplinkClient.ApiException) {
                assertEquals(status, e.status)
                assertTrue(e.error.isNotEmpty())
            }
        }
    }

    @Test
    fun logoutClearsToken() {
        loginFirst()
        client.logout()
        assertTrue(!client.isLoggedIn)
    }
}
