package com.memex.im.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.BaseAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.memex.im.R
import com.memex.im.core.FilesClient
import com.memex.im.core.PrefsInitStore
import com.memex.im.core.Session
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * 邮箱·移动端（原型 mobile-mailbox）：统一收件箱＝告警/通知/公告三类同箱。
 * 邮箱＝视图＋已读状态，不是新通道——条目只 pull 不 push；清空/标记已读
 * 只动邮箱状态，永不触归档行（硬边界文案在抽屉底部常驻）。
 * 数据面全部走 core [FilesClient]（阻塞式），本页只做线程搬运与展示；
 * 连接面板与文件助手同模板（文件面端口独立 24561，口令不落盘）。
 */
class MailboxActivity : AppCompatActivity() {

    private val executor: ExecutorService = Executors.newSingleThreadExecutor()
    private var client: FilesClient? = null

    /** 当前类目页签（""=全部；与只看未读正交） */
    private var filterType: String = ""

    private var unreadOnly: Boolean = false

    /** 最近一次拉到的列表与四路计数（chips 文案与徽章同源） */
    private var items: List<FilesClient.MailboxItem> = emptyList()
    private var counts: FilesClient.MailboxCounts? = null

    private lateinit var tvBadge: TextView
    private lateinit var etAccount: EditText
    private lateinit var etPassword: EditText
    private lateinit var etPort: EditText
    private lateinit var btnConnect: Button
    private lateinit var tvState: TextView
    private lateinit var chipAll: TextView
    private lateinit var chipWarning: TextView
    private lateinit var chipNotice: TextView
    private lateinit var chipAnnounce: TextView
    private lateinit var chipUnread: TextView
    private lateinit var tvEmpty: TextView
    private val adapter = RowsAdapter()

    // 抽屉
    private lateinit var sheetWrap: View
    private lateinit var sheetIcon: TextView
    private lateinit var sheetTitle: TextView
    private lateinit var sheetMeta: TextView
    private lateinit var sheetKind: TextView
    private lateinit var sheetBody: TextView
    private lateinit var sheetTrace: TextView
    private lateinit var sheetLedgerHead: TextView
    private lateinit var sheetLedger: LinearLayout
    private lateinit var sheetConfirm: Button
    private var sheetItem: FilesClient.MailboxItem? = null

