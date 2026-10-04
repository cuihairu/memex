package com.memex.im.core

import java.security.MessageDigest

/** 设备指纹：SHA-256 hex（服务端设备台账以此为键，T2.1/T3.3） */
object DeviceIdentity {

    fun sha256Hex(data: ByteArray): String {
        val digest = MessageDigest.getInstance("SHA-256").digest(data)
        val sb = StringBuilder(digest.size * 2)
        for (b in digest) {
            val v = b.toInt() and 0xFF
            sb.append(HEX[v ushr 4]).append(HEX[v and 0xF])
        }
        return sb.toString()
    }

    /** 由稳定种子推导指纹（Android 侧种子＝ANDROID_ID+机型等，见 ui 层组装） */
    fun fingerprint(seed: String): String = sha256Hex(seed.toByteArray(Charsets.UTF_8))

    private val HEX = "0123456789abcdef".toCharArray()
}
