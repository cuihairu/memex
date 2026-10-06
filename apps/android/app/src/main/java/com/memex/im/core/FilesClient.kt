package com.memex.im.core

import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import org.json.JSONArray
import org.json.JSONObject

/**
 * 文件面客户端（R23-3 块3）：FileServer HTTP 面的 Android 接线，
 * 端点/语义对齐桌面 FilesClient（client/engine/collab/files_client.cpp）：
 *
 *  - POST /files/session 用同源账号口令换 token（唯一免鉴权路径）；
 *    token 仅内存持有不落盘（对齐桌面 token_，进程重启重新登录）。
 *  - 通用鉴权 header `Authorization: Bearer <token>`。
 *  - 收件箱混排（target=inbox）：备忘录+文件按时间倒序，
 *    memo 取 updated_ms、文件取 upload_ts。
 *  - 上传：原始字节 + X-File-Name + application/octet-stream，
 *    「手机发自己=文件传输」走 target=inbox。
 *  - 下载：流式落盘，重名加序号（base-1.ext）不覆盖。
 *  - 失败统一抛 [ApiException]（op/status/error），对齐桌面
 *    request_failed(op, status, error) 三元组。
 *
 * 纯 java.net + org.json（Android 内置），不引入第三方 HTTP/JSON 栈；
 * 无 android.* 依赖，JVM 单测可直接跑（FakeFilesServer 见 FilesClientTest）。
 * 设计铁律：字节面一律过 memex server，不直连对象存储。
 */
class FilesClient {

    /** 文件面基址（http://host:port）；login(host, port, …) 时组装 */
    private var baseUrl: String = ""

    /** 文件面调用失败（op=操作名、status=HTTP 状态码；status 0＝本地守卫拒） */
    class ApiException(
        val op: String,
        val status: Int,
        val error: String,
    ) : IOException("$op: HTTP $status: $error")

    /** 文件面 token（内存持有；null＝未登录） */
    @Volatile
    var token: String? = null
        private set

    val isLoggedIn: Boolean get() = token != null

    /** 收件箱混排条目（/files/list?target=inbox 的 items 元素） */
    sealed class InboxItem {
        data class Memo(
            val id: Long,
            val content: String,
            val createdMs: Long,
            val updatedMs: Long,
        ) : InboxItem()

        data class FileItem(
            val id: Long,
            val fileName: String,
            val fileSize: Long,
            val fileHash: String,
            val pin: Int,
            val status: Int,
            val uploadTs: Long,
        ) : InboxItem()
    }

    data class Memo(
        val id: Long,
        val content: String,
        val createdMs: Long,
        val updatedMs: Long,
    )

    data class UploadResult(val id: Long, val secondTransfer: Boolean)

    // —— 会话 ——

    /**
     * 换 token（POST /files/session，服务端与消息面同一账号库）。
     * host 取初始化向导落盘的服务器主机（纯 host，不含端口——文件面端口
     * 独立，由调用方给）；IPv6 主机组装时补方括号。
     */
    @Throws(ApiException::class)
    fun login(host: String, port: Int, account: String, password: String) {
        baseUrl = "http://" + (if (host.contains(':')) "[$host]" else host) + ":" + port
        val body = JSONObject()
            .put("account", account)
            .put("password", password)
            .toString()
        val json = request(
            "session.login", "POST", "/files/session", body,
            authRequired = false,
        )
        val t = json.optString("token")
        if (t.isEmpty()) throw ApiException("session.login", 200, "响应缺 token")
        token = t
    }

    /** 清 token（退出文件助手；口令本就不落盘） */
    fun logout() {
        token = null
    }

    // —— 收件箱混排 ——

    @Throws(ApiException::class)
    fun listInbox(limit: Int = 200, offset: Int = 0): List<InboxItem> =
        list("inbox.list", "inbox", limit, offset)

    /**
     * 个人空间列表（target=me）：只列本人文件，与收件箱相互隔离
     * （server 侧个人空间不混排备忘录、不进收件箱文件）。
     */
    @Throws(ApiException::class)
    fun listPersonal(limit: Int = 200, offset: Int = 0): List<InboxItem> =
        list("me.list", "me", limit, offset)

