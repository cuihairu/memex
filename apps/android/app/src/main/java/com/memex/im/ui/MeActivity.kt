package com.memex.im.ui

import android.content.Intent
import android.os.Bundle
import android.provider.Settings
import android.view.View
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.memex.im.BuildConfig
import com.memex.im.R
import com.memex.im.core.ChatHolder
import com.memex.im.core.ChatManager
import com.memex.im.core.DeviceIdentity
import com.memex.im.core.InitStore
import com.memex.im.core.MeInfo
import com.memex.im.core.NoticeGrade
import com.memex.im.core.OrgMemberLite
import com.memex.im.core.PrefsInitStore
import com.memex.im.core.Route
import com.memex.im.core.RouteGuard
import com.memex.im.core.ServerAddress
import com.memex.im.core.Session

/**
 * 「我」屏（原型 docs/design/prototypes/mobile-me.html）：账号卡＋账号与设备＋
 * 通用＋切换服务器/退出登录。入口在主界面（btn_me），不新造 tab 结构。
 *
 * 数据口径（如实边界）：
 * - 账号卡姓名/部门/职务：ORG_QUERY/ORG_DATA（服务端已有能力，T3.1）；
 * - 登录设备管理：仅本机事实（Android 机型＋设备指纹）；服务端设备台账
 *   （devices 表）只有 CLI 查询面，协议无查询消息——如实标「未接入」；
 * - 登录记录（login_records 表）与消息留存策略（retention 表）同理未接入；
 * - 本地缓存清理：仅清本机内存暂存（ChatStore.clearAll），服务端归档不动。
 */
class MeActivity : AppCompatActivity(), ChatManager.Listener {

    private lateinit var store: InitStore
    private lateinit var tvName: TextView
    private lateinit var tvDept: TextView
    private lateinit var tvState: TextView
    private lateinit var tvDevSub: TextView
    private lateinit var tvAbout: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        store = PrefsInitStore(this)
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

        setContentView(R.layout.activity_me)
        tvName = findViewById(R.id.tv_me_name)
        tvDept = findViewById(R.id.tv_me_dept)
        tvState = findViewById(R.id.tv_me_state)
        tvDevSub = findViewById(R.id.tv_me_dev_sub)
        tvAbout = findViewById(R.id.tv_me_about)

        val account = Session.instance.account ?: ""
        val address = store.serverAddress()
        tvName.text = Session.instance.displayName ?: account
        tvDept.text = getString(R.string.me_org_loading)
        tvState.text = getString(
            R.string.me_state_collab,
            if (address == null) "?" else shownHost(address),
        )
        tvAbout.text = getString(
            R.string.me_about_value, BuildConfig.VERSION_NAME, BuildConfig.VERSION_CODE,
        )
        tvDevSub.text = getString(
            R.string.me_devices_sub,
            android.os.Build.MODEL,
            deviceFingerprint().take(8),
        )
        findViewById<TextView>(R.id.tv_me_avatar).text =
            (Session.instance.displayName ?: account).take(1).ifEmpty { "?" }

        findViewById<View>(R.id.row_me_cache).setOnClickListener { confirmClearCache() }
        findViewById<View>(R.id.row_me_notify).setOnClickListener { openNotifySettings() }
        findViewById<View>(R.id.row_me_about).setOnClickListener { showAbout() }
        findViewById<View>(R.id.row_me_actions).setOnClickListener { showSwitchOrLogout() }
    }

    override fun onResume() {
        super.onResume()
        ChatHolder.manager?.let { mgr ->
            mgr.addListener(this)
            // 账号卡组织资料（姓名/部门/职务）随每次进入拉取；服务端按通讯录
            // 可见性过滤下发，查不到本人时如实回退账号
            mgr.queryOrg()
        }
    }

    override fun onPause() {
        super.onPause()
        ChatHolder.manager?.removeListener(this)
    }

    // —— ChatManager.Listener（UI 线程）——
    override fun onNewMessage(peer: String) = Unit

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
        // 与主界面同口径：被互踢即清态回登录页（本页也订阅，主界面此时已摘听）
        ChatHolder.clear()
        Session.instance.signOut()
        Toast.makeText(this, reason, Toast.LENGTH_LONG).show()
        startActivity(Intent(this, LoginActivity::class.java))
        finish()
    }

    override fun onOrgData(members: List<OrgMemberLite>) {
        val account = Session.instance.account ?: return
        val self = MeInfo.self(account, members)
        tvName.text = MeInfo.nameLine(self, Session.instance.displayName ?: account)
        tvDept.text = MeInfo.deptLine(self, account)
    }

    private fun confirmClearCache() {
        AlertDialog.Builder(this)
            .setTitle(R.string.me_cache_confirm_title)
            .setMessage(R.string.me_cache_confirm_msg)
            .setNegativeButton(R.string.files_cancel, null)
            .setPositiveButton(R.string.me_cache_action) { _, _ ->
                ChatHolder.manager?.store?.clearAll()
                Toast.makeText(this, R.string.me_cache_done, Toast.LENGTH_SHORT).show()
            }
            .show()
    }

    private fun openNotifySettings() {
        // 通知渠道与横幅开关在系统设置（Android 8+ 应用通知设置页）
        val intent = Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
            .putExtra(Settings.EXTRA_APP_PACKAGE, packageName)
        try {
            startActivity(intent)
        } catch (_: Exception) {
            startActivity(Intent(Settings.ACTION_SETTINGS))
        }
    }

    private fun showAbout() {
        AlertDialog.Builder(this)
            .setTitle(R.string.me_about)
            .setMessage(
                getString(
                    R.string.me_about_msg,
                    BuildConfig.VERSION_NAME,
                    BuildConfig.VERSION_CODE,
                )
            )
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    private fun showSwitchOrLogout() {
        val items = arrayOf(
            getString(R.string.me_action_switch),
            getString(R.string.me_action_logout),
        )
        AlertDialog.Builder(this)
            .setItems(items) { _, which ->
                when (which) {
                    0 -> switchServer()
                    else -> logout()
                }
            }
            .show()
    }

    /** 切换服务器：清态回初始化向导（重设地址须过连通性校验，InitGate） */
    private fun switchServer() {
        ChatHolder.clear()
        Session.instance.signOut()
        startActivity(
            Intent(this, InitActivity::class.java)
                .putExtra(InitActivity.EXTRA_RECONFIGURE, true)
        )
        finish()
    }

    private fun logout() {
        ChatHolder.clear()
        Session.instance.signOut()
        startActivity(Intent(this, LoginActivity::class.java))
        finish()
    }

    /** 展示用服务器标识：默认端口只显域名/IP，非默认端口带端口 */
    private fun shownHost(address: ServerAddress): String =
        if (address.port == ServerAddress.DEFAULT_PORT) address.host else address.display()

    private fun deviceFingerprint(): String {
        val androidId =
            Settings.Secure.getString(contentResolver, Settings.Secure.ANDROID_ID) ?: ""
        return DeviceIdentity.androidFingerprint(
            androidId, android.os.Build.DEVICE, android.os.Build.MODEL,
        )
    }
}
