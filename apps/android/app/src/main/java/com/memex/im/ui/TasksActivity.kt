package com.memex.im.ui

import android.content.Intent
import android.graphics.Color
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
 * 任务清单（R27-6）：手机侧对齐桌面 TaskDialog 功能面——
 *  - 类型从服务端拉取（/files/task-types，admin 后台可增删改，缺省默认类型）；
 *  - 截止（deadline）＋开始时间（勾选才生效，0=未设）随创建提交；
 *  - 状态三态（待办/进行中/完成）：行操作「开始/暂停/完成」走
 *    /files/tasks/status（服务端判权并双写 done，读取以 status 为准）；
 *  - 看板＝同一批任务数据的另一视图（不新建数据模型/通道），横滑三列
 *    待办/进行中/完成，卡片带类型与开始/截止，逾期红/临近黄；窄屏
 *    （sw<600dp）看板自动降级回列表视图；
 *  - 提醒开关（开始到点＋截止到期，默认开）与本页轮询提醒同源桌面
 *    通知中心口径：到期待办→系统通知＋回执只提醒一次。
 * 数据面全部走 core [FilesClient]（阻塞式），本页只做线程搬运与展示。
 */
class TasksActivity : AppCompatActivity() {

    private val executor: ExecutorService = Executors.newSingleThreadExecutor()
    private var client: FilesClient? = null

    private lateinit var etAccount: EditText
    private lateinit var etPassword: EditText
    private lateinit var etPort: EditText
    private lateinit var btnConnect: Button
    private lateinit var tvError: TextView
    private lateinit var tvState: TextView
    private lateinit var tvEmpty: TextView
    private lateinit var pbBusy: ProgressBar
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
    private lateinit var lvTasks: ListView
    private lateinit var boardScroll: View
    private lateinit var lvBoardTodo: ListView
    private lateinit var lvBoardDoing: ListView
    private lateinit var lvBoardDone: ListView

    private val adapter = TasksAdapter()
    private val todoAdapter = CardsAdapter()
    private val doingAdapter = CardsAdapter()
    private val doneAdapter = CardsAdapter()

    /** 类型清单（登录后拉取；缺省选中默认类型） */
    private var types: List<FilesClient.TaskType> = emptyList()

    /** 最近一次拉取的看板数据源（同批数据，切看板即重渲染） */
    private var board: FilesClient.TaskBoard = FilesClient.TaskBoard(emptyList(), emptyList())

    private var boardMode = false

    /** 窄屏降级：看板入口隐藏（sw<600dp），列表视图兜底 */
    private val narrowScreen: Boolean by lazy {
        resources.configuration.smallestScreenWidthDp < 600
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_tasks)