    private fun list(op: String, target: String, limit: Int, offset: Int): List<InboxItem> {
        val json = request(
            op, "GET",
            "/files/list?target=$target&limit=$limit&offset=$offset",
        )
        // 服务端两种响应形（files_server route_list）：inbox 回 items 混排
        // （带 type 字段区分 memo/file），非 inbox（me）回 files 纯文件数组
        val arr = (if (target == "inbox") json.optJSONArray("items")
                   else json.optJSONArray("files")) ?: JSONArray()
        return (0 until arr.length()).mapNotNull { i ->
            val obj = arr.optJSONObject(i) ?: return@mapNotNull null
            val type = if (target == "inbox") obj.optString("type") else "file"
            when (type) {
                "memo" -> InboxItem.Memo(
                    id = obj.optLong("id"),
                    content = obj.optString("content"),
                    createdMs = obj.optLong("created_ms"),
                    updatedMs = obj.optLong("updated_ms"),
                )
                "file" -> InboxItem.FileItem(
                    id = obj.optLong("id"),
                    fileName = obj.optString("file_name"),
                    fileSize = obj.optLong("file_size"),
                    fileHash = obj.optString("file_hash"),
                    pin = obj.optInt("pin"),
                    status = obj.optInt("status"),
                    uploadTs = obj.optLong("upload_ts"),
                )
                else -> null
            }
        }
    }

    // —— 备忘录 CRUD ——

    @Throws(ApiException::class)
    fun createMemo(content: String): Long {
        val json = request(
            "memo.create", "POST", "/files/memo",
            JSONObject().put("content", content).toString(),
        )
        return json.optLong("id")
    }

    /** 有 id=更新（服务端同一 POST /files/memo 路由） */
    @Throws(ApiException::class)
    fun updateMemo(id: Long, content: String): Long {
        val json = request(
            "memo.update", "POST", "/files/memo",
            JSONObject().put("id", id).put("content", content).toString(),
        )
        return json.optLong("id")
    }

    @Throws(ApiException::class)
    fun deleteMemo(id: Long) {
        request("memo.delete", "DELETE", "/files/memo?id=$id")
    }

    @Throws(ApiException::class)
    fun listMemos(limit: Int = 100, offset: Int = 0): List<Memo> {
        val json = request("memo.list", "GET", "/files/memo?limit=$limit&offset=$offset")
        return parseMemos(json.optJSONArray("memos"))
    }

    @Throws(ApiException::class)
    fun fetchMemo(id: Long): Memo {
        val json = request("memo.fetch", "GET", "/files/memo?id=$id")
        val obj = json.optJSONObject("memo")
            ?: throw ApiException("memo.fetch", 200, "响应缺 memo")
        return parseMemo(obj)
    }

    // —— 文件上传/下载/删除 ——

    /**
     * 上传原始字节（target：inbox=收件箱（手机发自己）／me=个人空间／
     * group:<id>=群）。首版整包进内存（手机侧文件体量可控，服务端
     * kMaxUpload=512MiB 与配额另有闸），后续要大文件再改分块流式。
     */
    @Throws(ApiException::class)
    fun upload(target: String, fileName: String, data: ByteArray): UploadResult {
        val op = "inbox.upload"
        if (token == null) throw ApiException(op, 0, "未登录（先 login()）")
        val conn = open("/files/upload?target=$target", "POST")
        conn.setRequestProperty("Authorization", "Bearer ${token}")
        conn.setRequestProperty("Content-Type", "application/octet-stream")
        // X-File-Name 是唯一文件名通道（服务端校验 1..255 字节、禁换行）
        conn.setRequestProperty("X-File-Name", fileName)
        conn.doOutput = true
        conn.setFixedLengthStreamingMode(data.size)
        try {
            conn.outputStream.use { it.write(data) }
        } catch (e: IOException) {
            conn.disconnect()
            throw ApiException(op, 0, e.message ?: "上传中断")
        }
        return finish(op, conn) { json ->
            UploadResult(
                id = json.optLong("id"),
                secondTransfer = json.optBoolean("second_transfer"),
            )
        }
    }

    /** 下载到 [destDir]（不存在则创建），重名加序号（base-1.ext）不覆盖 */
    @Throws(ApiException::class)
    fun downloadTo(fileId: Long, destDir: File): File {
        val op = "file.download"
        if (token == null) throw ApiException(op, 0, "未登录（先 login()）")
        if (!destDir.exists() && !destDir.mkdirs()) {
            throw ApiException(op, 0, "保存目录创建失败")
        }
        val conn = open("/files/download?id=$fileId", "GET")
        conn.setRequestProperty("Authorization", "Bearer ${token}")
        val status = conn.responseCode
        if (status !in 200..299) {
            throw fail(op, status, readError(conn))
        }
        // 文件名取响应头 X-File-Name（服务端已滤控制字符）。线口径是
        // 原始 UTF-8 字节（桌面 toUtf8()/server 原样转发），而
        // HttpURLConnection 按 Latin-1 解码头——先按 Latin-1 还原字节
        // 再按 UTF-8 解码
        val headerName = conn.getHeaderField("X-File-Name") ?: "download.bin"
        val rawName = String(headerName.toByteArray(Charsets.ISO_8859_1), Charsets.UTF_8)
        val target = File(destDir, dedupeName(destDir, sanitizeName(rawName)))
        try {
            conn.inputStream.use { input ->
                target.outputStream().use { output -> input.copyTo(output) }
            }
        } catch (e: IOException) {
            target.delete()
            throw ApiException(op, 0, e.message ?: "下载中断")
        } finally {
            conn.disconnect()
        }
        return target
    }

