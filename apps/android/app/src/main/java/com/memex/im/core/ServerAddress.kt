package com.memex.im.core

/**
 * 服务器地址（R17 初始化向导第一步的输入域）。
 *
 * 接受「host」「host:port」「[IPv6]:port」三种形态；也容忍用户粘贴
 * 「http://host:port/」这类带前缀的完整地址（剥掉 scheme 与路径）。
 * 端口缺省取服务端默认监听 24360（server/src/main.cpp kDefaultPort）。
 */
data class ServerAddress(val host: String, val port: Int) {

    init {
        require(port in 1..PORT_MAX) { "端口越界：$port" }
    }

    /** 展示形态：IPv6 加方括号，其余 host:port */
    fun display(): String =
        if (host.contains(':')) "[$host]:$port" else "$host:$port"

    sealed class Parsed {
        data class Ok(val address: ServerAddress) : Parsed()
        data class Err(val reason: Reason) : Parsed()
    }

    enum class Reason { EMPTY, BAD_HOST, BAD_PORT, SCHEME, IPV6_FORM }

    companion object {
        const val DEFAULT_PORT = 24360
        const val PORT_MAX = 65535

        // 主机名/IPv4：字母数字开头结尾，中间允许点、横线、下划线
        private val HOST_RE = Regex("""^[A-Za-z0-9_](?:[A-Za-z0-9._-]*[A-Za-z0-9_])?$""")
        // 方括号内裸 IPv6：十六进制与冒号
        private val IPV6_RE = Regex("""^[0-9A-Fa-f:.]+$""")

        fun parse(raw: String): Parsed {
            var s = raw.trim()
            if (s.isEmpty()) return Parsed.Err(Reason.EMPTY)

            val schemeAt = s.indexOf("://")
            if (schemeAt >= 0) {
                val scheme = s.substring(0, schemeAt).lowercase()
                // 只认 http（用户粘贴习惯）；其余前缀（ftp:// 等）明确报错
                if (scheme != "http") return Parsed.Err(Reason.SCHEME)
                s = s.substring(schemeAt + 3)
            }
            val cut = s.indexOfFirst { it == '/' || it == '?' || it == '#' }
            if (cut >= 0) s = s.substring(0, cut)
            if (s.isEmpty()) return Parsed.Err(Reason.EMPTY)

            val host: String
            var portStr: String? = null
            if (s.startsWith("[")) {
                // [IPv6] 或 [IPv6]:port
                val close = s.indexOf(']')
                if (close <= 1) return Parsed.Err(Reason.IPV6_FORM)
                host = s.substring(1, close)
                val rest = s.substring(close + 1)
                if (rest.isNotEmpty()) {
                    if (!rest.startsWith(":") || rest.length == 1) {
                        return Parsed.Err(Reason.IPV6_FORM)
                    }
                    portStr = rest.substring(1)
                }
                if (!IPV6_RE.matches(host)) return Parsed.Err(Reason.BAD_HOST)
            } else {
                val colon = s.lastIndexOf(':')
                if (colon >= 0) {
                    // 多于一个冒号＝裸 IPv6 没包方括号
                    if (s.indexOf(':') != colon) return Parsed.Err(Reason.IPV6_FORM)
                    host = s.substring(0, colon)
                    portStr = s.substring(colon + 1)
                } else {
                    host = s
                }
                if (host.isEmpty() || !HOST_RE.matches(host)) {
                    return Parsed.Err(Reason.BAD_HOST)
                }
            }

            val port = when {
                portStr == null -> DEFAULT_PORT
                else -> portStr.toIntOrNull()?.takeIf { it in 1..PORT_MAX }
                    ?: return Parsed.Err(Reason.BAD_PORT)
            }
            return Parsed.Ok(ServerAddress(host, port))
        }
    }
}
