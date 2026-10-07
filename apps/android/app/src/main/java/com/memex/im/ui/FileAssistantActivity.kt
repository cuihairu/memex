package com.memex.im.ui

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.BaseAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.ListView
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.memex.im.R
import com.memex.im.core.FilesClient
import com.memex.im.core.PrefsInitStore
import com.memex.im.core.Session
import com.memex.im.core.UplinkClient
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * 文件助手（R23-3 块3）：手机侧对齐桌面文件助手功能面——
 * 文件服务连接（Bearer token，文件面端口独立 24561）、备忘录建/改/删/查、
 * 收件箱混排列表（分页、时间倒序）、文件上传/下载/删除（inbox=「手机发自己」）、
 * target=me 个人空间与收件箱隔离、401/403/503 失败明示。
 * 数据面全部走 core [FilesClient]（阻塞式），本页只做线程搬运与展示；
 * 账号口令只在连接时使用不落盘（口令不持久化，与桌面面板同口径）。
 */
class FileAssistantActivity : AppCompatActivity() {

    private val executor: ExecutorService = Executors.newSingleThreadExecutor()
    private var client: FilesClient? = null

    /** 外网模式客户端（R23-4）：与内网面互斥，勾选框切换 */
    private var uplink: UplinkClient? = null

    /** 当前读面：inbox（收件箱）或 me（个人空间）；列表随 tab 切换重拉 */
    private var target: String = TARGET_INBOX

    private lateinit var etAccount: EditText
    private lateinit var etPassword: EditText
    private lateinit var etPort: EditText
    private lateinit var btnConnect: Button
    private lateinit var tvError: TextView
    private lateinit var tvState: TextView
    private lateinit var tvEmpty: TextView
    private lateinit var pbBusy: ProgressBar
    private lateinit var etMemo: EditText
    private lateinit var btnMemoSave: Button
    private lateinit var btnUpload: Button
    private lateinit var btnTabInbox: Button
    private lateinit var btnTabMe: Button
    private lateinit var cbUplink: CheckBox
    private lateinit var llMemoRow: View
    private val adapter = ItemsAdapter()

    /** 长按进入编辑的备忘录 id；null＝快记行在新建 */
    private var editingMemoId: Long? = null

