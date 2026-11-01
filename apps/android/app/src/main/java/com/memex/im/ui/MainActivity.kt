package com.memex.im.ui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.BaseAdapter
import android.widget.EditText
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.memex.im.R
import com.memex.im.core.ChatHolder
import com.memex.im.core.ChatManager
import com.memex.im.core.Conversation
import com.memex.im.core.NoticeGrade
import com.memex.im.core.PrefsInitStore
import com.memex.im.core.Route
import com.memex.im.core.RouteGuard
import com.memex.im.core.ServerAddress
import com.memex.im.core.Session
import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Date
import java.util.Locale

/**
 * 主界面＝会话列表（T6.3 会话列表与收发，原型 mobile-sessions）。
 * onCreate 先过 RouteGuard（未初始化→向导；未登录→登录页）。
 * 会话数据来自 ChatHolder 本地库聚合；长连接由登录页移交，本页只订阅展示。
 * 新建聊天：appbar「＋」开对话框输入对端账号（单聊）；群功能随后续块。
 * 搜索框为本会话列表过滤（联系人/群聊；消息级检索是桌面审计面，不在此）。
 */
class MainActivity : AppCompatActivity(), ChatManager.Listener {

    private lateinit var lv: ListView
    private lateinit var tvEmpty: TextView
    private lateinit var etSearch: EditText
    private val convsAdapter = ConversationsAdapter()

    /** 全量会话（搜索框按显示名/预览过滤显示面） */
    private var allConvs: List<Conversation> = emptyList()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val store = PrefsInitStore(this)
        when (RouteGuard.next(store, Session.instance)) {
            Route.INIT -> {
                startActivity(Intent(this, InitActivity::class.java))
                finish()
                return
            }
            Route.LOGIN -> {
                startActivity(Intent(this, LoginActivity::class.java))
                finish()
                return
            }
            Route.MAIN -> Unit
        }

        setContentView(R.layout.activity_main)
        lv = findViewById(R.id.lv_conversations)
        tvEmpty = findViewById(R.id.tv_empty)
        etSearch = findViewById(R.id.et_search)

        // appbar 副行：协作态 · 服务器 · 全量归档（默认端口只显主机名）
        findViewById<TextView>(R.id.main_state_sub).text = getString(
            R.string.main_state_sub,
            store.serverAddress()?.let { shownHost(it) } ?: "?",
        )

        findViewById<View>(R.id.btn_new_chat).setOnClickListener { showNewChatDialog() }

        // 四页导航：会话（本页 active）；邮箱/任务/我真跳转
        findViewById<View>(R.id.tab_mailbox).setOnClickListener {
            startActivity(Intent(this, MailboxActivity::class.java))
        }
        findViewById<View>(R.id.tab_tasks).setOnClickListener {
            startActivity(Intent(this, TasksActivity::class.java))
        }
        findViewById<View>(R.id.tab_me).setOnClickListener {
            startActivity(Intent(this, MeActivity::class.java))
        }

