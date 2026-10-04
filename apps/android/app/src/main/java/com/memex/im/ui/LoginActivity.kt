package com.memex.im.ui

import android.content.Intent
import android.os.Bundle
import android.provider.Settings
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.ProgressBar
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.memex.im.BuildConfig
import com.memex.im.R
import com.memex.im.core.ChatHolder
import com.memex.im.core.ChatSession
import com.memex.im.core.DeviceIdentity
import com.memex.im.core.InitStore
import com.memex.im.core.PrefsInitStore
import com.memex.im.core.ServerAddress
import com.memex.im.core.Session
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * 登录页（R18）：移动端全部为协作态，必须登录后使用。
 * 本页自身也守卫：未初始化（例如清除了应用数据）一律改道向导。
 * 登录验证与长连接建立一次完成（ChatManager.attach）：成功即移交
 * ChatHolder，主界面/聊天页直接消费；避免「验证连接＋常连接」双连接互踢。
 */
class LoginActivity : AppCompatActivity() {

    private lateinit var store: InitStore
    private val executor: ExecutorService = Executors.newSingleThreadExecutor()

    private lateinit var etAccount: EditText
    private lateinit var etPassword: EditText
    private lateinit var tvError: TextView
    private lateinit var pbBusy: ProgressBar
    private lateinit var btnLogin: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        store = PrefsInitStore(this)

        // 防御守卫：初始化态被清（清数据/多进程边界）时不允许停在登录页
        if (!store.isInitialized()) {
            startActivity(Intent(this, InitActivity::class.java))
            finish()
            return
        }

        setContentView(R.layout.activity_login)
        etAccount = findViewById(R.id.et_account)
        etPassword = findViewById(R.id.et_password)
        tvError = findViewById(R.id.tv_login_error)
        pbBusy = findViewById(R.id.pb_login)
        btnLogin = findViewById(R.id.btn_login)

        btnLogin.setOnClickListener { onLoginClicked() }
        findViewById<View>(R.id.tv_change_server).setOnClickListener {
            startActivity(
                Intent(this, InitActivity::class.java)
                    .putExtra(InitActivity.EXTRA_RECONFIGURE, true)
            )
        }
    }

    private fun onLoginClicked() {
        tvError.visibility = View.GONE
        val account = etAccount.text.toString().trim()
        val password = etPassword.text.toString()
        if (account.isEmpty()) {
            etAccount.error = getString(R.string.login_account_hint)
            return
        }
        val address = store.serverAddress() ?: run {
            // 落盘地址缺失视同未初始化，回向导
            startActivity(Intent(this, InitActivity::class.java))
            finish()
            return
        }
        setBusy(true)
        executor.execute {
            val manager = ChatHolder.establish()
            val outcome = manager.attach(
                address = address,
                password = password,
                account = account,
                displayName = account,
                deviceFingerprint = deviceFingerprint(),
                deviceName = android.os.Build.MODEL,
                clientVersion = BuildConfig.VERSION_NAME,
            )
            runOnUiThread { setBusy(false); renderLogin(outcome, account) }
        }
    }

    private fun renderLogin(outcome: ChatSession.ConnectOutcome, account: String) {
        when (outcome) {
            is ChatSession.ConnectOutcome.Ok -> {
                Session.instance.signIn(account, displayName(account))
                startActivity(Intent(this, MainActivity::class.java))
                finish()
            }
            is ChatSession.ConnectOutcome.Rejected -> showError(outcome.reason)
            is ChatSession.ConnectOutcome.Unreachable -> showError(getString(R.string.err_unreachable))
            is ChatSession.ConnectOutcome.Timeout -> showError(getString(R.string.err_timeout))
            is ChatSession.ConnectOutcome.NotMemex -> showError(getString(R.string.err_not_memex))
        }
    }

    private fun displayName(account: String): String =
        // 展示名由服务端 LOGIN_RESULT.display_name 回填；当前协议版本服务端
        // 未回填时用账号兜底（对齐桌面端 CollabEngine 的显示逻辑）
        account

    private fun showError(text: String) {
        tvError.text = text
        tvError.visibility = View.VISIBLE
    }

    private fun setBusy(busy: Boolean) {
        pbBusy.visibility = if (busy) View.VISIBLE else View.GONE
        btnLogin.isEnabled = !busy
        etAccount.isEnabled = !busy
        etPassword.isEnabled = !busy
    }

    private fun deviceFingerprint(): String {
        val androidId =
            Settings.Secure.getString(contentResolver, Settings.Secure.ANDROID_ID) ?: ""
        return DeviceIdentity.fingerprint(
            "android:$androidId:${android.os.Build.DEVICE}:${android.os.Build.MODEL}"
        )
    }

    override fun onDestroy() {
        executor.shutdownNow()
        super.onDestroy()
    }
}
