package com.memex.im.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * R18 的结构性验收（界面面）：全部界面无免登录/匿名入口。
 * 1) 唯一 launcher 入口是守卫过的 MainActivity；无其他 exported activity；
 * 2) 全部 res 文案不出现「免登录/匿名/附近设备/直连」类字样；
 * 3) 文本一律走 @string（不留硬编码入口文案绕过第 2 条的扫描面）。
 */
class R18SurfaceTest {

    private fun moduleDir(): File {
        var dir = File(System.getProperty("user.dir")).absoluteFile
        repeat(8) {
            if (File(dir, "src/main/AndroidManifest.xml").exists()) return dir
            dir = dir.parentFile ?: return@repeat
        }
        error("找不到模块目录（user.dir=${System.getProperty("user.dir")}）")
    }

    @Test
    fun `唯一 launcher 入口是守卫的 MainActivity`() {
        val manifest = File(moduleDir(), "src/main/AndroidManifest.xml").readText()

        val activities = Regex("""<activity\b[^>]*name="([^"]+)"[\s\S]*?</activity>|<activity\b[^>]*/>""")
            .findAll(manifest)
            .map { it.groupValues[1] }
            .toList()
        assertTrue("至少应登记三个界面", activities.size >= 3)
        assertEquals(
            "LAUNCHER 只能出现一次",
            1,
            Regex("android.intent.category.LAUNCHER").findAll(manifest).count()
        )

        // 唯一 launcher 必须挂在守卫入口 MainActivity 上
        val activityBlocks = Regex("""<activity\b[\s\S]*?</activity>""").findAll(manifest).map { it.value }.toList()
        val launcherBlock = activityBlocks.first { it.contains("LAUNCHER") }
        assertTrue("launcher 必须是 MainActivity", launcherBlock.contains("com.memex.im.ui.MainActivity"))

        // MainActivity 之外不允许 exported（不给任何旁路入口）
        for (block in activityBlocks) {
            val isMain = block.contains("com.memex.im.ui.MainActivity")
            val exported = Regex("""android:exported="true"""").containsMatchIn(block)
            assertEquals("仅 MainActivity 可 exported：$block", isMain, exported)
        }
    }

    @Test
    fun `res 文案无免登录匿名类入口字样`() {
        val forbidden = listOf("免登录", "匿名", "附近设备", "免注册", "直连")
        val resDir = File(moduleDir(), "src/main/res")
        assertTrue(resDir.isDirectory)
        val offenders = resDir.walkTopDown()
            .filter { it.isFile }
            .map { it to it.readText() }
            .flatMap { (f, text) -> forbidden.filter { text.contains(it) }.map { "${f.name}:$it" } }
            .toList()
        assertTrue("出现禁用文案：$offenders", offenders.isEmpty())
    }

    @Test
    fun `文本一律走 string 资源`() {
        val resDir = File(moduleDir(), "src/main/res")
        val offenders = resDir.walkTopDown()
            .filter { it.isFile && it.extension == "xml" }
            .flatMap { f ->
                Regex("""android:text="([^@][^"]*)"""").findAll(f.readText())
                    .map { "${f.name}:${it.groupValues[1]}" }
            }.toList()
        assertTrue("存在硬编码文案（绕过扫描面）：$offenders", offenders.isEmpty())
    }

    @Test
    fun `设备指纹为 sha256 hex`() {
        assertEquals(64, DeviceIdentity.fingerprint("seed").length)
        assertTrue(DeviceIdentity.fingerprint("seed").all { it in '0'..'9' || it in 'a'..'f' })
        // RFC 已知向量：sha256("abc")
        assertEquals(
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            DeviceIdentity.sha256Hex("abc".toByteArray(Charsets.UTF_8))
        )
        assertFalse(DeviceIdentity.fingerprint("a") == DeviceIdentity.fingerprint("b"))
    }
}