        etAccount = findViewById(R.id.et_tasks_account)
        etPassword = findViewById(R.id.et_tasks_password)
        etPort = findViewById(R.id.et_tasks_port)
        btnConnect = findViewById(R.id.btn_tasks_connect)
        tvError = findViewById(R.id.tv_tasks_error)
        tvState = findViewById(R.id.tv_tasks_state)
        tvEmpty = findViewById(R.id.tv_tasks_empty)
        pbBusy = findViewById(R.id.pb_tasks)
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
        lvTasks = findViewById(R.id.lv_tasks)
        boardScroll = findViewById(R.id.hs_tasks_board)
        lvBoardTodo = findViewById(R.id.lv_board_todo)
        lvBoardDoing = findViewById(R.id.lv_board_doing)
        lvBoardDone = findViewById(R.id.lv_board_done)

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
                tvEmpty.visibility =
                    if (b.mine.isEmpty() && !boardMode) View.VISIBLE else View.GONE
                renderBoard()
                tvState.text = getString(R.string.tasks_state_count, b.mine.size)
                checkDue()
            }
        }
    }

    private fun toggleBoard() {
        boardMode = !boardMode
        lvTasks.visibility = if (boardMode) View.GONE else View.VISIBLE
        boardScroll.visibility = if (boardMode) View.VISIBLE else View.GONE
        btnBoard.setText(if (boardMode) R.string.tasks_list else R.string.tasks_board)
        tvEmpty.visibility = View.GONE
        renderBoard()
    }

    /** 看板重渲染：三列按 status 语义分桶（同批数据，不另拉）；逾期红/
     *  临近黄（阈值 3 天，与桌面缺省同口径），完成灰 */
    private fun renderBoard() {
        val now = System.currentTimeMillis()
        val warnMs = 3L * 24 * 60 * 60 * 1000
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
        todoAdapter.update(todo, now, warnMs)
        doingAdapter.update(doing, now, warnMs)
        doneAdapter.update(done, now, warnMs)
    }

    // —— 行动作 ——

    private fun showTaskActions(task: FilesClient.Task) {
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
                    1 -> act { it.setTaskStatus(task.id, STATUS_DONE) }
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
        btnAdd.isEnabled = !busy
    }

    private fun setConnected(connected: Boolean) {
        btnConnect.setText(if (connected) R.string.files_disconnect else R.string.files_connect)
        tvState.setText(if (connected) R.string.tasks_state_on else R.string.files_state_off)
        etAccount.isEnabled = !connected
        etPassword.isEnabled = !connected
        etPort.isEnabled = !connected
        btnAdd.isEnabled = connected
        if (!connected) tvEmpty.visibility = View.GONE
    }

    private fun fmtTime(tsMs: Long): String = try {
        SimpleDateFormat("MM-dd HH:mm", Locale.getDefault()).format(Date(tsMs))
    } catch (_: Exception) {
        ""
    }

    /** 列表行：[x]标题 ·类型  来源  状态  开始/截止 */
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
                android.R.layout.simple_list_item_2, parent, false
            )
            val t = items[position]
            val title = view.findViewById<TextView>(android.R.id.text1)
            val subtitle = view.findViewById<TextView>(android.R.id.text2)
            val mark = if (t.done || t.status == STATUS_DONE) "[x] " else "[ ] "
            title.text = "$mark${t.title} ·${t.type}"
            if (t.done || t.status == STATUS_DONE) {
                title.setTextColor(Color.GRAY)
            } else if (t.dueMs in 1..System.currentTimeMillis()) {
                title.setTextColor(Color.RED) // 逾期
            } else {
                title.setTextColor(tvState.textColors.defaultColor)
            }
            val parts = ArrayList<String>()
            if (t.creator.isEmpty()) parts.add(getString(R.string.tasks_self)) else parts.add(
                getString(R.string.tasks_from, t.creator)
            )
            if (t.status == STATUS_DOING) parts.add(getString(R.string.tasks_doing))
            if (t.startMs > 0) parts.add(getString(R.string.tasks_start_at, fmtTime(t.startMs)))
            if (t.dueMs > 0) parts.add(getString(R.string.tasks_due_at, fmtTime(t.dueMs)))
            subtitle.text = parts.joinToString("  ")
            return view
        }
    }

    /** 看板卡片（单行）：标题 ·类型＋截止；逾期红/临近黄/完成灰 */
    private inner class CardsAdapter : BaseAdapter() {
        private var items: List<FilesClient.Task> = emptyList()
        private var now = 0L
        private var warnMs = 0L

        fun update(list: List<FilesClient.Task>, nowMs: Long, warn: Long) {
            items = list
            now = nowMs
            warnMs = warn
            notifyDataSetChanged()
        }

        override fun getCount() = items.size
        override fun getItem(position: Int): FilesClient.Task? =
            if (position in items.indices) items[position] else null

        override fun getItemId(position: Int) = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val view = convertView ?: layoutInflater.inflate(
                android.R.layout.simple_list_item_1, parent, false
            )
            val t = items[position]
            val line = view.findViewById<TextView>(android.R.id.text1)
            val extra = if (t.dueMs > 0) "  ${getString(R.string.tasks_due_at, fmtTime(t.dueMs))}" else ""
            line.text = "${t.title} ·${t.type}$extra"
            when {
                t.done || t.status == STATUS_DONE -> line.setTextColor(Color.GRAY)
                t.dueMs in 1..now -> line.setTextColor(Color.RED)
                t.dueMs - now <= warnMs -> line.setTextColor(Color.rgb(0xB8, 0x86, 0x0B))
                else -> line.setTextColor(tvState.textColors.defaultColor)
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
    }
}
