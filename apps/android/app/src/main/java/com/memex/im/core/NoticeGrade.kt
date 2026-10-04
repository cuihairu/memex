package com.memex.im.core

/**
 * 通知三级（T6.3 消息推送，对齐桌面 T4.10 / proto Notice.Urgency）：
 * - NORMAL：普通＝仅站内会话消息（落库渲染，不弹横幅）；
 * - IMPORTANT：重要＝横幅强提醒（声音＋震动，不看应用前后台，桌面同口径）；
 * - URGENT：紧急＝横幅＋声音＋震动＋需确认收悉（确认前常驻不可滑除，对齐
 *   桌面「未确认的通知会继续排队弹出」）。
 */
enum class NoticeGrade {
    NORMAL, IMPORTANT, URGENT;

    companion object {
        /** proto Notice.Urgency 编号 → 分级（未指定/未识别按普通处理，对齐桌面）。 */
        fun fromProtoNumber(n: Int): NoticeGrade = when (n) {
            2 -> IMPORTANT
            3 -> URGENT
            else -> NORMAL
        }
    }
}

/**
 * 通知正文的归档形态（对齐桌面 compose_notice_text，服务端归档同源生成）：
 * 「标题：正文[ 跳转]」（全角冒号），气泡按单行渲染，跳转随文留痕。
 */
fun composeNoticeText(title: String, content: String, jumpUrl: String): String {
    val sb = StringBuilder(title).append("：").append(content)
    if (jumpUrl.isNotEmpty()) sb.append(' ').append(jumpUrl)
    return sb.toString()
}
