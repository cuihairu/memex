package com.memex.im.core

import com.memex.im.core.ServerAddress.Parsed
import com.memex.im.core.ServerAddress.Reason
import org.junit.Assert.assertEquals
import org.junit.Test

/** R17 第一步输入域：域名／IP／host:port／[IPv6]:port 解析与校验 */
class ServerAddressTest {

    private fun ok(raw: String): ServerAddress =
        (ServerAddress.parse(raw) as Parsed.Ok).address

    private fun err(raw: String): Reason =
        (ServerAddress.parse(raw) as Parsed.Err).reason

    @Test
    fun `纯主机名默认端口 24360`() {
        assertEquals(ServerAddress("192.168.1.10", 24360), ok("192.168.1.10"))
        assertEquals(ServerAddress("im.example.com", 24360), ok("im.example.com"))
    }

    @Test
    fun `host 冒号 port`() {
        assertEquals(ServerAddress("im.example.com", 24360), ok("im.example.com:24360"))
        assertEquals(ServerAddress("10.0.0.2", 8000), ok("10.0.0.2:8000"))
    }

    @Test
    fun `容忍空白与粘贴的完整地址`() {
        assertEquals(ServerAddress("im.example.com", 24360), ok("  im.example.com:24360  "))
        assertEquals(ServerAddress("im.example.com", 9000), ok("http://im.example.com:9000/path?q=1"))
        assertEquals(ServerAddress("im.example.com", 24360), ok("http://im.example.com/"))
    }

    @Test
    fun `IPv6 方括号形态`() {
        assertEquals(ServerAddress("::1", 24360), ok("[::1]"))
        assertEquals(ServerAddress("fe80::1", 24361), ok("[fe80::1]:24361"))
    }

    @Test
    fun `空与空白`() {
        assertEquals(Reason.EMPTY, err(""))
        assertEquals(Reason.EMPTY, err("   "))
        assertEquals(Reason.EMPTY, err("http://"))
    }

    @Test
    fun `端口非法`() {
        assertEquals(Reason.BAD_PORT, err("host:"))
        assertEquals(Reason.BAD_PORT, err("host:0"))
        assertEquals(Reason.BAD_PORT, err("host:99999"))
        assertEquals(Reason.BAD_PORT, err("host:abc"))
    }

    @Test
    fun `主机名非法`() {
        assertEquals(Reason.BAD_HOST, err(":24360"))
        assertEquals(Reason.BAD_HOST, err("has space:24360"))
        assertEquals(Reason.BAD_HOST, err("-leading.dash"))
        assertEquals(Reason.BAD_HOST, err("bad;name"))
    }

    @Test
    fun `裸 IPv6 与括号残缺`() {
        assertEquals(Reason.IPV6_FORM, err("::1"))
        assertEquals(Reason.IPV6_FORM, err("fe80::1:24360"))
        assertEquals(Reason.IPV6_FORM, err("[::1"))
        assertEquals(Reason.IPV6_FORM, err("[::1]x:1"))
        assertEquals(Reason.IPV6_FORM, err("[]:24360"))
    }

    @Test
    fun `不支持的前缀`() {
        assertEquals(Reason.SCHEME, err("ftp://im.example.com"))
        assertEquals(Reason.SCHEME, err("memex://im.example.com"))
    }

    @Test
    fun `展示形态`() {
        assertEquals("im.example.com:24360", ServerAddress("im.example.com", 24360).display())
        assertEquals("[::1]:24360", ServerAddress("::1", 24360).display())
    }

    @Test(expected = IllegalArgumentException::class)
    fun `端口越界构造直接拒绝`() {
        ServerAddress("host", 65536)
    }
}
