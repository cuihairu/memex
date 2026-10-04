package com.memex.im.ui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.BaseAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.memex.im.R
import com.memex.im.core.ChatHolder
import com.memex.im.core.ChatManager
import com.memex.im.core.Conversation
import com.memex.im.core.NoticeGrade
import com.memex.im.core.PrefsInitStore
import com.memex.im.core.Route
import com.memex.im.core.RouteGuard
import com.memex.im.core.Session
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 主界面＝会话列表（T6.3 会话列表与收发）。
 * onCreate 先过 RouteGuard（未初始化→向导；未登录→登录页）。
 * 会话数据来自 ChatHolder 本地库聚合；长连接由登录页移交，本页只订阅展示。
 * 新建聊天：输入对端账号（单聊）；群功能随后续块。
 */
class MainActivity : AppCompatActivity(), ChatManager.Listener {

    private lateinit var lv: ListView
    private lateinit var tvEmpty: TextView
    private lateinit var etPeer: EditText
    private val convsAdapter = ConversationsAdapter()

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
        etPeer = findViewById(R.id.et_peer)

        findViewById<Button>(R.id.btn_new_chat).setOnClickListener {
            val peer = etPeer.text.toString().trim()
            if (peer.isEmpty()) {
                Toast.makeText(this, R.string.main_peer_hint, Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            etPeer.setText("")
            openChat(peer)
        }

        findViewById<Button>(R.id.btn_logout).setOnClickListener {
            ChatHolder.manager?.removeListener(this)
            ChatHolder.clear()
            Session.instance.signOut()
            startActivity(Intent(this, LoginActivity::class.java))
            finish()
        }

        lv.adapter = convsAdapter
        lv.onItemClickListener =
            AdapterView.OnItemClickListener { _, _, position, _ ->
                val conv = convsAdapter.getItem(position)
                if (conv != null) openChat(conv.peer)
            }

        requestNotificationPermissionIfNeeded()
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
        super.onPause()
        ChatHolder.manager?.removeListener(this)
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
        val convs = ChatHolder.manager?.store?.conversations() ?: emptyList()
        convsAdapter.update(convs)
        tvEmpty.visibility = if (convs.isEmpty()) View.VISIBLE else View.GONE
    }

    private fun openChat(peer: String) {
        startActivity(
            Intent(this, ChatActivity::class.java)
                .putExtra(ChatActivity.EXTRA_PEER, peer)
        )
    }

    /** 会话列表行渲染：标题（对端/群名）＋ 预览 ＋ 时间 ＋ 未读角标 */
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
                android.R.layout.simple_list_item_2, parent, false
            )
            val conv = items[position]
            view.findViewById<TextView>(android.R.id.text1).text =
                displayName(conv.peer) + unreadSuffix(conv.unread)
            view.findViewById<TextView>(android.R.id.text2).text =
                conv.lastText + "\n" + fmtTime(conv.lastTsMs)
            return view
        }

        private fun displayName(peer: String): String =
            if (peer.startsWith("group:")) getString(R.string.group_display, peer)
            else peer

        private fun unreadSuffix(unread: Int): String =
            if (unread > 0) getString(R.string.unread_count, unread) else ""

        private fun fmtTime(tsMs: Long): String = try {
            SimpleDateFormat("MM-dd HH:mm", Locale.getDefault()).format(Date(tsMs))
        } catch (_: Exception) {
            ""
        }
    }

    companion object {
        private const val REQ_NOTIF_PERM = 1
    }
}