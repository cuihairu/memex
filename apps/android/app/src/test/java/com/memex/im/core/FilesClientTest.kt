package com.memex.im.core

import java.io.File
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
 * 文件面 HTTP 全链路（真 HTTP＋假 FileServer）：验证 Android 端发出的
 * 请求形态与服务端口径一致（端点/鉴权/查询串/头/字节），以及响应
 * 解析正确（inbox 混排两型、me 纯文件数组、JSON 特殊字符、错误语义）。
 * 真端到端由桌面 ctest（test_files_client 进程级真服务端）与 e2e 承担。
 */
class FilesClientTest {

    /** 脚本化假 FileServer：记录全部请求，按 handler 决定响应 */
    private class FakeFilesServer {
        data class Recorded(
            val method: String,
            val path: String, // 含 query
            val headers: Map<String, String>,
            val body: ByteArray,
        ) {
            override fun equals(other: Any?): Boolean = this === other
            override fun hashCode(): Int = System.identityHashCode(this)
        }

        // com.sun.net.httpserver 不在 android.jar 编译类路径上（JVM 单测
        // 只见 android.jar＋JDK java.* 面），假面用 ServerSocket 手写：
        // 逐字节读请求行/头、按 Content-Length 收体、回包 Connection: close
        private val server = ServerSocket(0, 50, InetAddress.getByName("127.0.0.1"))
        val port: Int get() = server.localPort
        val requests = CopyOnWriteArrayList<Recorded>()

        /** handler：(请求) → (状态码, 响应体) */
        @Volatile
        var handler: (Recorded) -> Pair<Int, String> = { 404 to """{"ok":false,"error":"未实现"}""" }

