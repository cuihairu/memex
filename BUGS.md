# BUGS 清单

> 登记原则：**只登记不修**（2026-10-04 用户令「先记进 todo 吧」）。状态：`[ ]` 待修 · `[~]` 修复中 · `[x]` 已修并验收。
> 修复时按条目逐项复现→定位→修→回归，并在条目下补「修复记录」留痕；不得整条改写原话口径。

## 2026-10-04 用户反馈批次（BUG×3）

- [ ] BUG-001 点击「截图」没反应
  现象（原话）：点击「截图」没反应。
  疑似方向：入口按钮没接线，或截图面板没弹出（调用链断在按钮 on_clicked → 截图窗口 show 之间）。
  排查面：聊天窗/工具栏截图入口 → T4.4 截图与标注窗口激活路径；确认是按钮无槽可发、还是窗口创建了未 raise。
  平台：待复现确认（桌面端优先查，手机端另记）。
- [ ] BUG-002 点击「发文件」没反应
  现象（原话）：点击「发文件」没反应。
  疑似方向：入口按钮没接线，或文件选择面板没弹出（QFileDialog 未触发／被父窗口遮蔽）。
  排查面：聊天窗发文件入口 → T1.3 直连文件传输、协作态文件发送路径。
  平台：待复现确认。
- [ ] BUG-003 点击「表情」没反应
  现象（原话）：点击「表情」没反应。
  疑似方向：入口按钮没接线，或表情面板没弹出（面板控件未创建／定位到屏外）。
  排查面：聊天窗表情入口 → T4.5 内置表情与表情包面板弹出逻辑。
  平台：待复现确认。

（三处均为「点击无响应」，疑似入口按钮没接线或面板没弹出，逐条独立定位、独立验收，修一条关一条。）

- [ ] BUG-005 Android release 变体单测偶发红（debug 变体恒绿）
  现象：`./gradlew test`（debug＋release 两变体全跑）release 腿 2–3 处红，debug 腿 57/57 恒绿。
  连跑两次实录（2026-10-04）：第一次 3 红——ChatSessionTest.个人通知落库归档态回ACK并回调分级（NoSuchElementException，ChatSessionTest.kt:567，两跑皆红）、ChatSessionTest.收到TEXT回ACK…重复补投去重（NPE :308，第二次复跑通过=时序敏感）、MemexClientTest.域名无法解析→连不上（期望 Unreachable 实得 NotMemex「对端在应答前关闭连接」，两跑皆红）；第二次 2 红（去重那条过了）。
  疑似方向：①测试的假服务端/ACK 轮询在 release 变体（无 debug 断言、JIT 时序不同）下有界等待不够宽；②「域名无法解析」用例依赖环境 DNS 行为（本沙箱对不可解析域名的连接失败路径与 CI/真机不同）。
  排查面：ChatSessionTest 轮询上界与快照读、MemexClientTest 域名用例对环境 DNS 的假设、MemexClient 错误分类（Unreachable vs NotMemex 判定在「连接即被关闭」时的归档）。
  平台：Android release 变体（本机 JVM；安卓无 CI 腿——`.github/workflows/` 无 android.yml，此腿只在开发机跑，CI 视野外）。
  备注：登记于 2026-10-04 文档对账批（T6.3 三块落地后首跑 `./gradlew test` 全变体时发现；此前验收笔「单测 57/57 绿」指 debug 变体）。按「只登记不修」原则挂起，修复时先定性（真时序缺陷 vs 用例环境假设）再动。

- [x] BUG-004 托盘图标（右下角）双击不弹主界面
  现象（原话）：托盘图标（右下角）双击不弹主界面——现在必须右键菜单选「主界面」才显示，与主流软件交互不一致，影响体验。
  定位（2026-10-04）：`client/app/main_window.cpp` setup_tray() 的 `QSystemTrayIcon::activated` 槽只接 `Trigger`（单击）且仅 `!isVisible()` 时才 show——双击（`DoubleClick`）完全不处理，最小化态单击也不管。
  修法口径（用户令）：① 双击托盘图标＝显示/激活主界面（隐藏时弹出、最小化时还原＋置顶聚焦）；② 单击行为对齐同类软件（Qt 惯例默认激活主界面）；③ 右键菜单保留原样；三平台行为一致（Windows/macOS/Linux）。
  平台：三平台（Qt 抽象层同一代码路径，无平台分支）。
  修复记录（2026-10-04）：
  - 接线重构：新 `MainWindow::activate_from_tray()`（去最小化＋保留最大化态＋show/raise/置顶聚焦）与 `wire_tray_activation(tray)`（Trigger/DoubleClick 激活、Context 留菜单、中键/未知不响应），菜单「显示主窗口」同走此路径——三平台同一代码路径。
  - 排查中发现并一并修复的真机坑：closeEvent 关窗即弹 5 秒「已最小化到托盘」气泡（tray_notify→showMessage），气泡盖在图标上方，隐藏后第一击常落在气泡上被吃掉（Qt 气泡标准行为）——表现为「隐藏后第一次单击不弹、第二次才弹」。修法：`wire_tray_activation` 同时接 `QSystemTrayIcon::messageClicked`→激活主界面，气泡被点也弹窗（微信同类惯用法）；unit 测试⑦覆盖。
  - 单测：`client/tests/test_tray.cpp` 7 块（双击隐藏弹出/单击激活幂等/Context·中键·未知不弹/最小化还原/最大化保留/直调同路径/气泡点击激活），offscreen 裸 QSystemTrayIcon 发真实信号驱动生产接线，ctest `tray` 绿。
  - 真机走查：`scripts/bug-004-tray-walkthrough/run.sh`（裸 Xvfb＋假托盘宿主＋xdotool 真点击，无 DE 可复跑），5 门禁全过、5 截图入档 `docs/src/public/screenshots/bug-004-tray-{1..5}.png`：①启动 ②隐藏进托盘（气泡在屏）③双击还原 ④单击还原 ⑤右键菜单（显示主窗口/开机启动/通知偏好/退出 照旧）。补充实证：隐藏后第一击无论落气泡（messageClicked）还是落图标（Trigger）都弹窗，两轮连测均通过。
  - 环境备注（如实）：走查环境曾有用户 fontconfig 缓存与随链 Qt 错位导致 xcb 首布局崩溃，已清理 `~/.cache/fontconfig/*.cache-*`（可再生缓存，非项目资产）；走查脚本 ⑤ 须在无点击历史态抓图（裸 X 合成输入下点过左键后右键 press 不再送达，合成环境现象、真桌面无此问题），run.sh 内有注释与顺序说明。
