package com.memex.im.core

import android.content.Context

/** [InitStore] 的 Android 落盘实现：SharedPreferences（进程重启后初始化态仍在） */
class PrefsInitStore(context: Context) : InitStore {
    private val prefs =
        context.getSharedPreferences("memex_init", Context.MODE_PRIVATE)

    override fun isInitialized(): Boolean = prefs.getBoolean(KEY_DONE, false)

    override fun serverAddress(): ServerAddress? {
        if (!isInitialized()) return null
        val host = prefs.getString(KEY_HOST, null) ?: return null
        val port = prefs.getInt(KEY_PORT, ServerAddress.DEFAULT_PORT)
        return ServerAddress(host, port)
    }

    override fun markInitialized(address: ServerAddress) {
        prefs.edit()
            .putBoolean(KEY_DONE, true)
            .putString(KEY_HOST, address.host)
            .putInt(KEY_PORT, address.port)
            .apply()
    }

    private companion object {
        const val KEY_DONE = "initialized"
        const val KEY_HOST = "server_host"
        const val KEY_PORT = "server_port"
    }
}