        /** 附加响应头（下载场景回 X-File-Name 用） */
        @Volatile
        var responseHeaders: Map<String, String> = emptyMap()

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
                    return // stop() 关侦听口
                }
                handle(sock) // 测试流量串行，单线程逐个处理
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
                    val head = buildString {
                        append("HTTP/1.1 $status ${reason(status)}\r\n")
                        responseHeaders.forEach { (k, v) -> append("$k: $v\r\n") }
                        append("Content-Type: application/json\r\n")
                        append("Content-Length: ${bytes.size}\r\n")
                        append("Connection: close\r\n\r\n")
                    }
                    s.getOutputStream().apply {
                        write(head.toByteArray(Charsets.UTF_8))
                        write(bytes)
                        flush()
                    }
                    s.shutdownOutput()
                }
            } catch (_: java.io.IOException) {
                // 客户端提前断开等——测试语义忽略
            }
        }

        /** 读一行（含结尾 CRLF）；流尽回 null。头是字节面——攒字节后按
         *  UTF-8 解码（线口径：X-File-Name 原始 UTF-8，与桌面 toUtf8 同） */
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
                if (line.isEmpty()) break // 空行＝头结束
                val idx = line.indexOf(':')
                if (idx > 0) headers[line.substring(0, idx).trim()] = line.substring(idx + 1).trim()
            }
            return headers
        }

        private fun reason(status: Int) = when (status) {
            200 -> "OK"
            400 -> "Bad Request"
            401 -> "Unauthorized"
            403 -> "Forbidden"
            404 -> "Not Found"
            503 -> "Service Unavailable"
            else -> "Status"
        }

        fun stop() {
            running = false
            try {
                server.close()
            } catch (_: java.io.IOException) {
            }
            acceptThread?.join(1000)
        }
    }

    private lateinit var fake: FakeFilesServer
    private lateinit var client: FilesClient

    @Before
    fun setUp() {
        fake = FakeFilesServer()
        fake.start()
        client = FilesClient()
    }

    @After
    fun tearDown() = fake.stop()

    /** 已登录客户端（脚本回 token）；host/port 由 login 组装基址 */
    private fun loginFirst(token: String = "tok-abc", password: String = "pw") {
        fake.handler = { 200 to """{"ok":true,"token":"$token","expires_in":43200}""" }
        client.login("127.0.0.1", fake.port, "alice", password)
        assertEquals(token, client.token)
        assertTrue(client.isLoggedIn)
    }

    // ---------- 会话 ----------

    @Test
    fun `登录换 token——请求体与免鉴权路径形态正确`() {
        loginFirst("tok-abc")
        val req = fake.requests.single()
        assertEquals("POST", req.method)
        assertEquals("/files/session", req.path)
        // 唯一免鉴权路径：不带 Authorization
        assertTrue(!req.headers.containsKey("Authorization"))
        val json = org.json.JSONObject(String(req.body, Charsets.UTF_8))
        assertEquals("alice", json.getString("account"))
        assertEquals("pw", json.getString("password"))
    }

    @Test
    fun `登录失败 401 转异常带状态与文案`() {
        fake.handler = { 401 to """{"ok":false,"error":"账号或口令错误"}""" }
        try {
            client.login("127.0.0.1", fake.port, "alice", "wrong")
            fail("应抛 ApiException")
        } catch (e: FilesClient.ApiException) {
            assertEquals("session.login", e.op)
            assertEquals(401, e.status)
            assertEquals("账号或口令错误", e.error)
        }
        // 登录失败 token 仍为空
        assertNull(client.token)
        assertTrue(!client.isLoggedIn)
    }

    @Test
    fun `未登录调受保护端点本地拒绝 status=0`() {
        try {
            client.listInbox()
            fail("应抛 ApiException")
        } catch (e: FilesClient.ApiException) {
            assertEquals("inbox.list", e.op)
            assertEquals(0, e.status)
        }
        assertTrue(fake.requests.isEmpty())
    }

    @Test
    fun `受保护请求带 Bearer token`() {
        loginFirst("tok-abc")
        fake.handler = { 200 to """{"ok":true,"items":[]}""" }
        client.listInbox()
        val req = fake.requests.last()
        assertEquals("Bearer tok-abc", req.headers["Authorization"])
        assertTrue(req.path.startsWith("/files/list?target=inbox"))
    }

    // ---------- 列表（收件箱混排 / 个人空间） ----------

    @Test
    fun `收件箱混排解析 memo 与 file 两型`() {
        loginFirst()
        fake.handler = {
            200 to """
                {"ok":true,"items":[
                  {"type":"memo","id":7,"content":"买牛奶\n明天 \"九点\"",
                   "created_ms":100,"updated_ms":200},
                  {"type":"file","id":9,"file_name":"a.txt","file_size":12,
                   "file_hash":"h1","pin":0,"status":0,"upload_ts":300},
                  {"type":"weird","id":1}
                ]}
            """.trimIndent()
        }
        val items = client.listInbox(limit = 50, offset = 10)
        assertEquals(2, items.size)
        val memo = items[0] as FilesClient.InboxItem.Memo
        assertEquals(7L, memo.id)
        assertEquals("买牛奶\n明天 \"九点\"", memo.content)
        assertEquals(200L, memo.updatedMs)
        val file = items[1] as FilesClient.InboxItem.FileItem
        assertEquals(9L, file.id)
        assertEquals("a.txt", file.fileName)
        assertEquals(12L, file.fileSize)
        assertEquals(300L, file.uploadTs)
        // 分页参数透传
        assertTrue(fake.requests.last().path.contains("limit=50"))
        assertTrue(fake.requests.last().path.contains("offset=10"))
    }

    @Test
    fun `个人空间 target=me 解析 files 数组（服务端非 inbox 回 files 非 items）`() {
        loginFirst()
        fake.handler = {
            200 to """
                {"ok":true,"files":[
                  {"id":3,"owner":"alice","file_name":"个人.txt","file_size":5,
                   "file_hash":"h2","pin":1,"status":0,"upload_ts":400}
                ]}
            """.trimIndent()
        }
        val items = client.listPersonal()
        assertEquals(1, items.size)
        val file = items[0] as FilesClient.InboxItem.FileItem
        assertEquals(3L, file.id)
        assertEquals("个人.txt", file.fileName)
        assertEquals(1, file.pin)
        val req = fake.requests.last()
        assertTrue(req.path.startsWith("/files/list?target=me"))
        assertEquals("Bearer tok-abc", req.headers["Authorization"])
    }

    // ---------- 备忘录 CRUD ----------

    @Test
    fun `备忘录建改删的 JSON 往返——特殊字符不破`() {
        loginFirst()
        val tricky = "第一行 \"引号\"\n第二行\\反斜杠\t制表"
        fake.handler = { 200 to """{"ok":true,"id":42}""" }
        assertEquals(42L, client.createMemo(tricky))
        val created = fake.requests.last()
        assertEquals("POST", created.method)
        assertEquals("/files/memo", created.path)
        // 请求体可无损还原原文（org.json 编码正确）
        val sent = org.json.JSONObject(String(created.body, Charsets.UTF_8))
        assertEquals(tricky, sent.getString("content"))
        assertTrue(!sent.has("id"))

        fake.handler = { 200 to """{"ok":true,"id":42}""" }
        client.updateMemo(42, "改后内容")
        val updated = org.json.JSONObject(String(fake.requests.last().body, Charsets.UTF_8))
        assertEquals(42L, updated.getLong("id"))
        assertEquals("改后内容", updated.getString("content"))

        fake.handler = { 200 to """{"ok":true}""" }
        client.deleteMemo(42)
        val deleted = fake.requests.last()
        assertEquals("DELETE", deleted.method)
        assertEquals("/files/memo?id=42", deleted.path)
    }

    @Test
    fun `备忘录单条获取与列表解析`() {
        loginFirst()
        fake.handler = {
            200 to """{"ok":true,"memo":{"id":3,"owner":"alice","content":"单条",
              "created_ms":1,"updated_ms":2}}"""
        }
        val memo = client.fetchMemo(3)
        assertEquals(3L, memo.id)
        assertEquals("单条", memo.content)
        assertEquals("/files/memo?id=3", fake.requests.last().path)

        fake.handler = {
            200 to """{"ok":true,"memos":[
              {"id":1,"content":"甲","created_ms":1,"updated_ms":9},
              {"id":2,"content":"乙","created_ms":2,"updated_ms":8}]}"""
        }
        val memos = client.listMemos()
        assertEquals(2, memos.size)
        assertEquals(9L, memos[0].updatedMs)
    }

    @Test
    fun `备忘录删除 404 转异常`() {
        loginFirst()
        fake.handler = { 404 to """{"ok":false,"error":"备忘录不存在"}""" }
        try {
            client.deleteMemo(999)
            fail("应抛 ApiException")
        } catch (e: FilesClient.ApiException) {
            assertEquals("memo.delete", e.op)
            assertEquals(404, e.status)
            assertEquals("备忘录不存在", e.error)
        }
    }

    // ---------- 上传 ----------

    @Test
    fun `上传断言 X-File-Name、octet-stream、target=inbox 与字节一致`() {
        loginFirst()
        fake.handler = { 200 to """{"ok":true,"id":11,"second_transfer":false}""" }
        val payload = byteArrayOf(0x50, 0x4B, 0x03, 0x04, 0x00, -1, 0x7F.toByte())
        val result = client.upload("inbox", "报告 v2.zip", payload)
        assertEquals(11L, result.id)
        assertEquals(false, result.secondTransfer)

        val req = fake.requests.last()
        assertEquals("POST", req.method)
        assertEquals("/files/upload?target=inbox", req.path)
        assertEquals("报告 v2.zip", req.headers["X-File-Name"])
        assertEquals("application/octet-stream", req.headers["Content-Type"])
        assertEquals("Bearer tok-abc", req.headers["Authorization"])
        assertArrayEquals(payload, req.body)
    }

    @Test
    fun `上传秒传标记透传`() {
        loginFirst()
        fake.handler = { 200 to """{"ok":true,"id":12,"second_transfer":true}""" }
        val result = client.upload("inbox", "a.txt", ByteArray(8))
        assertTrue(result.secondTransfer)
    }

    // ---------- 下载 ----------

    @Test
    fun `下载落盘字节一致——X-File-Name 定名且重名加序号不覆盖`() {
        loginFirst()
        val content = "hello 世界".toByteArray(Charsets.UTF_8)
        fake.responseHeaders = mapOf("X-File-Name" to "报告 终版.txt")
        fake.handler = { 200 to String(content, Charsets.UTF_8) }

        val dir = File.createTempFile("files-dl", "").let { f -> f.delete(); f }
        dir.mkdirs()
        try {
            val f1 = client.downloadTo(5, dir)
            assertEquals("报告 终版.txt", f1.name)
            assertArrayEquals(content, f1.readBytes())

            val f2 = client.downloadTo(5, dir)
            assertEquals("报告 终版-1.txt", f2.name)
            assertArrayEquals(content, f2.readBytes())
        } finally {
            dir.deleteRecursively()
        }
    }

    @Test
    fun `下载响应头缺 X-File-Name 时兜底名 download-bin`() {
        loginFirst()
        val content = "raw".toByteArray(Charsets.UTF_8)
        fake.responseHeaders = emptyMap()
        fake.handler = { 200 to String(content, Charsets.UTF_8) }

        val dir = File.createTempFile("files-dl2", "").let { f -> f.delete(); f }
        dir.mkdirs()
        try {
            val saved = client.downloadTo(5, dir)
            assertEquals("download.bin", saved.name)
            assertArrayEquals(content, saved.readBytes())
        } finally {
            dir.deleteRecursively()
        }
    }

    @Test
    fun `下载 404 转异常不落盘`() {
        loginFirst()
        fake.handler = { 404 to """{"ok":false,"error":"文件不存在"}""" }
        val dir = File.createTempFile("files-dl3", "").let { f -> f.delete(); f }
        dir.mkdirs()
        try {
            client.downloadTo(404, dir)
            fail("应抛 ApiException")
        } catch (e: FilesClient.ApiException) {
            assertEquals("file.download", e.op)
            assertEquals(404, e.status)
        }
        assertEquals(0, dir.listFiles()?.size ?: 0)
        dir.deleteRecursively()
    }

    // ---------- 删除文件 ----------

    @Test
    fun `删除文件走 manage-delete 且带 id`() {
        loginFirst()
        fake.handler = { 200 to """{"ok":true}""" }
        client.deleteFile(6)
        val req = fake.requests.last()
        assertEquals("POST", req.method)
        assertEquals("/files/manage/delete?id=6", req.path)
        assertEquals("Bearer tok-abc", req.headers["Authorization"])
    }

    @Test
    fun `删除文件他人资源 403 转异常`() {
        loginFirst()
        fake.handler = { 403 to """{"ok":false,"error":"无权操作该文件"}""" }
        try {
            client.deleteFile(6)
            fail("应抛 ApiException")
        } catch (e: FilesClient.ApiException) {
            assertEquals("file.delete", e.op)
            assertEquals(403, e.status)
            assertTrue(e.error.isNotEmpty())
        }
    }

    // ---------- token 生命周期 ----------

    @Test
    fun `logout 清 token 后受保护端点本地拒绝`() {
        loginFirst()
        client.logout()
        assertNull(client.token)
        assertTrue(!client.isLoggedIn)
        try {
            client.listMemos()
            fail("应抛 ApiException")
        } catch (e: FilesClient.ApiException) {
            assertEquals(0, e.status)
        }
    }
}
