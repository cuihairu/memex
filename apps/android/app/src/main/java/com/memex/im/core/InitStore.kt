package com.memex.im.core

/**
 * 初始化状态存取（R17）。接口化以便纯 JVM 单测用内存实现。
 * Android 侧实现见 [com.memex.im.core.PrefsInitStore]（SharedPreferences）。
 */
interface InitStore {
    /** 是否已完成「服务器地址设置 + 连通性校验通过」 */
    fun isInitialized(): Boolean

    /** 已初始化时返回地址；未初始化返回 null */
    fun serverAddress(): ServerAddress?

    /** 校验通过后落盘（UI 层保证只在探测成功后调用） */
    fun markInitialized(address: ServerAddress)
}

/** 应用三个可达界面；守卫输出唯一去向 */
enum class Route { INIT, LOGIN, MAIN }

/**
 * 路由守卫（R17/R18 的门禁核心）：
 * - 未完成初始化 → 只能去向初始化向导；
 * - 已初始化未登录 → 只能去向登录页（移动端无免登录/匿名形态）；
 * - 两者齐备 → 主界面。
 * 换言之：MAIN 的前置条件是 isInitialized && isLoggedIn，缺一不可。
 */
object RouteGuard {
    fun next(store: InitStore, session: SessionState): Route =
        if (!store.isInitialized()) Route.INIT
        else if (!session.isLoggedIn) Route.LOGIN
        else Route.MAIN
}

/** 初始化页自身的入口判定 */
enum class InitEntry { SHOW_FORM, TO_LOGIN }

object InitGate {
    /**
     * 已初始化的常规进入一律改道登录页（向导不可作为回头路绕过登录）；
     * 仅登录页「修改服务器地址」显式要求重设时（reconfigure）再出示表单，
     * 且重设同样必须通过连通性校验才能保存。
     */
    fun entry(store: InitStore, reconfigure: Boolean): InitEntry =
        if (store.isInitialized() && !reconfigure) InitEntry.TO_LOGIN else InitEntry.SHOW_FORM

    /** 校验通过并保存后的去向：必然是登录页 */
    fun afterSaved(store: InitStore): Route =
        if (store.isInitialized()) Route.LOGIN else Route.INIT
}
