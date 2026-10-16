> **English** | [中文](/guide/crash-reporting)

# Crash Reporting (Crashpad)

> Crash-reporting batch (2026-10-08 user order): the choice is pinned to **Google Crashpad**
> (direct C++ integration; Breakpad is kept only as a historical comparison, not introduced).
> This page first records the brief (design); implementation follows with the batch;
> acceptance = deliberate crash → dump on disk + symbolization recovering the call stack.

## Choice

- **Crashpad**: the Chrome family's next-generation crash reporting — the crash report is
  completed by a **standalone handler process** (at the instant of the target process's crash
  only minimal signal handling survives; write-to-disk/compression/upload all happen in the
  handler, so it can finish even when the target's stack is already wrecked); supports
  attaching metadata and attachments alongside the dump; the upstream is active.
- **Breakpad (historical comparison, not introduced)**: Crashpad's predecessor — report
  collection completes inside the target process (an in-process reporter is forked), which is
  weaker at the finish than the standalone-process design; the upstream is frozen
  (maintenance only), and the official advice for new projects is to use Crashpad directly.
- Both are compatible at the minidump format level: the `minidump_stackwalk` toolchain works
  for either.

## Brief (design scope)

- **Integration**: vcpkg manifest dependency `crashpad` (Apache-2.0; the baseline pinned by
  the repo ships the port, producing the four static libraries client/common/util/base plus
  a standalone `crashpad_handler` executable and a `crashpadConfig.cmake` export surface).
  The repo discipline 「组件均由 vcpkg 按 manifest 获取与构建，不 vendor 源码」 takes
  precedence over the literal submodule/fetchcontent path — the two pursue the same goal
  (pull upstream sources and build); the vcpkg route avoids maintaining a third-party source
  tree and shares the pipeline of the existing components (Qt/OpenSSL/asio…). The
  symbolization tools `dump_syms` / `minidump_stackwalk` are **developer-machine/CI-side**
  tools and do not enter the repo's dependency set.
- **Handler initialization point**: the earliest stretch of the client's `main()` — after
  QApplication is constructed and **before** the business engines/windows are constructed.
  A crash early in startup still produces a dump; Crashpad takes over the crash path via
  signals and does not interfere with normal exit (no atexit hook).
- **Dump directory**: `AppDataLocation/crashes` (same root as the file download directory;
  on Linux that is `~/.local/share/memex/Memex/crashes`); initialization creates the
  directory if it does not exist. The dump database uses Crashpad's CrashReportDatabase (the
  directory layout is managed by it: `new/` `pending/` `completed/`).
- **Standalone handler process packaging**: `crashpad_handler` ships as a **standalone
  executable** alongside the client package (its build artifact sits in the client's
  directory; the packaging script collects it as well). At startup the target process only
  hands the handler path to `CrashpadClient::StartHandler`, after which the handler stays
  resident and listens.
- **Symbol table management**:
  - Build: the client target carries `-g` (RelWithDebInfo baseline); release artifacts
    separate symbols with `objcopy --only-keep-debug` (the `*.debug` files) +
    `--strip-debug --add-gnu-debuglink` (the slimmed release binary remains traceable).
    The separated symbols are archived under 「二进制名＋build-id」 (`artifacts/symbols/`).
  - Restore: `dump_syms` (reads `.debug`/symbol-bearing binaries) produces Breakpad-format
    symbol files, and `minidump_stackwalk <dump> <符号目录>` restores the call stack.
    Symbolization happens on the **developer-machine/CI side**; the client site does no
    symbolization at all.
- **Upload slot**: dumps are **local-disk only**, never sent out — sending out is data
  egress and defaults to off; the code reserves the reporting interface slot (an empty
  `UploadUrl` = local disk only). The future egress policy (when to send, what to send,
  whether paths/accounts are included) is decided in a separate batch, and stays off until
  that decision.
