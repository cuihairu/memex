// memex 管理后台（admin 2026-10-11 用户令）：Drogon 独立进程，默认监听
// 8089（用户已配 admin.memex.cuihairu.site → http://192.168.5.5:8089）。
// 波次 1＝直连库管理面：登录（与 /files/session 同源 PBKDF2 口令校验，
// 不依赖文件面）、账号与组织、策略下发、留存策略、归档检索与导出＋在线
// 审计（auditor 证，平台-6 红线）、设备与登录台账、健康状态。
// 判权复用服务端 az 引擎（authz.cpp）——规则在此按 files_server 既有语义
// 镜像注册（org-admin=组织级管理、auditor=审计查阅），引擎与角色集都
// 是服务端同一套，不自造口径。数据面直连 ServerStore（与 CLI 同径，
// busy_timeout 5s 并发口径），不动现有 FileServer 与协议。
#pragma once

namespace memex::admin {

// 配置与路由注册（不阻塞；argv 覆盖 env，env 覆盖默认）。返回 0＝就绪
// 可 run。测试在进程内 setup + 另线程 run + 退出时 drogon::app().quit()。
//   --port N            监听端口（env MEMEX_ADMIN_PORT，默认 8089）
//   --db P              服务端 SQLite 库路径（env MEMEX_ADMIN_DB）
//   --www D             静态页目录（env MEMEX_ADMIN_WWW，默认 ./www）
//   --files-url U       文件面基地址（env MEMEX_FILES_URL；空＝健康页不探）
//   --session-ttl S     会话超时秒（env MEMEX_ADMIN_SESSION_TTL，默认 1800）
int setup(int argc, char** argv);

// setup + 阻塞运行 Drogon 主循环（生产入口；setup 失败即返回其错误码）。
int run(int argc, char** argv);

} // namespace memex::admin
