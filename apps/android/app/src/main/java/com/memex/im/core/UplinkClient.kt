package com.memex.im.core

import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import org.json.JSONArray
import org.json.JSONObject

/**
 * 外网单向 uplink 客户端（R23-4）：外网模式（手机）的 Android 接线。
 * 与内网面 [FilesClient] 刻意分开成两个类——两套路由两套 scope，类型上
 * 防误用；功能也刻意做少（设计口径：登录→传文件→看自己记录）：
 *
 *  - POST /uplink/session 换 token（同源账号库；响应带 scope=uplink，
 *    客户端校验——把内网口当外网口配错时 scope=internal，明示报错）。
 *  - POST /uplink/upload 上传：无 target 参数（外网面无落点选择权，
 *    服务端强制落文件助手收件箱）；X-File-Name + octet-stream。
 *  - GET /uplink/mine 我的上传记录（只列本人，纯记录：无下载端点——
 *    外网会话永远没有读取内网数据的权限，铁律）。
 *  - POST /uplink/delete?id= 删自己的上传。
 *
 * 纯 java.net + org.json（对齐 FilesClient：无第三方栈、无 android.*
 * 依赖，JVM 单测可跑）；token 仅内存持有不落盘。字节面一律过 memex
 * server（设计铁律），不直连对象存储。
 */
class UplinkClient {

    /** 外网面调用失败（op/status/error 三元组；status 0＝本地守卫拒） */
    class ApiException(
        val op: String,
        val status: Int,
        val error: String,
    ) : IOException("$op: HTTP $status: $error")

    @Volatile
    var token: String? = null
        private set

    val isLoggedIn: Boolean get() = token != null

    data class UploadResult(val id: Long, val secondTransfer: Boolean)

    /** 我的上传记录（/uplink/mine 的 files 元素；纯记录无下载通道） */
    data class Record(
        val id: Long,
        val fileName: String,
        val fileSize: Long,
        val fileHash: String,
        val uploadTs: Long,
    )

    /**
     * 换 token（POST /uplink/session）。host/port 由部署明示（外网口
     * 默认关闭、显式开启——客户端不设缺省端口常量，防「猜口」惯性）。
     */
    @Throws(ApiException::class)
    fun login(host: String, port: Int, account: String, password: String) {
        val base = "http://" + (if (host.contains(':')) "[$host]" else host) + ":" + port
        val json = request(
            base, "session.login", "POST", "/uplink/session",
            JSONObject().put("account", account).put("password", password).toString(),
            authRequired = false,
        )
        // scope 校验：外网口只发 uplink scope。配错口（连到内网面
        // /files/* 上不存在——404）或未来面变更时明示，不带错 scope 往下走
        val scope = json.optString("scope")
        if (scope != "uplink") {
            throw ApiException("session.login", 200, "该入口不是外网 uplink 入口（scope=$scope）")
        }
        val t = json.optString("token")
        if (t.isEmpty()) throw ApiException("session.login", 200, "响应缺 token")
        token = t
        baseUrl = base
    }

    /** 清 token（退出外网模式；口令本就不落盘） */
    fun logout() {
        token = null
    }

    /**
     * 上传（唯一动作）：服务端强制落文件助手收件箱，客户端不发 target。
     * 首版整包进内存（对齐 FilesClient.upload 口径）。
     */
    @Throws(ApiException::class)
    fun upload(fileName: String, data: ByteArray): UploadResult {
        val op = "uplink.upload"
        if (token == null) throw ApiException(op, 0, "未登录（先 login()）")
        val conn = open("/uplink/upload", "POST")
        conn.setRequestProperty("Authorization", "Bearer ${token}")
        conn.setRequestProperty("Content-Type", "application/octet-stream")
        // X-File-Name 原始 UTF-8 字节上线（HttpURLConnection 头值直写
        // 非 Latin-1 字符不落字节——这是 JVM 已知行为，与内网面同款注记）
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

    /** 我的上传记录（只列本人；纯记录，无任何取回内网数据的端点） */
    @Throws(ApiException::class)
    fun mine(limit: Int = 200, offset: Int = 0): List<Record> {
        val json = request(baseUrl, "uplink.mine", "GET", "/uplink/mine?limit=$limit&offset=$offset")
        val arr = json.optJSONArray("files") ?: JSONArray()
        return (0 until arr.length()).mapNotNull { i ->
            arr.optJSONObject(i)?.let { obj ->
                Record(
                    id = obj.optLong("id"),
                    fileName = obj.optString("file_name"),
                    fileSize = obj.optLong("file_size"),
                    fileHash = obj.optString("file_hash"),
                    uploadTs = obj.optLong("upload_ts"),
                )
            }
        }
    }

    /** 删自己的上传（他人记录/内网文件服务端 403——客户端只管透传） */
    @Throws(ApiException::class)
    fun delete(id: Long) {
        request(baseUrl, "uplink.delete", "POST", "/uplink/delete?id=$id", body = "")
    }

    // —— 内部（请求基建与 FilesClient 同款口径；独立成类不共享基类，
    //     两面客户端各自自洽） ——

    private var baseUrl: String = ""

    private fun open(path: String, method: String): HttpURLConnection {
        val conn = URL(baseUrl + path).openConnection() as HttpURLConnection
        conn.requestMethod = method
        conn.connectTimeout = CONNECT_TIMEOUT_MS
        conn.readTimeout = READ_TIMEOUT_MS
        return conn
    }

    private fun request(
        base: String,
        op: String,
        method: String,
        path: String,
        body: String? = null,
        authRequired: Boolean = true,
    ): JSONObject {
        if (authRequired && token == null) {
            throw ApiException(op, 0, "未登录（先 login()）")
        }
        val conn = (URL(base + path).openConnection() as HttpURLConnection).apply {
            requestMethod = method
            connectTimeout = CONNECT_TIMEOUT_MS
            readTimeout = READ_TIMEOUT_MS
        }
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

    private companion object {
        const val CONNECT_TIMEOUT_MS = 10_000
        const val READ_TIMEOUT_MS = 30_000
    }
}
