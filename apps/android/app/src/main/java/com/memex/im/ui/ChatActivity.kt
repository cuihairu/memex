package com.memex.im.ui

import android.os.Bundle
import android.view.View
import android.view.ViewGroup
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
import com.memex.im.core.Session
import com.memex.im.core.StoredMessage
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 聊天窗（T6.3 单聊；群消息可收（peer=group:N），群 UI 随后续块）。
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
        setTitle(displayName(peer))
        setContentView(R.layout.activity_chat)

        lv = findViewById(R.id.lv_messages)
        etInput = findViewById(R.id.et_input)
        lv.adapter = msgAdapter

        findViewById<Button>(R.id.btn_send).setOnClickListener {
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
    override fun onNewMessage(p: String) {
        if (p == peer) render()
    }

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

    /** 消息行：时间+发送者 与 内容（撤回标灰） */
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
                android.R.layout.simple_list_item_2, parent, false
            )
            val m = items[position]
            val mine = m.from == Session.instance.account
            val who = if (mine) getString(R.string.chat_me) else m.from
            view.findViewById<TextView>(android.R.id.text1).text = "$who ${fmtTime(m.tsMs)}"
            view.findViewById<TextView>(android.R.id.text2).apply {
                text = if (m.recalled) getString(R.string.chat_recalled) else m.text
                if (m.recalled) alpha = 0.4f else alpha = 1f
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