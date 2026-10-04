package com.memex.im.ui

import android.app.NotificationManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/**
 * 紧急通知「确认收悉」动作（T6.3 第三块）：点按后才允许滑除该横幅，
 * 对齐桌面「点『确认收悉』后关闭，未确认的通知会继续排队弹出」。
 */
class MemexNotifyReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != NotificationHelper.ACTION_NOTICE_CONFIRM) return
        val id = intent.getIntExtra(NotificationHelper.EXTRA_NOTIF_ID, -1)
        if (id < 0) return
        val sys = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        sys.cancel(id)
    }
}