    private val pickFile =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
            if (uri != null) uploadPicked(uri)
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_file_assistant)

        etAccount = findViewById(R.id.et_files_account)
        etPassword = findViewById(R.id.et_files_password)
        etPort = findViewById(R.id.et_files_port)
        btnConnect = findViewById(R.id.btn_files_connect)
        tvError = findViewById(R.id.tv_files_error)
        tvState = findViewById(R.id.tv_files_state)
        tvEmpty = findViewById(R.id.tv_files_empty)
        pbBusy = findViewById(R.id.pb_files)
        etMemo = findViewById(R.id.et_memo)
        btnMemoSave = findViewById(R.id.btn_memo_save)
        btnUpload = findViewById(R.id.btn_files_upload)
        btnTabInbox = findViewById(R.id.btn_tab_inbox)
        btnTabMe = findViewById(R.id.btn_tab_me)
        cbUplink = findViewById(R.id.cb_uplink_mode)
        llMemoRow = findViewById(R.id.ll_memo_row)

        // 账号预填协作登录态（口令不回填——不落盘）
        Session.instance.account?.let { etAccount.setText(it) }
        etPort.setText(FilesClient.DEFAULT_FILES_PORT.toString())

        btnConnect.setOnClickListener { onConnectClicked() }
        btnMemoSave.setOnClickListener { onMemoSaveClicked() }
        btnUpload.setOnClickListener { pickFile.launch(arrayOf("*/*")) }
        btnTabInbox.setOnClickListener { switchTarget(TARGET_INBOX) }
        btnTabMe.setOnClickListener { switchTarget(TARGET_ME) }
        findViewById<Button>(R.id.btn_files_refresh).setOnClickListener { refresh() }
        // 外网模式切换：换端口提示与预填（外网口无缺省——部署明示），收窄功能面
        cbUplink.setOnCheckedChangeListener { _, _ -> renderUplinkMode() }
        renderUplinkMode()

        val lv = findViewById<ListView>(R.id.lv_files)
        lv.adapter = adapter
        lv.onItemClickListener =
            AdapterView.OnItemClickListener { _, _, position, _ -> onItemClicked(position) }
        renderTabs()
    }

    override fun onDestroy() {
        executor.shutdownNow()
        super.onDestroy()
    }

    // —— 连接 ——

    private fun onConnectClicked() {
        if (client?.isLoggedIn == true || uplink?.isLoggedIn == true) {
            client?.logout()
            client = null
            uplink?.logout()
            uplink = null
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
        val isUplink = cbUplink.isChecked
        showError("")
        setBusy(true)
        executor.execute {
            var err: String? = null
            var internal: FilesClient? = null
            var external: UplinkClient? = null
            try {
                if (isUplink) {
                    external = UplinkClient().also { it.login(host, port, account, password) }
                } else {
                    internal = FilesClient().also { it.login(host, port, account, password) }
                }
            } catch (e: FilesClient.ApiException) {
                err = describe(e)
            } catch (e: UplinkClient.ApiException) {
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
                client = internal
                uplink = external
                setConnected(true)
                refresh()
            }
        }
    }

    // —— 备忘录 ——

    private fun onMemoSaveClicked() {
        val c = client ?: return
        val content = etMemo.text.toString()
        if (content.isBlank()) return
        val id = editingMemoId
        setBusy(true)
        executor.execute {
            var err: String? = null
            try {
                if (id == null) c.createMemo(content) else c.updateMemo(id, content)
            } catch (e: FilesClient.ApiException) {
                err = describe(e)
            } catch (e: java.io.IOException) {
                err = describe(e)
            }
            runOnUiThread {
                setBusy(false)
                if (err != null) {
                    showError(err)
                    return@runOnUiThread
                }
                etMemo.setText("")
                editingMemoId = null
                Toast.makeText(this, R.string.files_memo_saved, Toast.LENGTH_SHORT).show()
                refresh()
            }
        }
    }

    private fun editMemo(id: Long, content: String) {
        editingMemoId = id
        etMemo.setText(content)
        etMemo.requestFocus()
        Toast.makeText(this, R.string.files_memo_editing, Toast.LENGTH_SHORT).show()
    }

    private fun deleteMemo(id: Long) {
        val c = client ?: return
        setBusy(true)
        executor.execute {
            var err: String? = null
            try {
                c.deleteMemo(id)
            } catch (e: FilesClient.ApiException) {
                err = describe(e)
            } catch (e: java.io.IOException) {
                err = describe(e)
            }
            runOnUiThread {
                setBusy(false)
                if (err != null) showError(err) else refresh()
            }
        }
    }

    // —— 列表（收件箱/个人空间混排） ——

    private fun switchTarget(newTarget: String) {
        if (target == newTarget) return
        target = newTarget
        renderTabs()
        refresh()
    }

    private fun refresh() {
        val isUplink = uplink?.isLoggedIn == true
        val c = client
        val u = uplink
        if (!isUplink && c == null) return
        setBusy(true)
        executor.execute {
            var err: String? = null
            var items: List<FilesClient.InboxItem> = emptyList()
            try {
                if (isUplink && u != null) {
                    // 外网记录映射成 FileItem 展示（纯记录：pin/status 无意义置 0；
                    // 无任何下载动作——外网会话永远没有读取内网数据的权限）
                    items = u.mine().map {
                        FilesClient.InboxItem.FileItem(
                            id = it.id, fileName = it.fileName, fileSize = it.fileSize,
                            fileHash = it.fileHash, pin = 0, status = 0, uploadTs = it.uploadTs,
                        )
                    }
                } else if (c != null) {
                    items = if (target == TARGET_INBOX) c.listInbox() else c.listPersonal()
                }
            } catch (e: FilesClient.ApiException) {
                err = describe(e)
            } catch (e: UplinkClient.ApiException) {
                err = describe(e)
            } catch (e: java.io.IOException) {
                err = describe(e)
            }
            runOnUiThread {
                setBusy(false)
                if (err != null) {
                    showError(err)
                    return@runOnUiThread
                }
                adapter.update(items)
                tvEmpty.visibility = if (items.isEmpty()) View.VISIBLE else View.GONE
            }
        }
    }

    private fun onItemClicked(position: Int) {
        val item = adapter.getItem(position) ?: return
        when (item) {
            is FilesClient.InboxItem.Memo -> showMemoActions(item)
            is FilesClient.InboxItem.FileItem -> showFileActions(item)
        }
    }

    private fun showMemoActions(memo: FilesClient.InboxItem.Memo) {
        val options =
            arrayOf(getString(R.string.files_memo_edit), getString(R.string.files_action_delete))
        AlertDialog.Builder(this)
            .setTitle(R.string.files_title_memo)
            .setItems(options) { _, which ->
                if (which == 0) editMemo(memo.id, memo.content) else deleteMemo(memo.id)
            }
            .setNegativeButton(R.string.files_cancel, null)
            .show()
    }

    private fun showFileActions(file: FilesClient.InboxItem.FileItem) {
        // 外网模式只有「删除」（自己的上传）：无下载端点，单向铁律
        if (uplink?.isLoggedIn == true) {
            AlertDialog.Builder(this)
                .setTitle(file.fileName)
                .setItems(arrayOf(getString(R.string.files_action_delete))) { _, _ ->
                    deleteUplinkFile(file.id)
                }
                .setNegativeButton(R.string.files_cancel, null)
                .show()
            return
        }
        val options =
            arrayOf(getString(R.string.files_action_download), getString(R.string.files_action_delete))
        AlertDialog.Builder(this)
            .setTitle(file.fileName)
            .setItems(options) { _, which ->
                if (which == 0) downloadFile(file) else deleteFile(file.id)
            }
            .setNegativeButton(R.string.files_cancel, null)
            .show()
    }

    // —— 文件上传/下载/删除 ——

    private fun uploadPicked(uri: Uri) {
        val isUplink = uplink?.isLoggedIn == true
        val c = client
        val u = uplink
        if (!isUplink && c == null) return
        val name = queryDisplayName(uri) ?: return
        setBusy(true)
        executor.execute {
            var err: String? = null
            try {
                val bytes = contentResolver.openInputStream(uri)?.use { it.readBytes() }
                if (bytes == null) {
                    err = getString(R.string.files_err_read)
                } else if (isUplink && u != null) {
                    u.upload(name, bytes) // 无 target：外网面无落点选择权
                } else if (c != null) {
                    c.upload(target, name, bytes)
                }
            } catch (e: FilesClient.ApiException) {
                err = describe(e)
            } catch (e: UplinkClient.ApiException) {
                err = describe(e)
            } catch (e: java.io.IOException) {
                err = describe(e)
            }
            runOnUiThread {
                setBusy(false)
                if (err != null) {
                    showError(err)
                    return@runOnUiThread
                }
                Toast.makeText(this, R.string.files_uploaded, Toast.LENGTH_SHORT).show()
                refresh()
            }
        }
    }

    private fun queryDisplayName(uri: Uri): String? {
        val proj = arrayOf(android.provider.OpenableColumns.DISPLAY_NAME)
        contentResolver.query(uri, proj, null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) {
                val idx = cursor.getColumnIndex(proj[0])
                if (idx >= 0) return cursor.getString(idx) ?: uri.lastPathSegment
            }
        }
        return uri.lastPathSegment ?: "file.bin"
    }

    private fun downloadFile(file: FilesClient.InboxItem.FileItem) {
        val c = client ?: return
        val dir = getExternalFilesDir(null) ?: filesDir
        setBusy(true)
        executor.execute {
            var err: String? = null
            var saved: java.io.File? = null
            try {
                saved = c.downloadTo(file.id, dir)
            } catch (e: FilesClient.ApiException) {
                err = describe(e)
            } catch (e: java.io.IOException) {
                err = describe(e)
            }
            runOnUiThread {
                setBusy(false)
                if (err != null) {
                    showError(err)
                    return@runOnUiThread
                }
                Toast.makeText(
                    this, getString(R.string.files_downloaded_to, saved?.absolutePath ?: ""),
                    Toast.LENGTH_LONG,
                ).show()
            }
        }
    }

    private fun deleteFile(id: Long) {
        val c = client ?: return
        setBusy(true)
        executor.execute {
            var err: String? = null
            try {
                c.deleteFile(id)
            } catch (e: FilesClient.ApiException) {
                err = describe(e)
            } catch (e: java.io.IOException) {
                err = describe(e)
            }
            runOnUiThread {
                setBusy(false)
                if (err != null) showError(err) else refresh()
            }
        }
    }

    /** 外网模式删自己的上传（POST /uplink/delete?id=；服务端守卫他人/内网文件） */
    private fun deleteUplinkFile(id: Long) {
        val u = uplink ?: return
        setBusy(true)
        executor.execute {
            var err: String? = null
            try {
                u.delete(id)
            } catch (e: UplinkClient.ApiException) {
                err = describe(e)
            } catch (e: java.io.IOException) {
                err = describe(e)
            }
            runOnUiThread {
                setBusy(false)
                if (err != null) showError(err) else refresh()
            }
        }
    }

    // —— 展示 ——

    /** 失败明示：状态码＋服务端文案直接示人（401 会话过期单独引导重连） */
    private fun describe(e: java.io.IOException): String =
        when {
            e is UplinkClient.ApiException && e.status == 401 -> getString(R.string.files_err_session)
            e is UplinkClient.ApiException && e.status == 0 -> getString(R.string.err_unreachable)
            e is UplinkClient.ApiException -> getString(R.string.files_err_prefix, e.status, e.error)
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
        btnMemoSave.isEnabled = !busy
        btnUpload.isEnabled = !busy
    }

    private fun setConnected(connected: Boolean) {
        btnConnect.setText(if (connected) R.string.files_disconnect else R.string.files_connect)
        tvState.setText(
            when {
                connected && uplink?.isLoggedIn == true -> R.string.files_state_on_uplink
                connected -> R.string.files_state_on
                else -> R.string.files_state_off
            }
        )
        etAccount.isEnabled = !connected
        etPassword.isEnabled = !connected
        etPort.isEnabled = !connected
        if (!connected) tvEmpty.visibility = View.GONE
    }

    /** 外网模式收窄功能面：tab 与备忘录行隐藏（外网面只有上传/记录/删除）；
     *  端口语义换外网口（无缺省——部署明示，清空防拿内网口当外网口） */
    private fun renderUplinkMode() {
        val uplinkMode = cbUplink.isChecked
        btnTabInbox.visibility = if (uplinkMode) View.GONE else View.VISIBLE
        btnTabMe.visibility = if (uplinkMode) View.GONE else View.VISIBLE
        llMemoRow.visibility = if (uplinkMode) View.GONE else View.VISIBLE
        etPort.hint = getString(
            if (uplinkMode) R.string.files_port_hint_uplink else R.string.files_port_hint
        )
        if (!etPort.isEnabled) return // 已连接态不动端口值
        if (uplinkMode) {
            if (etPort.text.toString() == FilesClient.DEFAULT_FILES_PORT.toString()) {
                etPort.setText("")
            }
            tvEmpty.setText(R.string.files_empty_uplink)
        } else {
            if (etPort.text.isNullOrBlank()) {
                etPort.setText(FilesClient.DEFAULT_FILES_PORT.toString())
            }
            renderTabs()
        }
    }

    private fun renderTabs() {
        val inboxActive = target == TARGET_INBOX
        btnTabInbox.isEnabled = !inboxActive
        btnTabMe.isEnabled = inboxActive
        tvEmpty.setText(if (inboxActive) R.string.files_empty else R.string.files_empty_me)
    }

    /** 混排行：备忘录＝正文预览；文件＝原名＋体积；第二行时间 */
    private inner class ItemsAdapter : BaseAdapter() {
        private var items: List<FilesClient.InboxItem> = emptyList()

        fun update(list: List<FilesClient.InboxItem>) {
            items = list
            notifyDataSetChanged()
        }

        override fun getCount() = items.size
        override fun getItem(position: Int): FilesClient.InboxItem? =
            if (position in items.indices) items[position] else null

        override fun getItemId(position: Int) = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val view = convertView ?: layoutInflater.inflate(
                android.R.layout.simple_list_item_2, parent, false
            )
            val item = items[position]
            val title = view.findViewById<TextView>(android.R.id.text1)
            val subtitle = view.findViewById<TextView>(android.R.id.text2)
            when (item) {
                is FilesClient.InboxItem.Memo -> {
                    title.text = getString(R.string.files_row_memo, item.content)
                    subtitle.text = fmtTime(item.updatedMs)
                }
                is FilesClient.InboxItem.FileItem -> {
                    title.text =
                        getString(R.string.files_row_file, item.fileName, fmtSize(item.fileSize))
                    subtitle.text = fmtTime(item.uploadTs)
                }
            }
            return view
        }

        private fun fmtTime(tsMs: Long): String = try {
            SimpleDateFormat("MM-dd HH:mm", Locale.getDefault()).format(Date(tsMs))
        } catch (_: Exception) {
            ""
        }

        private fun fmtSize(bytes: Long): String = when {
            bytes >= 1 shl 20 -> getString(R.string.files_size_mb, bytes shr 20)
            bytes >= 1 shl 10 -> getString(R.string.files_size_kb, bytes shr 10)
            else -> getString(R.string.files_size_b, bytes)
        }
    }

    companion object {
        private const val TARGET_INBOX = "inbox"
        private const val TARGET_ME = "me"
    }
}
