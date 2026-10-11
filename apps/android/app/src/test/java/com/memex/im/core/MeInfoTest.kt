package com.memex.im.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** 「我」屏账号卡组装：ORG_DATA 成员取本人与文案拼接（不留假数据的口径） */
class MeInfoTest {

    private val members = listOf(
        OrgMemberLite(
            account = "alice",
            displayName = "张伟",
            title = "工程师",
            departmentPath = "公司/产品研发部/研发一组",
            role = "member",
        ),
        OrgMemberLite(
            account = "bob",
            displayName = "李四",
            title = "",
            departmentPath = "",
            role = "admin",
        ),
    )

    @Test
    fun `按账号取本人`() {
        assertEquals("张伟", MeInfo.self("alice", members)?.displayName)
        assertEquals("李四", MeInfo.self("bob", members)?.displayName)
        assertNull(MeInfo.self("carol", members))
        assertNull(MeInfo.self("alice", emptyList()))
    }

    @Test
    fun `展示名优先组织架构真实姓名 否则回退登录态`() {
        assertEquals("张伟", MeInfo.nameLine(MeInfo.self("alice", members), "alice"))
        // 组织架构查不到本人（未加载/被可见性隐藏）：回退登录态展示名（当前＝账号）
        assertEquals("carol", MeInfo.nameLine(null, "carol"))
        // 协议允许 display_name 为空：同样回退
        val emptyName = OrgMemberLite("eve", "", "职务", "部门", "member")
        assertEquals("eve", MeInfo.nameLine(emptyName, "eve"))
    }

    @Test
    fun `部门行拼接 缺项跳过`() {
        assertEquals(
            "公司/产品研发部/研发一组 · 工程师 · 账号 alice",
            MeInfo.deptLine(MeInfo.self("alice", members), "alice"),
        )
        // bob 部门与职务皆空：只剩账号
        assertEquals("账号 bob", MeInfo.deptLine(MeInfo.self("bob", members), "bob"))
        // 查不到本人：如实只报账号（协议无「工号」字段，不杜撰）
        assertEquals("账号 carol", MeInfo.deptLine(null, "carol"))
    }
}
