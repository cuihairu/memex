package com.memex.im.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.EditText
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.memex.im.MemexApp
import com.memex.im.R
import com.memex.im.core.ChatHolder
import com.memex.im.core.ChatManager
import com.memex.im.core.NoticeGrade
import com.memex.im.core.Session
import com.memex.im.core.StoredMessage
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 聊天窗（T6.3 单聊；群消息可收（peer=group:N），群 UI 随后续块）。
 * 对齐 prototypes/mobile-chat.html：会话头（返回＋头像＋名称＋状态＋更多）、
 * 左对方/右己方气泡（--brand-fill #b4471b AA）、输入区（归档注＋发送）、底部四页导航。
 * 消息来自 ChatManager 本地库（收到即落库），发送走长连接；列表按时间正序。
 * 本页不拥有连接：连接随登录页移交 ChatHolder，本页只订阅。
 */
class ChatActivity : AppCompatActivity(), ChatManager.Listener {

    private lateinit var peer: String
    private lateinit var lv: ListView
    private lateinit var etInput: EditText
    private val msgAdapter = MessagesAdapter()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        peer = intent.getStringExtra(EXTRA_PEER) ?: run {
            finish()
            return
        }
        setContentView(R.layout.activity_chat)
        supportActionBar?.hide() // 自绘会话头，对齐原型（无 ActionBar）

        lv = findViewById(R.id.lv_messages)
        etInput = findViewById(R.id.et_input)
        lv.adapter = msgAdapter

        bindHeader()