    @Throws(ApiException::class)
    fun deleteFile(id: Long) {
        request("file.delete", "POST", "/files/manage/delete?id=$id", body = "")
    }

    // —— 内部 ——

    private fun open(path: String, method: String): HttpURLConnection {
        val conn = URL(baseUrl + path).openConnection() as HttpURLConnection
        conn.requestMethod = method
        conn.connectTimeout = CONNECT_TIMEOUT_MS
        conn.readTimeout = READ_TIMEOUT_MS
        return conn
    }

    /** 走一次请求→读 JSON→收尾；非 2xx 转 [ApiException] */
    private fun request(
        op: String,
        method: String,
        path: String,
        body: String? = null,
        authRequired: Boolean = true,
    ): JSONObject {
        if (authRequired && token == null) {
            throw ApiException(op, 0, "未登录（先 login()）")
        }
        val conn = open(path, method)
        if (authRequired) conn.setRequestProperty("Authorization", "Bearer ${token}")
        if (body != null) {
            conn.doOutput = true
            conn.setRequestProperty("Content-Type", "application/json; charset=utf-8")
            try {
                conn.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }
            } catch (e: IOException) {
                conn.disconnect()
                throw ApiException(op, 0, e.message ?: "请求中断")
            }
        }
        return finish(op, conn) { json -> json }
    }

    /** 读状态码→成功回 JSON 给 [onOk]，失败转异常；统一 disconnect */
    private fun <T> finish(op: String, conn: HttpURLConnection, onOk: (JSONObject) -> T): T {
        try {
            val status = conn.responseCode
            if (status !in 200..299) throw fail(op, status, readError(conn))
            val text = conn.inputStream.use { it.readBytes().toString(Charsets.UTF_8) }
            val json = if (text.isBlank()) JSONObject() else JSONObject(text)
            if (!json.optBoolean("ok", true)) {
                throw ApiException(op, status, json.optString("error", "服务端拒绝"))
            }
            return onOk(json)
        } finally {
            conn.disconnect()
        }
    }

    private fun readError(conn: HttpURLConnection): String {
        val text = conn.errorStream?.use { it.readBytes().toString(Charsets.UTF_8) } ?: ""
        if (text.isEmpty()) return "HTTP ${conn.responseCode}"
        return runCatching { JSONObject(text).optString("error") }.getOrNull()
            ?.takeIf { it.isNotEmpty() } ?: "HTTP ${conn.responseCode}"
    }

    private fun fail(op: String, status: Int, error: String) =
        ApiException(op, status, error)

    private fun parseMemos(arr: JSONArray?): List<Memo> =
        if (arr == null) emptyList()
        else (0 until arr.length()).mapNotNull { i ->
            arr.optJSONObject(i)?.let { parseMemo(it) }
        }

    private fun parseMemo(obj: JSONObject) = Memo(
        id = obj.optLong("id"),
        content = obj.optString("content"),
        createdMs = obj.optLong("created_ms"),
        updatedMs = obj.optLong("updated_ms"),
    )

    /** 文件名去控制字符（客户端兜底，服务端头里已滤一层） */
    private fun sanitizeName(name: String): String {
        val cleaned = name.filter { it >= ' ' && it != '\u007f' }.trim()
        return cleaned.ifEmpty { "download.bin" }
    }

    /** 重名加序号：name.ext → name-1.ext → name-2.ext（对齐桌面 QSaveFile 口径） */
    private fun dedupeName(dir: File, name: String): String {
        if (!File(dir, name).exists()) return name
        val dot = name.lastIndexOf('.')
        val base = if (dot > 0) name.substring(0, dot) else name
        val ext = if (dot > 0) name.substring(dot) else ""
        var n = 1
        while (File(dir, "$base-$n$ext").exists()) n++
        return "$base-$n$ext"
    }

    companion object {
        /** 文件面缺省端口（server/src/main.cpp 与桌面 file_assistant 同款 24561） */
        const val DEFAULT_FILES_PORT = 24561

        private const val CONNECT_TIMEOUT_MS = 10_000
        private const val READ_TIMEOUT_MS = 30_000
    }
}
