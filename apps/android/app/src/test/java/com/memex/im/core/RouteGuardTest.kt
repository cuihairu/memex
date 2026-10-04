package com.memex.im.core

import com.memex.im.core.InitEntry.SHOW_FORM
import com.memex.im.core.InitEntry.TO_LOGIN
import com.memex.im.core.Route.INIT
import com.memex.im.core.Route.LOGIN
import com.memex.im.core.Route.MAIN
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * R17/R18 门禁核心：未初始化进不了登录后的任何界面，未登录进不了主界面。
 * MAIN 的前置条件＝isInitialized && isLoggedIn，缺一不可（无匿名路径）。
 */
class RouteGuardTest {

    private class MemStore : InitStore {
        var addr: ServerAddress? = null
        override fun isInitialized(): Boolean = addr != null
        override fun serverAddress(): ServerAddress? = addr
        override fun markInitialized(address: ServerAddress) {
            addr = address
        }
    }

    @Test
    fun `全新安装 → 初始化向导`() {
        assertEquals(INIT, RouteGuard.next(MemStore(), SessionState()))
    }

    @Test
    fun `已初始化未登录 → 登录页`() {
        val store = MemStore().apply { markInitialized(ServerAddress("10.0.0.2", 24360)) }
        assertEquals(LOGIN, RouteGuard.next(store, SessionState()))
    }

    @Test
    fun `初始化加登录齐备 → 主界面`() {
        val store = MemStore().apply { markInitialized(ServerAddress("10.0.0.2", 24360)) }
        val session = SessionState().apply { signIn("zhangsan", "张三") }
        assertEquals(MAIN, RouteGuard.next(store, session))
    }

    @Test
    fun `全组合下 MAIN 仅在双条件成立时出现`() {
        for (initialized in listOf(false, true)) {
            for (loggedIn in listOf(false, true)) {
                val store = MemStore()
                if (initialized) store.markInitialized(ServerAddress("10.0.0.2", 24360))
                val session = SessionState()
                if (loggedIn) session.signIn("a", "A")
                val expected = when {
                    !initialized -> INIT
                    !loggedIn -> LOGIN
                    else -> MAIN
                }
                assertEquals(expected, RouteGuard.next(store, session))
            }
        }
    }

    @Test
    fun `退出登录后回落登录页而非主界面`() {
        val store = MemStore().apply { markInitialized(ServerAddress("10.0.0.2", 24360)) }
        val session = SessionState().apply { signIn("zhangsan", "张三") }
        assertEquals(MAIN, RouteGuard.next(store, session))
        session.signOut()
        assertEquals(LOGIN, RouteGuard.next(store, session))
    }

    @Test
    fun `向导不可作为绕过登录的回头路`() {
        val fresh = MemStore()
        assertEquals(SHOW_FORM, InitGate.entry(fresh, reconfigure = false))
        assertEquals(SHOW_FORM, InitGate.entry(fresh, reconfigure = true))

        val done = MemStore().apply { markInitialized(ServerAddress("10.0.0.2", 24360)) }
        // 常规进入已初始化的向导 → 改道登录页
        assertEquals(TO_LOGIN, InitGate.entry(done, reconfigure = false))
        // 仅显式「修改服务器地址」再出示表单，且保存仍须过连通性校验（UI 层）
        assertEquals(SHOW_FORM, InitGate.entry(done, reconfigure = true))
    }

    @Test
    fun `校验通过保存后的去向是登录页`() {
        val store = MemStore()
        assertEquals(INIT, InitGate.afterSaved(store))
        store.markInitialized(ServerAddress("10.0.0.2", 24360))
        assertEquals(LOGIN, InitGate.afterSaved(store))
    }
}