        findViewById<ImageButton>(R.id.btn_send).setOnClickListener {
            val text = etInput.text.toString().trim()
            if (text.isEmpty()) return@setOnClickListener
            val mgr = ChatHolder.manager
            if (mgr == null) {
                Toast.makeText(this, R.string.chat_not_connected, Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            val seq = mgr.sendText(peer, text)
            if (seq == 0L) {
                Toast.makeText(this, R.string.chat_not_connected, Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            etInput.setText("")
            render()
        }

        bindTabbar()
    }

    /** 会话头：返回、头像、名称、状态副行（只放真实可得数据）、更多 */
    private fun bindHeader() {
        findViewById<ImageButton>(R.id.btn_back).setOnClickListener { finish() }
        findViewById<ImageButton>(R.id.btn_more).setOnClickListener {
            Toast.makeText(this, R.string.chat_not_connected, Toast.LENGTH_SHORT).show()
        }
        val name = displayName(peer)
        findViewById<TextView>(R.id.tv_peer_name).text = name
        findViewById<TextView>(R.id.tv_peer_sub).text = peer // 无部门/在线态数据，如实列对端账号
        findViewById<TextView>(R.id.tv_peer_avatar).apply {
            text = name.firstOrNull()?.toString() ?: "?"
            backgroundTintList = avatarTint(peer)
        }
    }

    /** 底部导航：会话→回列表；文件/我→对应页；通讯录未实现（同桌面置灰口径） */
    private fun bindTabbar() {
        findViewById<TextView>(R.id.tab_sessions).setOnClickListener { finish() }
        findViewById<TextView>(R.id.tab_files).setOnClickListener {
            startActivity(Intent(this, FileAssistantActivity::class.java))
        }
        findViewById<TextView>(R.id.tab_me).setOnClickListener {
            startActivity(Intent(this, MeActivity::class.java))
        }
        findViewById<TextView>(R.id.tab_contacts).setOnClickListener {
            Toast.makeText(this, R.string.nav_contacts_not_impl, Toast.LENGTH_SHORT).show()
        }
    }

    override fun onResume() {
        super.onResume()
        (application as? MemexApp)?.openPeer = peer // 前台推送裁决：正看该会话不弹横幅
        ChatHolder.manager?.let { mgr ->
            mgr.addListener(this)
            mgr.store.markRead(peer) // 进会话即清未读（列表角标同步）
            render()
        }
    }

    override fun onPause() {
        super.onPause()
        (application as? MemexApp)?.openPeer = null
        ChatHolder.manager?.removeListener(this)
    }

    // —— ChatManager.Listener（UI 线程）——
    override fun onNewMessage(p: String) {
        if (p == peer) render()
    }

    /** 站内通知：弹窗由常驻 NotificationHelper 裁决，本页只随 onNewMessage 刷新 */
    override fun onNotice(
        peer: String,
        grade: NoticeGrade,
        title: String,
        content: String,
        jumpUrl: String,
        msgId: String,
    ) = Unit

    override fun onSent(seq: Long) = Unit

    override fun onOffline(cause: String) {
        Toast.makeText(this, getString(R.string.err_io, cause), Toast.LENGTH_SHORT).show()
    }

    override fun onKicked(reason: String) {
        ChatHolder.clear()
        Session.instance.signOut()
        Toast.makeText(this, reason, Toast.LENGTH_LONG).show()
        finish()
    }

    private fun render() {
        val hist = ChatHolder.manager?.store?.history(peer) ?: emptyList()
        msgAdapter.update(hist)
        lv.post { lv.setSelection(lv.count - 1) } // 滚到最新
    }

    private fun displayName(p: String): String =
        if (p.startsWith("group:")) getString(R.string.group_display, p) else p

    /** 头像身份色（prototypes.css .avatar.v2–v7，AA 值），按对端账号哈希取色 */
    private fun avatarTint(name: String) = run {
        val palette = intArrayOf(
            R.color.avatar_v2, R.color.avatar_v3, R.color.avatar_v4,
            R.color.avatar_v5, R.color.avatar_v6, R.color.avatar_v7,
        )
        androidx.core.content.ContextCompat.getColorStateList(
            this, palette[Math.floorMod(name.hashCode(), palette.size)]
        )
    }

    /** 消息行：左对方（头像＋发送者＋时间＋白泡）/ 右己方（反转＋时间＋品牌泡） */
    private inner class MessagesAdapter : BaseAdapter() {
        private var items: List<StoredMessage> = emptyList()

        fun update(list: List<StoredMessage>) {
            items = list
            notifyDataSetChanged()
        }

        override fun getCount() = items.size
        override fun getItem(position: Int): StoredMessage? =
            if (position in items.indices) items[position] else null

        override fun getItemId(position: Int) = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val view = convertView ?: layoutInflater.inflate(
                R.layout.item_chat_message, parent, false
            )
            val m = items[position]
            val mine = m.from == Session.instance.account
            val who = if (mine) getString(R.string.chat_me) else m.from

            // 反转：己方靠右（.msg.mine flex-direction:row-reverse + align-end）
            (view as LinearLayout).layoutDirection =
                if (mine) View.LAYOUT_DIRECTION_LTR else View.LAYOUT_DIRECTION_RTL

            val avatar = view.findViewById<TextView>(R.id.tv_avatar)
            avatar.text = who.firstOrNull()?.toString() ?: "?"
            avatar.backgroundTintList = avatarTint(who)

            val sender = view.findViewById<TextView>(R.id.tv_sender)
            // 己方不列发送者（原型仅列时间）
            sender.text = if (mine) "" else who

            val time = view.findViewById<TextView>(R.id.tv_time)
            time.text = fmtTime(m.tsMs)

            val bubble = view.findViewById<TextView>(R.id.tv_bubble)
            bubble.text = if (m.recalled) getString(R.string.chat_recalled) else m.text
            bubble.setBackgroundResource(if (mine) R.drawable.bubble_out else R.drawable.bubble_in)
            bubble.setTextColor(
                getColor(if (mine) android.R.color.white else R.color.text_primary)
            )
            bubble.alpha = if (m.recalled) 0.4f else 1f
            bubble.setOnLongClickListener {
                val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                cm.setPrimaryClip(ClipData.newPlainText("memex", m.text))
                Toast.makeText(this@ChatActivity, R.string.chat_copied, Toast.LENGTH_SHORT).show()
                true
            }
            return view
        }

        private fun fmtTime(tsMs: Long): String = try {
            SimpleDateFormat("HH:mm", Locale.getDefault()).format(Date(tsMs))
        } catch (_: Exception) {
            ""
        }
    }

    companion object {
        const val EXTRA_PEER = "peer"
    }
}
