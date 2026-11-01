package com.memex.im.ui

import android.content.Intent
import android.graphics.Color
import android.graphics.Paint
import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.BaseAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.ListView
import android.widget.ProgressBar
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
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
 * 任务清单（R27-6，原型 mobile-tasks）：手机侧对齐桌面 TaskDialog 功能面——
 *  - 类型从服务端拉取（/files/task-types，admin 后台可增删改，缺省默认类型）；
 *  - 截止（deadline）＋开始时间（勾选才生效，0=未设）随创建提交；
 *  - 状态三态（待办/进行中/完成）：卡上「完成/开始/暂停」走
 *    /files/tasks/status（服务端判权并双写 done，读取以 status 为准）；
 *  - 任务卡＝勾选框＋标题＋meta 胶囊（逾期/7 天内到期/截止/已完成/自建/
 *    由 X 分配/只读镜像）＋动作行；外部行（provider/ext_key）只读镜像，
 *    协议无详情 URL——「跳外部」如实不摆；
 *  - 看板＝同一批任务数据的另一视图（不新建数据模型/通道），横滑三列
 *    待办/进行中/完成，列头计数；窄屏（<600dp）看板入口降级隐藏；
 *  - 提醒开关（开始到点＋截止到期，默认开）与本页轮询提醒同源桌面
 *    通知中心口径：到期待办→系统通知＋回执只提醒一次。
 * 数据面全部走 core [FilesClient]（阻塞式），本页只做线程搬运与展示。
 */
class TasksActivity : AppCompatActivity() {

    private val executor: ExecutorService = Executors.newSingleThreadExecutor()
    private var client: FilesClient? = null

    private lateinit var tvTitle: TextView
    private lateinit var etAccount: EditText
    private lateinit var etPassword: EditText
    private lateinit var etPort: EditText
    private lateinit var btnConnect: Button
    private lateinit var tvError: TextView
    private lateinit var tvState: TextView
    private lateinit var tvEmpty: TextView
    private lateinit var pbBusy: ProgressBar
    private lateinit var entryBlock: View
    private lateinit var etTitle: EditText
    private lateinit var spType: Spinner
    private lateinit var etAssignee: EditText
    private lateinit var etDue: EditText
    private lateinit var cbStartOn: CheckBox
    private lateinit var etStart: EditText
    private lateinit var cbRemindStart: CheckBox
    private lateinit var cbRemindDue: CheckBox
    private lateinit var btnAdd: Button
    private lateinit var btnBoard: Button
    private lateinit var degNote: View
    private lateinit var lvTasks: ListView
    private lateinit var boardScroll: View
    private lateinit var lvBoardTodo: ListView
    private lateinit var lvBoardDoing: ListView
    private lateinit var lvBoardDone: ListView
    private lateinit var nTodo: TextView
    private lateinit var nDoing: TextView
    private lateinit var nDone: TextView

    private val adapter = TasksAdapter()
    private val todoAdapter = CardsAdapter()
    private val doingAdapter = CardsAdapter()
    private val doneAdapter = CardsAdapter()

    /** 类型清单（登录后拉取；缺省选中默认类型） */
    private var types: List<FilesClient.TaskType> = emptyList()

    /** 最近一次拉取的看板数据源（同批数据，切看板即重渲染） */
    private var board: FilesClient.TaskBoard = FilesClient.TaskBoard(emptyList(), emptyList())

    private var boardMode = false

    /** 窄屏降级：看板入口隐藏（<600dp），列表视图兜底 */
    private val narrowScreen: Boolean by lazy {
        resources.configuration.smallestScreenWidthDp < 600
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_tasks)

