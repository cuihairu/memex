package com.memex.im.ui

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.BaseAdapter
import android.widget.Button
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

        // 账号预填协作登录态（口令不回填——不落盘）
        Session.instance.account?.let { etAccount.setText(it) }
        etPort.setText(FilesClient.DEFAULT_FILES_PORT.toString())

        btnConnect.setOnClickListener { onConnectClicked() }
        btnMemoSave.setOnClickListener { onMemoSaveClicked() }
        btnUpload.setOnClickListener { pickFile.launch(arrayOf("*/*")) }
        btnTabInbox.setOnClickListener { switchTarget(TARGET_INBOX) }
        btnTabMe.setOnClickListener { switchTarget(TARGET_ME) }
        findViewById<Button>(R.id.btn_files_refresh).setOnClickListener { refresh() }

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
            val c = FilesClient()
            var err: String? = null
            try {
                c.login(host, port, account, password)
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
        val c = client ?: return
        setBusy(true)
        executor.execute {
            var err: String? = null
            var items: List<FilesClient.InboxItem> = emptyList()
            try {
                items = if (target == TARGET_INBOX) c.listInbox() else c.listPersonal()
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
        val c = client ?: return
        val name = queryDisplayName(uri) ?: return
        setBusy(true)
        executor.execute {
            var err: String? = null
            try {
                val bytes = contentResolver.openInputStream(uri)?.use { it.readBytes() }
                if (bytes == null) err = getString(R.string.files_err_read)
                else c.upload(target, name, bytes)
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

    // —— 展示 ——

    /** 失败明示：状态码＋服务端文案直接示人（401 会话过期单独引导重连） */
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
        btnMemoSave.isEnabled = !busy
        btnUpload.isEnabled = !busy
    }

    private fun setConnected(connected: Boolean) {
        btnConnect.setText(if (connected) R.string.files_disconnect else R.string.files_connect)
        tvState.setText(if (connected) R.string.files_state_on else R.string.files_state_off)
        etAccount.isEnabled = !connected
        etPassword.isEnabled = !connected
        etPort.isEnabled = !connected
        if (!connected) tvEmpty.visibility = View.GONE
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
