package com.memex.im.core

/** 登录态（进程内存）；进程重启后须重新登录（初始化态持久化不受影响） */
class SessionState {
    var account: String? = null
        private set
    var displayName: String? = null
        private set

    val isLoggedIn: Boolean get() = account != null

    fun signIn(account: String, displayName: String) {
        this.account = account
        this.displayName = displayName
    }

    fun signOut() {
        account = null
        displayName = null
    }
}

/** 应用级单例；单测用独立实例，不经过此对象 */
object Session {
    val instance = SessionState()
}