    private val timeFmt = SimpleDateFormat("MM-dd HH:mm", Locale.CHINA)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_mailbox)

        tvBadge = findViewById(R.id.mbx_badge)
        etAccount = findViewById(R.id.mbx_account)
        etPassword = findViewById(R.id.mbx_password)
        etPort = findViewById(R.id.mbx_port)
        btnConnect = findViewById(R.id.mbx_connect)
        tvState = findViewById(R.id.mbx_state)
        chipAll = findViewById(R.id.mbx_chip_all)
        chipWarning = findViewById(R.id.mbx_chip_warning)
        chipNotice = findViewById(R.id.mbx_chip_notice)
        chipAnnounce = findViewById(R.id.mbx_chip_announce)
        chipUnread = findViewById(R.id.mbx_chip_unread)
        tvEmpty = findViewById(R.id.mbx_empty)

        sheetWrap = findViewById(R.id.mbx_sheet_wrap)
        sheetIcon = findViewById(R.id.mbx_sheet_icon)
        sheetTitle = findViewById(R.id.mbx_sheet_title)
        sheetMeta = findViewById(R.id.mbx_sheet_meta)
        sheetKind = findViewById(R.id.mbx_sheet_kind)
        sheetBody = findViewById(R.id.mbx_sheet_body)
        sheetTrace = findViewById(R.id.mbx_sheet_trace)
        sheetLedgerHead = findViewById(R.id.mbx_sheet_ledger_head)
        sheetLedger = findViewById(R.id.mbx_sheet_ledger)
        sheetConfirm = findViewById(R.id.mbx_sheet_confirm)

        // 账号预填协作登录态（口令不回填——不落盘）
        Session.instance.account?.let { etAccount.setText(it) }
        etPort.setText(FilesClient.DEFAULT_FILES_PORT.toString())

        btnConnect.setOnClickListener { onConnectClicked() }
        findViewById<View>(R.id.mbx_back).setOnClickListener { finish() }
        findViewById<View>(R.id.mbx_sheet_dim).setOnClickListener { closeSheet() }
        sheetConfirm.setOnClickListener { onSheetConfirmClicked() }

        chipAll.setOnClickListener { switchType("") }
        chipWarning.setOnClickListener { switchType("warning") }
        chipNotice.setOnClickListener { switchType("notice") }
        chipAnnounce.setOnClickListener { switchType("announce") }
        chipUnread.setOnClickListener {
            unreadOnly = !unreadOnly
            renderChips()
            refresh()
        }

        // tabbar：会话＝返回主界面；任务/我＝换页（本页出栈）
        findViewById<View>(R.id.mbx_tab_sessions).setOnClickListener { finish() }
        findViewById<View>(R.id.mbx_tab_tasks).setOnClickListener {
            startActivity(Intent(this, TasksActivity::class.java))
            finish()
        }
        findViewById<View>(R.id.mbx_tab_me).setOnClickListener {
            startActivity(Intent(this, MeActivity::class.java))
            finish()
        }

        val lv = findViewById<ListView>(R.id.mbx_list)
        lv.adapter = adapter
        lv.onItemClickListener =
            AdapterView.OnItemClickListener { _, _, position, _ -> openSheet(items[position]) }
        renderChips()
    }

    override fun onDestroy() {
        executor.shutdownNow()
        super.onDestroy()
    }

    // —— 连接（与文件助手同模板） ——

    private fun onConnectClicked() {
        val c = client
        if (c?.isLoggedIn == true) {
            c.logout()
            client = null
            items = emptyList()
            counts = null
            adapter.update(emptyList())
            tvBadge.visibility = View.GONE
            setConnected(false)
            return
        }
        val account = etAccount.text.toString().trim()
        val password = etPassword.text.toString()
        val port = etPort.text.toString().toIntOrNull()
        if (account.isEmpty() || password.isEmpty()) {
            tvState.setText(R.string.files_err_need_account)
            return
        }
        if (port == null || port !in 1..65535) {
            tvState.setText(R.string.err_addr_bad_port)
            return
        }
        val host = PrefsInitStore(this).serverAddress()?.host ?: run {
            startActivity(Intent(this, InitActivity::class.java))
            finish()
            return
        }
        tvState.setText(R.string.login_busy)
        btnConnect.isEnabled = false
        executor.execute {
            var err: String? = null
            var internal: FilesClient? = null
            try {
                internal = FilesClient().also { it.login(host, port, account, password) }
            } catch (e: FilesClient.ApiException) {
                err = describe(e)
            } catch (e: java.io.IOException) {
                err = getString(R.string.err_unreachable)
            }
            runOnUiThread {
                btnConnect.isEnabled = true
                if (err != null) {
                    tvState.text = err
                    return@runOnUiThread
                }
                client = internal
                setConnected(true)
                refresh()
            }
        }
    }

    private fun setConnected(connected: Boolean) {
        btnConnect.setText(if (connected) R.string.files_disconnect else R.string.files_connect)
        tvState.setText(if (connected) R.string.files_state_on else R.string.files_state_off)
        etAccount.isEnabled = !connected
        etPassword.isEnabled = !connected
        etPort.isEnabled = !connected
        if (!connected) tvEmpty.visibility = View.GONE
    }

    /** 失败明示：401 会话过期单独引导重连；0＝不可达；其余带服务端文案 */
    private fun describe(e: java.io.IOException): String =
        when {
            e !is FilesClient.ApiException -> getString(R.string.err_unreachable)
            e.status == 401 -> getString(R.string.files_err_session)
            e.status == 0 -> getString(R.string.err_unreachable)
            else -> getString(R.string.files_err_prefix, e.status, e.error)
        }

    // —— 列表 ——

    private fun switchType(type: String) {
        if (filterType == type) return
        filterType = type
        renderChips()
        refresh()
    }

    private fun refresh() {
        val c = client ?: return
        val type = filterType
        val unread = unreadOnly
        btnConnect.isEnabled = false
        executor.execute {
            var err: String? = null
            var result: FilesClient.MailboxList? = null
            try {
                result = c.listMailbox(type = type, unreadOnly = unread)
            } catch (e: FilesClient.ApiException) {
                err = describe(e)
            } catch (e: java.io.IOException) {
                err = describe(e)
            }
            runOnUiThread {
                btnConnect.isEnabled = true
                if (err != null) {
                    tvState.text = err
                    return@runOnUiThread
                }
                items = result?.items ?: emptyList()
                counts = result?.counts
                adapter.update(items)
                renderChips()
                tvEmpty.visibility = if (items.isEmpty()) View.VISIBLE else View.GONE
            }
        }
    }

    private fun renderChips() {
        val c = counts
        setChipState(chipAll, filterType == "", getString(R.string.mbx_tab_all), c?.all)
        setChipState(chipWarning, filterType == "warning", getString(R.string.mbx_tab_warning), c?.warning)
        setChipState(chipNotice, filterType == "notice", getString(R.string.mbx_tab_notice), c?.notice)
        setChipState(chipAnnounce, filterType == "announce", getString(R.string.mbx_tab_announce), c?.announce)
        setChipState(chipUnread, unreadOnly, getString(R.string.mbx_unread_only), null)
        // appbar 徽章＝全部未读数（服务端唯一来源）
        if (c != null && c.all > 0) {
            tvBadge.text = c.all.toString()
            tvBadge.visibility = View.VISIBLE
        } else {
            tvBadge.visibility = View.GONE
        }
    }

    private fun setChipState(chip: TextView, active: Boolean, label: String, n: Int?) {
        chip.text = if (n != null && n > 0) "$label $n" else label
        chip.setBackgroundResource(if (active) R.drawable.mbx_chip_on else R.drawable.mbx_chip)
        chip.setTextColor(
            getColor(if (active) android.R.color.white else R.color.text_secondary),
        )
        chip.paint.isFakeBoldText = active
    }

    // —— 动作（只动邮箱状态，永不触归档行） ——

    private fun runAction(action: () -> Unit, onDone: () -> Unit) {
        val c = client ?: return
        btnConnect.isEnabled = false
        executor.execute {
            var err: String? = null
            try {
                action()
            } catch (e: FilesClient.ApiException) {
                err = describe(e)
            } catch (e: java.io.IOException) {
                err = describe(e)
            }
            runOnUiThread {
                btnConnect.isEnabled = true
                if (err != null) {
                    tvState.text = err
                } else {
                    onDone()
                }
            }
        }
    }

    private fun markRead(id: Long) {
        val c = client ?: return
        runAction({ c.markMailboxRead(id) }) {
            Toast.makeText(this, R.string.mbx_mark_done, Toast.LENGTH_SHORT).show()
            refresh()
        }
    }

    private fun confirmAnnounce(id: Long) {
        val c = client ?: return
        runAction({ c.confirmMailbox(id) }) {
            Toast.makeText(this, R.string.mbx_confirm_done, Toast.LENGTH_SHORT).show()
            closeSheet()
            refresh()
        }
    }

    private fun handleWarning(id: Long) {
        val c = client ?: return
        runAction({ c.markMailboxHandled(id) }) {
            Toast.makeText(this, R.string.mbx_handle_done, Toast.LENGTH_SHORT).show()
            refresh()
        }
    }

    // —— 详情抽屉（布局内底部面板；原型屏②） ——

    private fun openSheet(item: FilesClient.MailboxItem) {
        sheetItem = item
        val kindLabel = kindLabel(item.kind)
        sheetIcon.text = kindIcon(item.kind)
        sheetIcon.setBackgroundResource(kindIconBg(item.kind))
        sheetIcon.setTextColor(getColor(kindIconFg(item.kind)))
        sheetTitle.text = item.title
        sheetMeta.text =
            getString(R.string.mbx_meta, kindLabel, item.source, timeFmt.format(Date(item.timeMs)))
        sheetKind.text = kindLabel
        sheetKind.setBackgroundResource(kindTagBg(item.kind))
        sheetBody.text = if (item.sub.isBlank()) "—" else item.sub

        // 告警已处理留痕（state=2 时展示处理人）
        if (item.state == 2 && item.handledBy.isNotBlank()) {
            sheetTrace.text = getString(
                R.string.mbx_handled_by,
                item.handledBy,
                timeFmt.format(Date(item.handledMs)),
            )
            sheetTrace.visibility = View.VISIBLE
        } else {
            sheetTrace.visibility = View.GONE
        }

        // 台账与主钮按类目：公告＝确认台账＋确认已读；其余＝确认已读（标记已读）
        sheetLedgerHead.visibility = View.GONE
        sheetLedger.removeAllViews()
        sheetLedger.visibility = View.GONE
        sheetConfirm.visibility = View.VISIBLE
        if (item.kind == "announce") {
            sheetConfirm.setText(R.string.mbx_act_confirm)
            loadLedger(item)
        } else {
            sheetConfirm.setText(R.string.mbx_act_confirm)
        }
        sheetWrap.visibility = View.VISIBLE
    }

    private fun onSheetConfirmClicked() {
        val item = sheetItem ?: return
        if (item.kind == "announce") {
            if (item.state == 0) confirmAnnounce(item.id) else closeSheet()
        } else {
            if (item.state == 0) markRead(item.id) else closeSheet()
        }
    }

    private fun closeSheet() {
        sheetWrap.visibility = View.GONE
        sheetItem = null
    }

    /** 公告已读确认台账（仅公告收件人可查，403 明示）；谁/何时同屏 */
    private fun loadLedger(item: FilesClient.MailboxItem) {
        val c = client ?: return
        executor.execute {
            var err: String? = null
            var rows: List<FilesClient.MailboxConfirmation> = emptyList()
            try {
                rows = c.mailboxConfirmations(item.id)
            } catch (e: FilesClient.ApiException) {
                err = describe(e)
            } catch (e: java.io.IOException) {
                err = describe(e)
            }
            runOnUiThread {
                if (sheetItem?.id != item.id) return@runOnUiThread
                if (err != null) {
                    sheetLedgerHead.text = err
                    sheetLedgerHead.visibility = View.VISIBLE
                    return@runOnUiThread
                }
                val confirmed = rows.count { it.confirmed }
                sheetLedgerHead.text = getString(
                    R.string.mbx_ledger_head,
                    item.confirmTotal,
                    confirmed,
                    (item.confirmTotal - confirmed).coerceAtLeast(0),
                )
                sheetLedgerHead.visibility = View.VISIBLE
                sheetLedger.removeAllViews()
                val dp8 = (resources.displayMetrics.density * 8).toInt()
                for (row in rows) addLedgerRow(row, dp8)
                sheetLedger.visibility = View.VISIBLE
            }
        }
    }

    private fun addLedgerRow(row: FilesClient.MailboxConfirmation, dp8: Int) {
        val holder = LinearLayout(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            )
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
            setPadding(0, dp8 / 2, 0, dp8 / 2)
        }
        val avatar = TextView(this).apply {
            layoutParams = LinearLayout.LayoutParams(0, 0).apply {
                width = dp8 * 3
                height = dp8 * 3
                marginEnd = dp8
            }
            gravity = android.view.Gravity.CENTER
            text = row.account.take(1).ifBlank { "?" }
            textSize = 12f
            setTextColor(getColor(R.color.brand_deep_text))
            setBackgroundResource(R.drawable.mbx_badge)
        }
        // 头像底色改为柔色（mbx_badge 是品牌实底，台账头像用 brand_soft）
        avatar.background.setTint(getColor(R.color.brand_soft))
        val name = TextView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                0,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                1f,
            )
            text = row.account
            textSize = 12.5f
            setTextColor(getColor(R.color.text_primary))
        }
        val flag = TextView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            )
            text = getString(
                if (row.confirmed) R.string.mbx_ledger_confirmed else R.string.mbx_ledger_pending,
            )
            textSize = 11f
            setTextColor(getColor(if (row.confirmed) R.color.ok else R.color.brand_orange))
        }
        holder.addView(avatar)
        holder.addView(name)
        holder.addView(flag)
        sheetLedger.addView(holder)
    }

    // —— 类目样式 ——

    private fun kindLabel(kind: String): String = when (kind) {
        "warning" -> getString(R.string.mbx_kind_warning)
        "announce" -> getString(R.string.mbx_kind_announce)
        else -> getString(R.string.mbx_kind_notice)
    }

    /** 类目图标格（原型 SVG；本仓用字形近似） */
    private fun kindIcon(kind: String): String = when (kind) {
        "warning" -> "!"
        "announce" -> "公"
        else -> "i"
    }

    private fun kindIconBg(kind: String): Int = when (kind) {
        "warning" -> R.drawable.mbx_icon_warn
        "announce" -> R.drawable.mbx_icon_announce
        else -> R.drawable.mbx_icon_notice
    }

    private fun kindIconFg(kind: String): Int = when (kind) {
        "warning" -> R.color.warn
        "announce" -> R.color.brand_deep_text
        else -> R.color.text_secondary
    }

    private fun kindTagBg(kind: String): Int = when (kind) {
        "warning" -> R.drawable.mbx_tag_warn
        "announce" -> R.drawable.mbx_tag_announce
        else -> R.drawable.mbx_tag_notice
    }

    // —— 列表条目 ——

    /** 条目卡：未读＝brand-soft 底＋品牌描边＋左缘圆点；动作行按类目给真钮 */
    private inner class RowsAdapter : BaseAdapter() {
        private var rows: List<FilesClient.MailboxItem> = emptyList()

        fun update(list: List<FilesClient.MailboxItem>) {
            rows = list
            notifyDataSetChanged()
        }

        override fun getCount(): Int = rows.size

        override fun getItem(position: Int): FilesClient.MailboxItem = rows[position]

        override fun getItemId(position: Int): Long = rows[position].id

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val v = convertView ?: layoutInflater.inflate(R.layout.item_mailbox, parent, false)
            val m = rows[position]
            val card = v.findViewById<View>(R.id.mbx_row_card)
            val icon = v.findViewById<TextView>(R.id.mbx_row_icon)
            val title = v.findViewById<TextView>(R.id.mbx_row_title)
            val time = v.findViewById<TextView>(R.id.mbx_row_time)
            val kind = v.findViewById<TextView>(R.id.mbx_row_kind)
            val state = v.findViewById<TextView>(R.id.mbx_row_state)
            val sub = v.findViewById<TextView>(R.id.mbx_row_sub)
            val acts = v.findViewById<LinearLayout>(R.id.mbx_row_acts)
            val flat = v.findViewById<Button>(R.id.mbx_row_act_flat)
            val primary = v.findViewById<Button>(R.id.mbx_row_act_primary)

            // 未读卡：brand-soft 底＋品牌描边＋左缘圆点；已读回白底
            if (m.unread) {
                card.setBackgroundResource(R.drawable.mbx_card_unread)
            } else {
                card.setBackgroundResource(R.drawable.mbx_card)
            }
            icon.text = kindIcon(m.kind)
            icon.setBackgroundResource(kindIconBg(m.kind))
            icon.setTextColor(getColor(kindIconFg(m.kind)))
            title.text = m.title
            time.text = timeFmt.format(Date(m.timeMs))
            kind.text = kindLabel(m.kind)
            kind.setBackgroundResource(kindTagBg(m.kind))
            sub.text = m.sub

            // 状态胶囊：公告未读且需全员确认＝待确认；state 1/2＝已确认/已处理
            when {
                m.kind == "announce" && m.state == 0 && m.confirmTotal > 0 -> {
                    state.text = getString(R.string.mbx_state_pending)
                    state.setBackgroundResource(R.drawable.mbx_tag_warn)
                    state.setTextColor(getColor(R.color.brand_orange))
                    state.visibility = View.VISIBLE
                }
                m.state == 1 -> {
                    state.text = getString(R.string.mbx_state_confirmed)
                    state.setBackgroundResource(R.drawable.mbx_tag_status)
                    state.setTextColor(getColor(R.color.ok))
                    state.visibility = View.VISIBLE
                }
                m.state == 2 -> {
                    state.text = getString(R.string.mbx_state_handled)
                    state.setBackgroundResource(R.drawable.mbx_tag_status)
                    state.setTextColor(getColor(R.color.ok))
                    state.visibility = View.VISIBLE
                }
                else -> state.visibility = View.GONE
            }

            // 动作行：告警＝去处理＋确认已读；公告＝详情＋确认已读；通知＝无
            val unreadAction = m.state == 0
            when (m.kind) {
                "warning" -> {
                    acts.visibility = if (m.state == 2) View.GONE else View.VISIBLE
                    flat.visibility = View.VISIBLE
                    flat.text = getString(R.string.mbx_act_handle)
                    primary.visibility = if (unreadAction) View.VISIBLE else View.GONE
                    primary.text = getString(R.string.mbx_act_confirm)
                    flat.setOnClickListener { handleWarning(m.id) }
                    primary.setOnClickListener { markRead(m.id) }
                }
                "announce" -> {
                    acts.visibility = View.VISIBLE
                    flat.visibility = View.VISIBLE
                    flat.text = getString(R.string.mbx_act_detail)
                    primary.visibility = if (unreadAction) View.VISIBLE else View.GONE
                    primary.text = getString(R.string.mbx_act_confirm)
                    flat.setOnClickListener { openSheet(m) }
                    primary.setOnClickListener { confirmAnnounce(m.id) }
                }
                else -> {
                    acts.visibility = View.GONE
                    flat.visibility = View.GONE
                    primary.visibility = View.GONE
                }
            }
            return v
        }
    }
}
