package com.memex.im.core

/** 组织架构成员的移动端取用子集（ORG_DATA.members，「我」屏账号卡数据面） */
data class OrgMemberLite(
    val account: String,
    val displayName: String,
    val title: String,
    val departmentPath: String,
    val role: String,
)

/**
 * 「我」屏账号卡组装（纯逻辑，JVM 单测直测）。
 * 数据只来自登录态与 ORG_DATA（服务端已有能力，T3.1）；组织架构查不到本人
 * （未加载/被通讯录可见性隐藏）时如实回退为账号本身，不留假部门假职务。
 */
object MeInfo {

    /** 从组织架构成员表取本人（按账号；查不到=null） */
    fun self(account: String, members: List<OrgMemberLite>): OrgMemberLite? =
        members.firstOrNull { it.account == account }

    /** 展示名：组织架构有真实姓名用之；否则回退登录态展示名（当前＝账号） */
    fun nameLine(self: OrgMemberLite?, fallback: String): String =
        self?.displayName?.takeIf { it.isNotEmpty() } ?: fallback

    /**
     * 部门行文案：部门全路径 · 职务 · 账号（缺项跳过；协议无「工号」字段，
     * 以账号作身份标识——对齐服务端口径，不杜撰工号）。
     */
    fun deptLine(self: OrgMemberLite?, account: String): String {
        if (self == null) return "账号 $account"
        val parts = buildList {
            if (self.departmentPath.isNotEmpty()) add(self.departmentPath)
            if (self.title.isNotEmpty()) add(self.title)
            add("账号 $account")
        }
        return parts.joinToString(" · ")
    }
}
