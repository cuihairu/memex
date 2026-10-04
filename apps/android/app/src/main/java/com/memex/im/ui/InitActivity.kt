package com.memex.im.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.memex.im.R
import com.memex.im.core.InitEntry
import com.memex.im.core.InitGate
import com.memex.im.core.InitStore
import com.memex.im.core.MemexClient
import com.memex.im.core.PrefsInitStore
import com.memex.im.core.ProbeResult
import com.memex.im.core.ServerAddress
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * 初始化向导（R17）：首次启动强制进入的服务器地址设置页。
 * 无跳过路径——「校验并保存」必须拿到 PONG 才落盘并放行去登录页。
 */
class InitActivity : AppCompatActivity() {

    private lateinit var store: InitStore
    private val client = MemexClient()
    private val executor: ExecutorService = Executors.newSingleThreadExecutor()

    private lateinit var etAddress: EditText
    private lateinit var tvError: TextView
    private lateinit var pbBusy: ProgressBar
    private lateinit var btnProbe: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        store = PrefsInitStore(this)
        val reconfigure =
            intent.getBooleanExtra(EXTRA_RECONFIGURE, false)

        when (InitGate.entry(store, reconfigure)) {
            // 常规进入但已初始化：向导不是回头路，改道登录页
            InitEntry.TO_LOGIN -> {
                startActivity(Intent(this, LoginActivity::class.java))
                finish()
                return
            }
            InitEntry.SHOW_FORM -> Unit
        }

        setContentView(R.layout.activity_init)
        etAddress = findViewById(R.id.et_server_address)
        tvError = findViewById(R.id.tv_init_error)
        pbBusy = findViewById(R.id.pb_init)
        btnProbe = findViewById(R.id.btn_probe)

        btnProbe.setOnClickListener { onProbeClicked() }
    }

    private fun onProbeClicked() {
        tvError.visibility = View.GONE
        when (val parsed = ServerAddress.parse(etAddress.text.toString())) {
            is ServerAddress.Parsed.Err -> showAddressError(parsed.reason)
            is ServerAddress.Parsed.Ok -> {
                setBusy(true)
                executor.execute {
                    val result = client.probe(parsed.address)
                    runOnUiThread { setBusy(false); renderProbe(result, parsed.address) }
                }
            }
        }
    }

    private fun renderProbe(result: ProbeResult, address: ServerAddress) {
        when (result) {
            is ProbeResult.Ok -> {
                store.markInitialized(address)
                Toast.makeText(this, R.string.init_ok_toast, Toast.LENGTH_SHORT).show()
                // 校验通过后的唯一去向：登录页（R18：登录后才可用）
                startActivity(Intent(this, LoginActivity::class.java))
                finish()
            }
            is ProbeResult.Unreachable -> showError(getString(R.string.err_unreachable))
            is ProbeResult.Timeout -> showError(getString(R.string.err_timeout))
            is ProbeResult.NotMemex -> showError(getString(R.string.err_not_memex))
        }
    }

    private fun showAddressError(reason: ServerAddress.Reason) {
        val res = when (reason) {
            ServerAddress.Reason.EMPTY -> R.string.err_addr_empty
            ServerAddress.Reason.BAD_HOST -> R.string.err_addr_bad_host
            ServerAddress.Reason.BAD_PORT -> R.string.err_addr_bad_port
            ServerAddress.Reason.SCHEME -> R.string.err_addr_scheme
            ServerAddress.Reason.IPV6_FORM -> R.string.err_addr_ipv6
        }
        showError(getString(res))
    }

    private fun showError(text: String) {
        tvError.text = text
        tvError.visibility = View.VISIBLE
    }

    private fun setBusy(busy: Boolean) {
        pbBusy.visibility = if (busy) View.VISIBLE else View.GONE
        btnProbe.isEnabled = !busy
        etAddress.isEnabled = !busy
    }

    override fun onDestroy() {
        executor.shutdownNow()
        super.onDestroy()
    }

    companion object {
        const val EXTRA_RECONFIGURE = "reconfigure"
    }
}
