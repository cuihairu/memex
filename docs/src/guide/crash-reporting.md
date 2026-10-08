# 崩溃采集（Crashpad）

> 崩溃采集批（2026-10-08 用户令）：选型已钉 **Google Crashpad**（C++ 直集成；
> Breakpad 仅作历史对照，不引入）。本页先落简档（设计），实现随批续写；
> 验收＝故意崩溃→dump 落盘＋符号化还原调用栈。

## 选型

- **Crashpad**：Chrome 系下一代崩溃采集——崩溃报告由**独立 handler 进程**
  完成（目标进程崩溃瞬间只剩最小信号处理，落盘/压缩/上传都在 handler 里，
  目标进程栈已被破坏也能收尾）；支持随 dump 附带元数据与附件；上游活跃。
- **Breakpad（历史对照，不引入）**：Crashpad 前身——报告采集在目标进程内
  完成（fork 出 in-process reporter），收尾可靠性弱于独立进程方案；上游
  已冻结（仅维护），新项目官方建议直接用 Crashpad。
- minidump 格式两者兼容：`minidump_stackwalk` 工具链通用。

## 简档（设计口径）

- **接入方式**：vcpkg manifest 依赖 `crashpad`（Apache-2.0；仓库钉定的
  baseline 自带 port，产出 client/common/util/base 四个静态库＋独立
  `crashpad_handler` 可执行与 `crashpadConfig.cmake` 导出面）。仓库纪律
  「组件均由 vcpkg 按 manifest 获取与构建，不 vendor 源码」优先于
  子模块/fetchcontent 字面路径——两者目的相同（拉上游源码构建），vcpkg
  路线免维护第三方源码树且与既有组件（Qt/OpenSSL/asio…）同一管线。
  符号化工具 `dump_syms`／`minidump_stackwalk` 是**开发机/CI 侧**工具，
  不进仓库依赖。
- **handler 初始化点**：客户端 `main()` 最早段——QApplication 构造后、
  业务引擎/窗口构造**之前**。启动早期崩溃也能落 dump；Crashpad 通过信号
  接管崩溃路径，不干扰正常退出（无 atexit 挂钩）。
- **dump 落盘目录**：`AppDataLocation/crashes`（与文件下载目录同根，
  Linux 即 `~/.local/share/memex/Memex/crashes`）；目录不存在由初始化
  创建。dump 数据库采用 Crashpad 的 CrashReportDatabase（目录布局由其
  管理：`new/` `pending/` `completed/`）。
- **独立 handler 进程打包**：`crashpad_handler` 作为**独立可执行文件**
  随客户端包分发（构建产物与客户端同目录；打包脚本一并收集）。目标进程
  只在启动时把 handler 路径交给 `CrashpadClient::StartHandler`，此后
  handler 常驻监听。
- **符号表管理**：
  - 构建：客户端目标带 `-g`（RelWithDebInfo 基线）；发布产物用
    `objcopy --only-keep-debug` 分离符号（`*.debug` 文件）＋
    `--strip-debug --add-gnu-debuglink`（发布二进制瘦身后仍可溯源）。
    分离出的符号按「二进制名＋build-id」归档（`artifacts/symbols/`）。
  - 还原：`dump_syms`（读 `.debug`/带符号二进制）产出 Breakpad 格式符号
    文件，`minidump_stackwalk <dump> <符号目录>` 还原调用栈。符号化在
    **开发机/CI 侧**进行，客户端现场不做任何符号化。
- **上传留位**：dump **只本地落盘**，不外发——外发属数据外发，默认关；
  代码留上报接口位（`UploadUrl` 为空＝仅落盘）。未来的外发策略（何时发、
  发什么、含不含路径/账号）另批拍板，拍板前保持关闭。
- **故意崩溃验收开关**：环境变量 `MEMEX_CRASH_TEST=1` 显式开启——启动
  数秒后解引用空指针（`*(volatile int*)0 = 0;`），仅验收用；文档此处
  是唯一入口，不进任何菜单。默认（未设变量）零影响。
- **与既有日志/监控打通**：初始化成败写既有日志通道；dump 落盘后
  （handler 完成报告时回调）状态行/日志记录 dump 路径，与运行日志可
  对时（同一时间线）。

## 实现（2026-10-08 落地实录）

- **依赖与构建**：`vcpkg.json` client feature 增 `crashpad`；`client/CMakeLists.txt`
  `find_package(crashpad CONFIG REQUIRED)`＋链接 `crashpad::crashpad` 界面库；
  构建后 `copy_if_different` 把 `crashpad_handler` 拷到 `memex_client`
  同目录（找不到仅 WARNING，运行时 PATH 兜底）。`third_party/组件清单.md` 已登记。
- **初始化**：`client/app/crash_report.{hpp,cpp}`——`init_crash_reporting()`
  在 `main()` 里 QApplication 名称设置后调用。handler 探寻顺序＝可执行同
  目录→PATH；`CrashReportDatabase::Initialize` 建 crashes 目录；
  `StartHandler(..., url="" /*只落盘不外发*/, annotations={product, version},
  restartable=true, asynchronous_start=false)`。每步失败 qWarning 后返回
  false（客户端照常运行，不因采集组件缺失拒绝启动）。
- **符号面**：`memex_client` 目标恒 `-g`（GNU/Clang），dump_syms 可直接
  从产物抽符号；发布分离符号（objcopy）另批。
- **日志打通**：启动成功 `qInfo`（`[崩溃采集] Crashpad 已启动：handler=…
  dump=…（只本地落盘不外发）`）；启动时扫描 new/pending/completed 三区
  既有 `*.dmp` 计数同打一条，与运行日志同时间线对时。验收开关触发行
  `qWarning` 明示。
- **测试**：`client/tests/test_crash.cpp`（ctest 腿 `crash`）——QTemporaryDir
  隔离 XDG 起 `MEMEX_CRASH_TEST=1` 真客户端进程，断言非零退出（CrashExit）
  ＋`*.dmp` 落盘非空；dump 由独立 handler 进程写盘，探测按文件出现轮询
  （40s 上界），不依赖客户端进程存活。
- **验收实录（2026-10-08，本机 Linux）**：
  1. `MEMEX_CRASH_TEST=1 ./memex_client`（隔离 XDG_DATA_HOME）→ 2s 后
     SIGSEGV（exit 139），日志两行：`[崩溃采集] Crashpad 已启动：…（只
     本地落盘不外发）`＋`[崩溃采集] MEMEX_CRASH_TEST=1 触发故意崩溃（空
     指针写入）`；
  2. dump 落盘 `…/memex/Memex/crashes/pending/<uuid>.dmp`（＋`.meta`），
     约 32K；
  3. 符号化（开发机侧，rust 工具链）：`dump_syms memex_client` 产出
     Breakpad 符号，按 `symbols/<模块>/<build-id>/memex_client.sym` 目录树
     摆放，`minidump-stackwalk <dump> symbols/` 还原——栈顶
     `memex_client!QtPrivate::QCallableObject<main::{lambda()#1}>::impl
     [qobjectdefs_impl.h:548]`（即触发 lambda），`main.cpp:71` 可见；
     crash 原因 `SIGSEGV / SEGV_MAPERR @ 0x0`（空指针写入）；
  4. ctest 全量 61/61 绿（新增 crash 腿约 3.3s）。