        tvTitle = findViewById(R.id.tsk_title)
        etAccount = findViewById(R.id.et_tasks_account)
        etPassword = findViewById(R.id.et_tasks_password)
        etPort = findViewById(R.id.et_tasks_port)
        btnConnect = findViewById(R.id.btn_tasks_connect)
        tvError = findViewById(R.id.tv_tasks_error)
        tvState = findViewById(R.id.tv_tasks_state)
        tvEmpty = findViewById(R.id.tv_tasks_empty)
        pbBusy = findViewById(R.id.pb_tasks)
        entryBlock = findViewById(R.id.tsk_entry)
        etTitle = findViewById(R.id.et_task_title)
        spType = findViewById(R.id.sp_task_type)
        etAssignee = findViewById(R.id.et_task_assignee)
        etDue = findViewById(R.id.et_task_due)
        cbStartOn = findViewById(R.id.cb_task_start_on)
        etStart = findViewById(R.id.et_task_start)
        cbRemindStart = findViewById(R.id.cb_task_remind_start)
        cbRemindDue = findViewById(R.id.cb_task_remind_due)
        btnAdd = findViewById(R.id.btn_task_add)
        btnBoard = findViewById(R.id.btn_task_board)
        degNote = findViewById(R.id.tsk_deg)
        lvTasks = findViewById(R.id.lv_tasks)
        boardScroll = findViewById(R.id.hs_tasks_board)
        lvBoardTodo = findViewById(R.id.lv_board_todo)
        lvBoardDoing = findViewById(R.id.lv_board_doing)
        lvBoardDone = findViewById(R.id.lv_board_done)
        nTodo = findViewById(R.id.tsk_n_todo)
        nDoing = findViewById(R.id.tsk_n_doing)
        nDone = findViewById(R.id.tsk_n_done)

        Session.instance.account?.let { etAccount.setText(it) }
        etPort.setText(FilesClient.DEFAULT_FILES_PORT.toString())
        if (narrowScreen) btnBoard.visibility = View.GONE // 窄屏降级：只留列表

        // 提醒开关持久化（默认开，与桌面 QSettings 口径同源）
        val prefs = getSharedPreferences("tasks_ui", MODE_PRIVATE)
        cbRemindStart.isChecked = prefs.getBoolean(PREF_REMIND_START, true)
        cbRemindDue.isChecked = prefs.getBoolean(PREF_REMIND_DUE, true)
        cbRemindStart.setOnCheckedChangeListener { _, v ->
            prefs.edit().putBoolean(PREF_REMIND_START, v).apply()
        }
        cbRemindDue.setOnCheckedChangeListener { _, v ->
            prefs.edit().putBoolean(PREF_REMIND_DUE, v).apply()
        }

        spType.adapter = ArrayAdapter(
            this, android.R.layout.simple_spinner_dropdown_item,
            listOf(getString(R.string.tasks_type_default)),
        )

        btnConnect.setOnClickListener { onConnectClicked() }
        btnAdd.setOnClickListener { onAddClicked() }
        btnBoard.setOnClickListener { toggleBoard() }
        findViewById<Button>(R.id.btn_tasks_refresh).setOnClickListener { refresh() }
        cbStartOn.setOnCheckedChangeListener { _, checked ->
            etStart.isEnabled = checked
        }

        lvTasks.adapter = adapter
        lvTasks.onItemClickListener =
            AdapterView.OnItemClickListener { _, _, position, _ ->
                adapter.getItem(position)?.let { showTaskActions(it) }
            }
        // 看板卡片点按＝推进状态（待办→进行中→完成→待办循环）
        lvBoardTodo.onItemClickListener = clickCard { id ->
            act { it.setTaskStatus(id, STATUS_DOING) }
        }
        lvBoardDoing.onItemClickListener = clickCard { id ->
            act { it.setTaskStatus(id, STATUS_DONE) }
        }
        lvBoardDone.onItemClickListener = clickCard { id ->
            act { it.setTaskStatus(id, STATUS_TODO) }
        }