- **Deliberate-crash acceptance switch**: environment variable `MEMEX_CRASH_TEST=1` turns
  it on explicitly — a few seconds after startup it dereferences a null pointer
  (`*(volatile int*)0 = 0;`), for acceptance only; this spot in the docs is the single
  entry point and it is in no menu. Default (variable unset) has zero impact.
- **Wiring into the existing logs/monitoring**: initialization success or failure goes to
  the existing log channel; after a dump lands (callback when the handler completes the
  report) the status line/log records the dump path, which can be time-correlated with the
  run log (one timeline).

## Implementation (landed 2026-10-08)

- **Dependencies and build**: the `crashpad` dependency was added to the client feature in
  `vcpkg.json`; `client/CMakeLists.txt` got `find_package(crashpad CONFIG REQUIRED)` plus a
  link against the `crashpad::crashpad` interface library; after the build,
  `copy_if_different` copies `crashpad_handler` next to `memex_client` (a miss is only a
  WARNING, with PATH as the runtime fallback). Registered in `third_party/组件清单.md`.
- **Initialization**: `client/app/crash_report.{hpp,cpp}` — `init_crash_reporting()` is
  called in `main()` after the QApplication name is set. Handler search order = same
  directory as the executable → PATH; `CrashReportDatabase::Initialize` creates the crashes
  directory; `StartHandler(..., url="" /*only local disk, no egress*/, annotations={product, version},
  restartable=true, asynchronous_start=false)`. Each failing step qWarning's and returns
  false (the client runs as usual, and does not refuse to start just because the collection
  component is missing).
- **Symbol surface**: the `memex_client` target always carries `-g` (GNU/Clang), so dump_syms
  can pull symbols straight from the artifact; release symbol separation (objcopy) is a
  separate batch.
- **Log wiring**: a successful startup logs `qInfo`
  (`[崩溃采集] Crashpad 已启动：handler=… dump=…（只本地落盘不外发）`); at startup the
  existing `*.dmp` files across the new/pending/completed three areas are counted and logged
  in the same line, time-correlated with the run log on one timeline. The acceptance-switch
  trigger line is announced by `qWarning`.
- **Tests**: `client/tests/test_crash.cpp` (ctest leg `crash`) — a QTemporaryDir-isolated
  XDG launches the real client process with `MEMEX_CRASH_TEST=1`, asserting a non-zero exit
  (CrashExit) + a non-empty `*.dmp` on disk; the dump is written by the standalone handler
  process, so detection polls for the file's appearance (40s upper bound) and does not
  depend on the client process staying alive.
- **Acceptance record (2026-10-08, local Linux)**:
  1. `MEMEX_CRASH_TEST=1 ./memex_client` (isolated XDG_DATA_HOME) → SIGSEGV after 2s
     (exit 139), two log lines: `[崩溃采集] Crashpad 已启动：…（只本地落盘不外发）` +
     `[崩溃采集] MEMEX_CRASH_TEST=1 触发故意崩溃（空指针写入）`；
  2. the dump lands at `…/memex/Memex/crashes/pending/<uuid>.dmp` (+ `.meta`), about 32K;
  3. symbolization (developer-machine side, rust toolchain): `dump_syms memex_client`
     produced Breakpad symbols, laid out in the
     `symbols/<模块>/<build-id>/memex_client.sym` directory tree, and
     `minidump-stackwalk <dump> symbols/` restored — the stack top is
     `memex_client!QtPrivate::QCallableObject<main::{lambda()#1}>::impl
     [qobjectdefs_impl.h:548]` (that is the triggering lambda), `main.cpp:71` visible;
     crash reason `SIGSEGV / SEGV_MAPERR @ 0x0` (null-pointer write);
  4. full ctest 61/61 green (the new crash leg about 3.3s).
     Note: 61/61 was the baseline as of 2026-10-08; the baseline has since grown to 83
     registered tests (ctest names deduplicated, see the
     [Delivery Acceptance Matrix](/en/guide/delivery-matrix)).
