package com.memex.im.ui

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationCompat
import com.memex.im.MemexApp
import com.memex.im.R
import com.memex.im.core.ChatHolder
import com.memex.im.core.ChatManager
import com.memex.im.core.NoticeGrade

/**
 * 消息推送横幅＋震动（T6.3 第三块，对齐桌面 T4.10 三级通知语义）：
 * - TEXT 消息：仅应用在后台（或非当前打开会话）时横幅＋声音＋震动——前台
 *   站内渲染已覆盖，不重复打扰（移动端适配）；
 * - NOTICE 普通（NORMAL）：仅站内会话消息（已落库渲染），不弹——对齐桌面
 *   「普通通知弹窗默认关＝仅站内会话消息」；
 * - NOTICE 重要（IMPORTANT）：横幅＋声音＋震动强提醒，不看应用前后台——
 *   对齐桌面「重要通知桌面提醒（强提醒，不看窗口激活态）」；
 * - NOTICE 紧急（URGENT）：横幅＋声音＋震动＋需确认收悉——setOngoing 常驻
 *   不可滑除，点「确认收悉」动作后才消失（对齐桌面置顶弹窗确认，未确认
 *   的通知继续排队弹出）。
 */
class NotificationHelper(private val ctx: Context) : ChatManager.Listener {

    companion object {
        const val CH_MESSAGE = "memex_message"
        const val CH_IMPORTANT = "memex_notice_important"
        const val CH_URGENT = "memex_notice_urgent"
        const val ACTION_NOTICE_CONFIRM = "com.memex.im.NOTICE_CONFIRM"
        const val EXTRA_MSG_ID = "msg_id"
        const val EXTRA_NOTIF_ID = "notif_id"

        private val VIBRATE_PATTERN = longArrayOf(0, 300, 200, 300)

        /** 通知 id：同一 msg_id 稳定映射（重复投递更新原横幅而非叠条）。 */
        private fun notifId(msgId: String): Int =
            if (msgId.isEmpty()) ((System.nanoTime() and 0x7FFFFFFF).toInt()) else msgId.hashCode()
    }

    /** 建通知渠道（幂等；Application onCreate 调一次）。 */
    fun ensureChannels() {
        val sys = notificationService()
        val message = NotificationChannel(
            CH_MESSAGE, ctx.getString(R.string.channel_message),
            NotificationManager.IMPORTANCE_HIGH,
        ).apply {
            enableVibration(true)
            vibrationPattern = VIBRATE_PATTERN
        }
        val important = NotificationChannel(
            CH_IMPORTANT, ctx.getString(R.string.channel_important),
            NotificationManager.IMPORTANCE_HIGH,
        ).apply {
            enableVibration(true)
            vibrationPattern = VIBRATE_PATTERN
        }
        val urgent = NotificationChannel(
            CH_URGENT, ctx.getString(R.string.channel_urgent),
            NotificationManager.IMPORTANCE_HIGH,
        ).apply {
            enableVibration(true)
            vibrationPattern = VIBRATE_PATTERN
        }
        sys.createNotificationChannels(listOf(message, important, urgent))
    }

    // —— ChatManager.Listener（UI 线程） ——

    override fun onNewMessage(peer: String) {
        val app = ctx.applicationContext as? MemexApp ?: return
        if (app.foreground && peer == app.openPeer) return // 正看该会话，站内渲染即可
        val mgr = ChatHolder.manager ?: return
        val last = mgr.store.history(peer, 1).lastOrNull() ?: return
        if (last.msgId.isEmpty()) return // 自己刚发的受理暂存不推
        val n = NotificationCompat.Builder(ctx, CH_MESSAGE)
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentTitle(displayName(peer))
            .setContentText(last.text)
            .setStyle(NotificationCompat.BigTextStyle().bigText(last.text))
            .setAutoCancel(true)
            .setContentIntent(
                PendingIntent.getActivity(
                    ctx, notifId(last.msgId),
                    Intent(ctx, ChatActivity::class.java)
                        .putExtra(ChatActivity.EXTRA_PEER, peer),
                    PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
                )
            )
            .build()
        post(notifId(last.msgId), n)
    }

    override fun onNotice(
        peer: String,
        grade: NoticeGrade,
        title: String,
        content: String,
        jumpUrl: String,
        msgId: String,
    ) {
        val id = notifId(msgId)
        when (grade) {
            NoticeGrade.NORMAL -> Unit // 普通＝仅站内会话消息，不弹（桌面默认偏好同）
            NoticeGrade.IMPORTANT -> post(
                id,
                NotificationCompat.Builder(ctx, CH_IMPORTANT)
                    .setSmallIcon(R.mipmap.ic_launcher)
                    .setContentTitle(title)
                    .setContentText(content)
                    .setStyle(NotificationCompat.BigTextStyle().bigText(content))
                    .setAutoCancel(true)
                    .setContentIntent(mainPendingIntent())
                    .build(),
            )
            NoticeGrade.URGENT -> {
                val confirm = PendingIntent.getBroadcast(
                    ctx, id,
                    Intent(ctx, MemexNotifyReceiver::class.java)
                        .setAction(ACTION_NOTICE_CONFIRM)
                        .putExtra(EXTRA_NOTIF_ID, id)
                        .putExtra(EXTRA_MSG_ID, msgId),
                    PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
                )
                post(
                    id,
                    NotificationCompat.Builder(ctx, CH_URGENT)
                        .setSmallIcon(R.mipmap.ic_launcher)
                        .setContentTitle(title)
                        .setContentText(content)
                        .setStyle(NotificationCompat.BigTextStyle().bigText(content))
                        .setOngoing(true) // 需确认收悉：确认前不可滑除
                        .setContentIntent(mainPendingIntent())
                        .addAction(0, ctx.getString(R.string.notify_confirm), confirm)
                        .build(),
                )
            }
        }
    }

    override fun onSent(seq: Long) = Unit
    override fun onOffline(cause: String) = Unit
    override fun onKicked(reason: String) = Unit

    // —— 内部 ——

    private fun post(id: Int, notification: Notification) {
        if (!canPost()) return // Android 13+ 未授权通知权限：静默降级为站内
        notificationService().notify(id, notification)
    }

    private fun canPost(): Boolean =
        Build.VERSION.SDK_INT < 33 ||
            ctx.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) ==
            PackageManager.PERMISSION_GRANTED

    private fun notificationService(): NotificationManager =
        ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

    private fun mainPendingIntent(): PendingIntent =
        PendingIntent.getActivity(
            ctx, 0, Intent(ctx, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )

    private fun displayName(peer: String): String =
        if (peer.startsWith("group:")) ctx.getString(R.string.group_display, peer) else peer
}
