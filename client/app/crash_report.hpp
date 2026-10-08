// 崩溃采集（Crashpad）客户端面：简档见 docs/src/guide/crash-reporting.md。
// 初始化点＝main() 最早段（QApplication 构造后、业务引擎/窗口构造前）；
// dump 只本地落盘（上报 URL 恒空＝不外发，外发属数据外发默认关）。
#pragma once

namespace memex::client {

// 启动 Crashpad 独立 handler（crashpad_handler 随包与客户端同目录，运行时按
// 「可执行同目录→PATH」探寻）。成败都写既有日志（[崩溃采集] 前缀），并列出
// 本地既有 dump 份数据与运行日志对时。返回 handler 是否启动成功。
bool init_crash_reporting();

}  // namespace memex::client