        etSearch.addTextChangedListener(object : android.text.TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) = Unit
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) = Unit
            override fun afterTextChanged(s: android.text.Editable?) = render()
        })

        lv.adapter = convsAdapter
        lv.onItemClickListener =
            AdapterView.OnItemClickListener { _, _, position, _ ->
                val conv = convsAdapter.getItem(position)
                if (conv != null) openChat(conv.peer)
            }

        requestNotificationPermissionIfNeeded()
    }

    /** 发起聊天：输入对端账号（单聊）。原型 appbar「＋」的落地面 */
    private fun showNewChatDialog() {
        val input = EditText(this).apply { hint = getString(R.string.main_peer_hint) }
        val dp = (resources.displayMetrics.density * 16).toInt()
        val pad = (resources.displayMetrics.density * 4).toInt()
        input.setPadding(dp + pad, pad, dp + pad, pad)
        AlertDialog.Builder(this)
            .setTitle(R.string.main_new_chat_title)
            .setView(input)
            .setNegativeButton(R.string.files_cancel, null)
            .setPositiveButton(R.string.main_new_chat) { _, _ ->
                val peer = input.text.toString().trim()
                if (peer.isEmpty()) {
                    Toast.makeText(this, R.string.main_peer_hint, Toast.LENGTH_SHORT).show()
                } else {
                    openChat(peer)
                }
            }
            .show()
    }

    /** Android 13+ 通知权限：推送横幅需授权；未授权静默降级为站内（不阻塞主流程） */
    private fun requestNotificationPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT < 33) return
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) ==
            PackageManager.PERMISSION_GRANTED
        ) {
            return
        }
        requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQ_NOTIF_PERM)
    }

    override fun onResume() {
        super.onResume()
        ChatHolder.manager?.let { mgr ->
            mgr.addListener(this)
            render()
        }
    }

    override fun onPause() {
        ChatHolder.manager?.removeListener(this)
        super.onPause()
    }

    // —— ChatManager.Listener（UI 线程）——
    override fun onNewMessage(peer: String) = render()

    /** 站内通知：弹窗由常驻 NotificationHelper 裁决，列表随 onNewMessage 刷新 */
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
        startActivity(Intent(this, LoginActivity::class.java))
        finish()
    }

    private fun render() {
        allConvs = ChatHolder.manager?.store?.conversations() ?: emptyList()
        applyFilter()
    }

    /** 搜索框过滤显示面（显示名/预览子串，空串＝全量） */
    private fun applyFilter() {
        val q = etSearch.text.toString().trim()
        val shown = if (q.isEmpty()) allConvs else allConvs.filter {
            displayName(it.peer).contains(q, ignoreCase = true) ||
                it.lastText.contains(q, ignoreCase = true)
        }
        convsAdapter.update(shown)
        tvEmpty.visibility = if (shown.isEmpty()) View.VISIBLE else View.GONE
    }

    private fun openChat(peer: String) {
        startActivity(
            Intent(this, ChatActivity::class.java)
                .putExtra(ChatActivity.EXTRA_PEER, peer)
        )
    }

    /** 展示用服务器标识：默认端口只显域名/IP，非默认端口带端口（同「我」屏） */
    private fun shownHost(address: ServerAddress): String =
        if (address.port == ServerAddress.DEFAULT_PORT) address.host else address.display()

    private fun displayName(peer: String): String =
        if (peer.startsWith("group:")) getString(R.string.group_display, peer) else peer

    /** 原型头像七色盘（prototypes.css .avatar.v2–v8），按账号哈希稳定取色 */
    private val avatarPalette = intArrayOf(
        Color.parseColor("#7c5cbf"),
        Color.parseColor("#237373"),
        Color.parseColor("#965c11"),
        Color.parseColor("#4d6fa8"),
        Color.parseColor("#a84a6c"),
        Color.parseColor("#3f7439"),
        Color.parseColor("#8a6d4f"),
    )

    /** 会话行渲染：40dp 圆角头像（首字＋稳定底色）＋名称/时间＋预览＋未读角标 */
    private inner class ConversationsAdapter : BaseAdapter() {
        private var items: List<Conversation> = emptyList()

        fun update(list: List<Conversation>) {
            items = list
            notifyDataSetChanged()
        }

        override fun getCount() = items.size
        override fun getItem(position: Int): Conversation? =
            if (position in items.indices) items[position] else null

        override fun getItemId(position: Int) = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val view = convertView ?: layoutInflater.inflate(
                R.layout.item_conversation, parent, false
            )
            val conv = items[position]
            val name = displayName(conv.peer)
            val avatar = view.findViewById<TextView>(R.id.ses_avatar)
            avatar.text = name.take(1).ifEmpty { "?" }
            avatar.backgroundTintList = ColorStateList.valueOf(
                avatarPalette[Math.floorMod(conv.peer.hashCode(), avatarPalette.size)],
            )
            view.findViewById<TextView>(R.id.ses_name).text = name
            view.findViewById<TextView>(R.id.ses_time).text = fmtTime(conv.lastTsMs)
            view.findViewById<TextView>(R.id.ses_sub).text = conv.lastText
            val unread = view.findViewById<TextView>(R.id.ses_unread)
            if (conv.unread > 0) {
                unread.text = conv.unread.toString()
                unread.visibility = View.VISIBLE
            } else {
                unread.visibility = View.GONE
            }
            return view
        }

        /** 时间粒度对齐原型：今天 HH:mm／昨天「昨天」／更早 MM-dd */
        private fun fmtTime(tsMs: Long): String {
            val cal = Calendar.getInstance()
            cal.time = Date(tsMs)
            val now = Calendar.getInstance()
            return when {
                cal.get(Calendar.YEAR) == now.get(Calendar.YEAR) &&
                    cal.get(Calendar.DAY_OF_YEAR) == now.get(Calendar.DAY_OF_YEAR) ->
                    SimpleDateFormat("HH:mm", Locale.getDefault()).format(Date(tsMs))
                run {
                    val y = (now.clone() as Calendar).apply { add(Calendar.DAY_OF_YEAR, -1) }
                    cal.get(Calendar.YEAR) == y.get(Calendar.YEAR) &&
                        cal.get(Calendar.DAY_OF_YEAR) == y.get(Calendar.DAY_OF_YEAR)
                } -> getString(R.string.main_time_yesterday)
                else -> SimpleDateFormat("MM-dd", Locale.getDefault()).format(Date(tsMs))
            }
        }
    }

    companion object {
        private const val REQ_NOTIF_PERM = 1
    }
}
