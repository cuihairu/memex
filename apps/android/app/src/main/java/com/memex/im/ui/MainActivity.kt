package com.memex.im.ui

import android.content.Intent
import android.os.Bundle
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.memex.im.R
import com.memex.im.core.PrefsInitStore
import com.memex.im.core.Route
import com.memex.im.core.RouteGuard
import com.memex.im.core.Session

/**
 * 主界面（launcher 唯一入口）。onCreate 先过 RouteGuard：
 * 未初始化 → 向导；已初始化未登录 → 登录页；都不满足不进入本界面内容。
 * 会话列表/收发/通知面随 T6.3 后续块落地，此处为登录态展示占位。
 */
class MainActivity : AppCompatActivity() {

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

        findViewById<TextView>(R.id.tv_server).text =
            getString(R.string.main_server, store.serverAddress()?.display() ?: "-")
        findViewById<TextView>(R.id.tv_account).text =
            getString(R.string.main_account, Session.instance.account ?: "-")
        findViewById<TextView>(R.id.tv_display).text =
            getString(R.string.main_display, Session.instance.displayName ?: "-")

        findViewById<Button>(R.id.btn_logout).setOnClickListener {
            Session.instance.signOut()
            startActivity(Intent(this, LoginActivity::class.java))
            finish()
        }
    }
}