        // 四页导航（任务 active；会话=回主界面栈）
        findViewById<View>(R.id.tsk_tab_sessions).setOnClickListener { finish() }
        findViewById<View>(R.id.tsk_tab_mailbox).setOnClickListener {
            startActivity(Intent(this, MailboxActivity::class.java))
            finish()
        }
        findViewById<View>(R.id.tsk_tab_me).setOnClickListener {
            startActivity(Intent(this, MeActivity::class.java))
            finish()
        }
    }

    override fun onDestroy() {
        executor.shutdownNow()
        super.onDestroy()
    }

    private fun clickCard(onCard: (Long) -> Unit): AdapterView.OnItemClickListener =
        AdapterView.OnItemClickListener { parent, _, position, _ ->
            (parent.adapter as? CardsAdapter)?.getItem(position)?.let { onCard(it.id) }
        }

    /** 统一线程搬运：阻塞调用进 executor，异常统一 describe 上屏 */
    private fun act(block: (FilesClient) -> Unit) {
        val c = client ?: return
        setBusy(true)
        executor.execute {
            var err: String? = null
            try {
                block(c)
            } catch (e: FilesClient.ApiException) {
                err = describe(e)
            } catch (e: java.io.IOException) {
                err = getString(R.string.err_unreachable)
            }
            runOnUiThread {
                setBusy(false)
                if (err != null) showError(err) else refresh()
            }
        }
    }

    // —— 连接 ——

    private fun onConnectClicked() {
        if (client?.isLoggedIn == true) {
            client?.logout()
            client = null
            adapter.update(emptyList())
            setConnected(false)
            return
        }
        val account = etAccount.text.toString().trim()
        val password = etPassword.text.toString()
        val port = etPort.text.toString().toIntOrNull()
        if (account.isEmpty() || password.isEmpty()) {
            showError(getString(R.string.files_err_need_account))
            return
        }
        if (port == null || port !in 1..65535) {
            showError(getString(R.string.err_addr_bad_port))
            return
        }
        val host = PrefsInitStore(this).serverAddress()?.host ?: run {
            startActivity(Intent(this, InitActivity::class.java))
            finish()
            return
        }
        showError("")
        setBusy(true)
        executor.execute {
            var err: String? = null
            var c: FilesClient? = null
            try {
                c = FilesClient().also { it.login(host, port, account, password) }
                types = c.taskTypes()
            } catch (e: FilesClient.ApiException) {
                err = describe(e)
            } catch (e: java.io.IOException) {
                err = getString(R.string.err_unreachable)
            }
            runOnUiThread {
                setBusy(false)
                if (err != null) {
                    showError(err)
                    return@runOnUiThread
                }
                client = c
                setConnected(true)
                renderTypes()
                refresh()
            }
        }
    }

    /** 类型下拉：服务端清单（默认类型置首选中） */
    private fun renderTypes() {
        val names = types.map { it.name }
            .ifEmpty { listOf(getString(R.string.tasks_type_default)) }
        val defIdx = types.indexOfFirst { it.isDefault }.coerceAtLeast(0)
        spType.adapter = ArrayAdapter(
            this, android.R.layout.simple_spinner_dropdown_item, names,
        )
        if (defIdx in names.indices) spType.setSelection(defIdx)
    }

    // —— 建任务 ——

    private fun onAddClicked() {
        val title = etTitle.text.toString().trim()
        if (title.isEmpty()) {
            showError(getString(R.string.tasks_err_need_title))
            return
        }
        val dueMs = etDue.text.toString().toLongOrNull() ?: 0L
        val startMs = if (cbStartOn.isChecked) etStart.text.toString().toLongOrNull() ?: 0L else 0L
        val type = types.getOrNull(spType.selectedItemPosition)?.name ?: ""
        val assignee = etAssignee.text.toString().trim()
        act { c ->
            c.createTask(title, "", dueMs, assignee, type, startMs)
        }
        etTitle.setText("")
    }

    /** 到点提醒（轮询口径＝每次刷新后检查）：开关关的类目直接跳过；
     *  系统通知＋回执落服务端（start/截止两类回执独立），只提醒一次 */
    private fun checkDue() {
        val c = client ?: return
        val wantStart = cbRemindStart.isChecked
        val wantDue = cbRemindDue.isChecked
        if (!wantStart && !wantDue) return
        val now = System.currentTimeMillis()
        val fired = ArrayList<Pair<FilesClient.Task, String>>()
        for (t in board.mine) {
            if (t.done || t.status == STATUS_DONE) continue
            if (wantDue && t.dueMs in 1..now && t.remindedMs == 0L) {
                fired.add(t to "")
            }
            if (wantStart && t.startMs in 1..now && t.startRemindedMs == 0L) {
                fired.add(t to "start")
            }
        }
        if (fired.isEmpty()) return
        executor.execute {
            for ((t, kind) in fired) {
                try {
                    c.markTaskReminded(t.id, kind)
                } catch (_: java.io.IOException) {
                } // 回执失败不拦通知；下轮重试
            }
        }
        val sys = getSystemService(NOTIFICATION_SERVICE) as android.app.NotificationManager
        for ((t, kind) in fired) {
            val text =
                if (kind == "start") getString(R.string.tasks_notify_start, t.title)
                else getString(R.string.tasks_notify_due, t.title)
            val n = androidx.core.app.NotificationCompat.Builder(
                this, NotificationHelper.CH_IMPORTANT
            )
                .setSmallIcon(R.mipmap.ic_launcher)
                .setContentTitle(getString(R.string.tasks_notify_title))
                .setContentText(text)
                .setAutoCancel(true)
                .build()
            // 通知 id 带 kind：同一任务的 start/截止两类回执互不覆盖
            sys.notify(("task-${kind}-${t.id}").hashCode(), n)
        }
    }

    // —— 列表 / 看板 ——

    private fun refresh() {
        val c = client ?: return
        setBusy(true)
        executor.execute {
            var err: String? = null
            var b = FilesClient.TaskBoard(emptyList(), emptyList())
            try {
                b = c.listTasks()
            } catch (e: FilesClient.ApiException) {
                err = describe(e)
            } catch (e: java.io.IOException) {
                err = getString(R.string.err_unreachable)
            }
            runOnUiThread {
                setBusy(false)
                if (err != null) {
                    showError(err)
                    return@runOnUiThread
                }
                board = b
                adapter.update(b.mine)
                val showEmpty = b.mine.isEmpty() && !boardMode
                tvEmpty.visibility = if (showEmpty) View.VISIBLE else View.GONE
                if (!boardMode) lvTasks.visibility =
                    if (showEmpty) View.GONE else View.VISIBLE
                renderBoard()
                tvState.text = getString(R.string.tasks_state_count, b.mine.size)
                checkDue()
            }
        }
    }

    private fun toggleBoard() {
        boardMode = !boardMode
        boardScroll.visibility = if (boardMode) View.VISIBLE else View.GONE
        if (!boardMode && board.mine.isEmpty()) {
            lvTasks.visibility = View.GONE
            tvEmpty.visibility = View.VISIBLE
        } else {
            lvTasks.visibility = if (boardMode) View.GONE else View.VISIBLE
            tvEmpty.visibility = View.GONE
        }
        tvTitle.setText(if (boardMode) R.string.tsk_board_title else R.string.tasks_title)
        // 看板视图收起录入区（回列表恢复，输入不丢）＋亮降级注
        entryBlock.visibility = if (boardMode) View.GONE else View.VISIBLE
        degNote.visibility = if (boardMode) View.VISIBLE else View.GONE
        // 钮随视图换主次：看板态「列表」为主钮，列表态「看板」为次钮
        btnBoard.setText(if (boardMode) R.string.tasks_list else R.string.tasks_board)
        btnBoard.setBackgroundResource(
            if (boardMode) R.drawable.mbx_btn_primary else R.drawable.mbx_btn_flat
        )
        btnBoard.setTextColor(
            if (boardMode) Color.WHITE else getColor(R.color.text_secondary)
        )
        btnBoard.paint.isFakeBoldText = boardMode
        renderBoard()
    }

    /** 看板重渲染：三列按 status 语义分桶（同批数据，不另拉）＋列头计数；
     *  卡片逾期切 warn 描边、临近（7 天内）soon 胶囊、完成半透明删除线 */
    private fun renderBoard() {
        val now = System.currentTimeMillis()
        val todo = ArrayList<FilesClient.Task>()
        val doing = ArrayList<FilesClient.Task>()
        val done = ArrayList<FilesClient.Task>()
        for (t in board.mine) {
            when {
                t.done || t.status == STATUS_DONE -> done.add(t)
                t.status == STATUS_DOING -> doing.add(t)
                else -> todo.add(t)
            }
        }
        nTodo.text = todo.size.toString()
        nDoing.text = doing.size.toString()
        nDone.text = done.size.toString()
        todoAdapter.update(todo, now)
        doingAdapter.update(doing, now)
        doneAdapter.update(done, now)
    }

    // —— 行动作 ——

    /** 行点按＝动作单（补删除入口；主操作在卡上按钮） */
    private fun showTaskActions(task: FilesClient.Task) {
        if (task.provider.isNotEmpty()) {
            // 外部只读镜像：无本地动作（协议无详情 URL，「跳外部」不摆）
            Toast.makeText(this, R.string.tsk_tag_read, Toast.LENGTH_SHORT).show()
            return
        }
        val options = ArrayList<String>()
        options.add(getString(R.string.tasks_action_start_pause))
        options.add(getString(R.string.tasks_action_done_toggle))
        options.add(getString(R.string.files_action_delete))
        AlertDialog.Builder(this)
            .setTitle(task.title)
            .setItems(options.toTypedArray()) { _, which ->
                when (which) {
                    0 -> act {
                        it.setTaskStatus(
                            task.id,
                            if (task.status == STATUS_DOING) STATUS_TODO else STATUS_DOING,
                        )
                    }
                    1 -> act {
                        it.setTaskStatus(
                            task.id,
                            if (task.done || task.status == STATUS_DONE) STATUS_TODO else STATUS_DONE,
                        )
                    }
                    else -> act { it.deleteTask(task.id) }
                }
            }
            .setNegativeButton(R.string.files_cancel, null)
            .show()
    }

    // —— 展示 ——

    private fun describe(e: java.io.IOException): String =
        when {
            e !is FilesClient.ApiException -> getString(R.string.err_unreachable)
            e.status == 401 -> getString(R.string.files_err_session)
            e.status == 0 -> getString(R.string.err_unreachable)
            else -> getString(R.string.files_err_prefix, e.status, e.error)
        }

    private fun showError(text: String) {
        if (text.isEmpty()) {
            tvError.visibility = View.GONE
        } else {
            tvError.text = text
            tvError.visibility = View.VISIBLE
        }
    }

    private fun setBusy(busy: Boolean) {
        pbBusy.visibility = if (busy) View.VISIBLE else View.GONE
        btnConnect.isEnabled = !busy
        btnAdd.isEnabled = !busy && client?.isLoggedIn == true
    }

    private fun setConnected(connected: Boolean) {
        btnConnect.setText(if (connected) R.string.files_disconnect else R.string.files_connect)
        tvState.setText(if (connected) R.string.tasks_state_on else R.string.files_state_off)
        tvState.setTextColor(
            if (connected) getColor(R.color.ok) else getColor(R.color.text_tertiary)
        )
        etAccount.isEnabled = !connected
        etPassword.isEnabled = !connected
        etPort.isEnabled = !connected
        btnAdd.isEnabled = connected
        if (!connected) tvEmpty.visibility = View.GONE
    }

    /** 胶囊日期（原型 meta 只显 MM-dd） */
    private fun fmtDate(tsMs: Long): String = try {
        SimpleDateFormat("MM-dd", Locale.getDefault()).format(Date(tsMs))
    } catch (_: Exception) {
        ""
    }

    private fun styleTag(tv: TextView, bgRes: Int, colorRes: Int) {
        tv.setBackgroundResource(bgRes)
        tv.setTextColor(getColor(colorRes))
        tv.visibility = View.VISIBLE
    }

    /** 第一胶囊（状态）：逾期（warn）/7 天内到期（brand）/截止（中性）/
     *  已完成（ok）/只读镜像（虚线） */
    private fun bindStateTag(tv: TextView, t: FilesClient.Task, now: Long) {
        when {
            t.provider.isNotEmpty() -> {
                styleTag(tv, R.drawable.tsk_tag_read, R.color.text_tertiary)
                tv.text = getString(R.string.tsk_tag_read)
            }
            t.done || t.status == STATUS_DONE -> {
                styleTag(tv, R.drawable.mbx_tag_status, R.color.ok)
                tv.text = if (t.doneMs > 0) {
                    getString(R.string.tsk_tag_done_at, fmtDate(t.doneMs))
                } else {
                    getString(R.string.tsk_tag_done)
                }
            }
            t.dueMs in 1..now -> {
                styleTag(tv, R.drawable.mbx_tag_warn, R.color.warn)
                tv.text = if (t.dueMs > 0) {
                    getString(R.string.tsk_tag_late_due, fmtDate(t.dueMs))
                } else {
                    getString(R.string.tsk_tag_late)
                }
            }
            t.dueMs > 0 -> {
                styleTag(tv, R.drawable.me_state_badge, R.color.brand_deep_text)
                tv.text = if (t.dueMs - now <= SOON_MS) {
                    getString(R.string.tsk_tag_soon, fmtDate(t.dueMs))
                } else {
                    getString(R.string.tsk_tag_due, fmtDate(t.dueMs))
                }
            }
            else -> tv.visibility = View.GONE
        }
    }

    /** 第二胶囊（来源）：自建 / 由 X 分配（外部行无来源胶囊） */
    private fun bindWhoTag(tv: TextView, t: FilesClient.Task) {
        if (t.provider.isNotEmpty()) {
            tv.visibility = View.GONE
            return
        }
        if (t.creator.isEmpty()) {
            styleTag(tv, R.drawable.mbx_tag_notice, R.color.text_secondary)
            tv.text = getString(R.string.tasks_self)
        } else {
            styleTag(tv, R.drawable.mbx_tag_notice, R.color.text_secondary)
            tv.text = getString(R.string.tasks_from, t.creator)
        }
    }

    /** 列表卡（原型 .titem）：勾选框＋标题＋meta 胶囊＋动作行 */
    private inner class TasksAdapter : BaseAdapter() {
        private var items: List<FilesClient.Task> = emptyList()

        fun update(list: List<FilesClient.Task>) {
            items = list
            notifyDataSetChanged()
        }

        override fun getCount() = items.size
        override fun getItem(position: Int): FilesClient.Task? =
            if (position in items.indices) items[position] else null

        override fun getItemId(position: Int) = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val view = convertView ?: layoutInflater.inflate(
                R.layout.item_task, parent, false
            )
            val t = items[position]
            val done = t.done || t.status == STATUS_DONE
            val external = t.provider.isNotEmpty()
            val now = System.currentTimeMillis()

            val box = view.findViewById<CheckBox>(R.id.tsk_box)
            box.setOnCheckedChangeListener(null)
            box.isChecked = done
            box.text = if (done) "✓" else ""
            box.isEnabled = !external
            if (!external) box.setOnCheckedChangeListener { _, checked ->
                act { it.setTaskStatus(t.id, if (checked) STATUS_DONE else STATUS_TODO) }
            }

            val title = view.findViewById<TextView>(R.id.tsk_title)
            title.text = t.title
            if (done) {
                title.paintFlags = title.paintFlags or Paint.STRIKE_THRU_TEXT_FLAG
                title.setTextColor(getColor(R.color.text_tertiary))
            } else {
                title.paintFlags = title.paintFlags and Paint.STRIKE_THRU_TEXT_FLAG.inv()
                title.setTextColor(getColor(R.color.text_primary))
            }

            bindStateTag(view.findViewById(R.id.tsk_tag1), t, now)
            bindWhoTag(view.findViewById(R.id.tsk_tag2), t)
            view.findViewById<TextView>(R.id.tsk_doing).visibility =
                if (t.status == STATUS_DOING && !done) View.VISIBLE else View.GONE

            // 动作行：外部只读与已完成卡无按钮
            val btns = view.findViewById<View>(R.id.tsk_btns)
            if (external || done) {
                btns.visibility = View.GONE
            } else {
                btns.visibility = View.VISIBLE
                view.findViewById<Button>(R.id.tsk_btn_done).setOnClickListener {
                    act { it.setTaskStatus(t.id, STATUS_DONE) }
                }
                view.findViewById<Button>(R.id.tsk_btn_toggle).setOnClickListener {
                    act {
                        it.setTaskStatus(
                            t.id,
                            if (t.status == STATUS_DOING) STATUS_TODO else STATUS_DOING,
                        )
                    }
                }
            }
            return view
        }
    }

    /** 看板卡（原型 .kb）：panel-2 底卡；逾期 warn 描边、完成 0.62
     *  透明度＋删除线；meta 一胶囊（逾期/日期/来源） */
    private inner class CardsAdapter : BaseAdapter() {
        private var items: List<FilesClient.Task> = emptyList()
        private var now = 0L

        fun update(list: List<FilesClient.Task>, nowMs: Long) {
            items = list
            now = nowMs
            notifyDataSetChanged()
        }

        override fun getCount() = items.size
        override fun getItem(position: Int): FilesClient.Task? =
            if (position in items.indices) items[position] else null

        override fun getItemId(position: Int) = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val view = convertView ?: layoutInflater.inflate(
                R.layout.item_task_card, parent, false
            )
            val t = items[position]
            val done = t.done || t.status == STATUS_DONE
            val external = t.provider.isNotEmpty()
            val late = !done && t.dueMs in 1..now

            view.setBackgroundResource(
                if (late) R.drawable.tsk_kb_late_bg else R.drawable.tsk_kb_bg
            )
            view.alpha = if (done) 0.62f else 1f

            val k = view.findViewById<TextView>(R.id.tsk_k)
            k.text = t.title
            if (done) {
                k.paintFlags = k.paintFlags or Paint.STRIKE_THRU_TEXT_FLAG
            } else {
                k.paintFlags = k.paintFlags and Paint.STRIKE_THRU_TEXT_FLAG.inv()
            }

            val m = view.findViewById<TextView>(R.id.tsk_m)
            when {
                done -> {
                    styleTag(m, R.drawable.mbx_tag_status, R.color.ok)
                    m.text = if (t.doneMs > 0) fmtDate(t.doneMs) else getString(R.string.tsk_tag_done)
                }
                late -> {
                    styleTag(m, R.drawable.mbx_tag_warn, R.color.warn)
                    m.text = getString(R.string.tsk_tag_late)
                }
                t.dueMs > 0 -> {
                    styleTag(m, R.drawable.me_state_badge, R.color.brand_deep_text)
                    m.text = fmtDate(t.dueMs)
                }
                external -> {
                    styleTag(m, R.drawable.tsk_tag_read, R.color.text_tertiary)
                    m.text = getString(R.string.tsk_tag_read)
                }
                t.creator.isEmpty() -> {
                    styleTag(m, R.drawable.mbx_tag_notice, R.color.text_secondary)
                    m.text = getString(R.string.tasks_self)
                }
                else -> {
                    styleTag(m, R.drawable.mbx_tag_notice, R.color.text_secondary)
                    m.text = getString(R.string.tasks_from, t.creator)
                }
            }
            return view
        }
    }

    companion object {
        private const val STATUS_TODO = "待办"
        private const val STATUS_DOING = "进行中"
        private const val STATUS_DONE = "完成"
        private const val PREF_REMIND_START = "task_remind_start"
        private const val PREF_REMIND_DUE = "task_remind_due"

        /** 「7 天内到期」胶囊阈值（原型 meta 口径） */
        private const val SOON_MS = 7L * 24 * 60 * 60 * 1000
    }
}
