package com.memex.im

import android.app.Activity
import android.app.Application
import android.os.Bundle
import com.memex.im.core.ChatHolder
import com.memex.im.ui.NotificationHelper

/**
 * Application（T6.3 第三块·消息推送）：
 * - 通知渠道初始化（普通消息/重要/紧急三渠道，震动与声音策略随渠道）；
 * - 常驻通知器挂载（ChatHolder，跨重新登录保持；三级裁决见 NotificationHelper）；
 * - 前后台与当前会话跟踪（后台 TEXT 推送的裁决依据：前台站内渲染不重复弹）。
 */
class MemexApp : Application() {
    /** 有可见 Activity 即前台（ActivityLifecycleCallbacks 全在主线程回调） */
    var foreground: Boolean = false
        private set

    /** 当前打开的会话（ChatActivity onResume/onPause 维护）；正看该会话则不弹推送 */
    var openPeer: String? = null

    override fun onCreate() {
        super.onCreate()
        registerActivityLifecycleCallbacks(object : ActivityLifecycleCallbacks {
            private var started = 0

            override fun onActivityStarted(activity: Activity) {
                started++
                foreground = started > 0
            }

            override fun onActivityStopped(activity: Activity) {
                started = (started - 1).coerceAtLeast(0)
                foreground = started > 0
            }

            override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) = Unit
            override fun onActivityResumed(activity: Activity) = Unit
            override fun onActivityPaused(activity: Activity) = Unit
            override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) = Unit
            override fun onActivityDestroyed(activity: Activity) = Unit
        })
        NotificationHelper(this).let { helper ->
            helper.ensureChannels()
            ChatHolder.attachNotifier(helper)
        }
    }
}
