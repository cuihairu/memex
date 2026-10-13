// 主窗列表态窗口宽度约束冒烟（2026-10-11 用户实录：列表态拖窗口右缘拉宽，
// 对话面板已收起，列表与右框之间撑出死空间、视觉与右框分离——拉宽只应
// 发生在会话态右窗可见时）。离屏真 MainWindow 五腿：构造＝启动列表态
// （2026-10-13 默认尺寸令：启动保持默认横窗 1280×800，不钳宽）；点列表项
// 开会话→上限释放＋窗保持宽；再点同一行（＝当前会话）回列表态→上限复归
// ＋窗宽收回 320；再开会话→窄窗恢复会话态宽（1280）；托盘隐藏态
// quit_app→exec 立即返回（退出即显式 quit，不赖 lastWindowClosed——修前
// 隐藏态 close 进程滞留）。
// 最大化态不设上限（受屏幕几何支配），不在本测内。
#include <QApplication>
#include <QListWidget>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>

#include <app/main_window.hpp>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      qCritical("FAIL %s:%d %s", __FILE__, __LINE__, #cond);                 \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  // QSettings 隔离（org/app 独立命名＋INI 重定向到临时目录）：主窗构造会触
  // ThemeManager 等读配置——不污染开发机真配置面；窗口几何持久化（本测新增
  // 的 save/restore 腿）不得读上次跑遗留的存档（否则默认几何断言假红），
  // BrandKit 缓存同口径（env 须在首次 ensure_cache_dir 前设置）
  QCoreApplication::setOrganizationName(QStringLiteral("memex-test"));
  QCoreApplication::setApplicationName(QStringLiteral("win-layout-test"));
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     tmp.filePath(QStringLiteral("settings")).toUtf8());
  qputenv("MEMEX_TEST_BRAND_DIR",
          tmp.filePath(QStringLiteral("brand-cache")).toUtf8());

  memex::client::MainWindow w;

  // ① 构造＝启动列表态（2026-10-13 默认尺寸令：启动保持默认横窗 1280×800，
  //    不钳宽——钳宽只在用户关会话时生效；见 main_window.cpp 构造注释）
  CHECK(w.maximumWidth() == QWIDGETSIZE_MAX);
  CHECK(w.width() == 1280);
  CHECK(w.height() == 800);

  // ② 点列表项开会话（itemClicked 程序化发同款，见 test_report_dialog）：
  //    上限释放（会话态自由拉宽，右窗由 chat 面吸收）；窗已宽（1280≥520）
  //    故 open_chat 不再触发恢复会话态宽
  auto* list = w.findChild<QListWidget*>(QStringLiteral("device_list"));
  CHECK(list);
  auto* row = new QListWidgetItem(QStringLiteral("测试设备"), list);
  row->setData(Qt::UserRole, QStringLiteral("dev-lp"));
  row->setData(Qt::UserRole + 1, QStringLiteral("direct"));
  emit list->itemClicked(row);
  CHECK(w.maximumWidth() == QWIDGETSIZE_MAX);
  CHECK(w.width() == 1280);

  // ③ 再点同一行＝回列表态（再点当前会话＝收起）：上限复归＋窗宽收回
  emit list->itemClicked(row);
  CHECK(w.maximumWidth() == 60 + 260);
  CHECK(w.width() == 320);

  // ④ 关会话再开会话：窄窗（320）恢复会话态宽（＝默认宽 1280）
  emit list->itemClicked(row);
  CHECK(w.width() == 1280);

  // ⑤ 窗口几何持久化（2026-10-13 默认尺寸令）：save→新 MainWindow restore
  //    ＝尺寸与位置回来；默认几何（1280×800）只在首次启动（无存档）用。
  //    列表态窄条（320<480）不存档——关会话退出后下次仍回会话态宽。
  //    注：离屏测试屏 800×800，restoreGeometry 会按目标屏钳宽（存 1100 会被
  //    钳到 798）；真机 1080p/2K 屏容得下 1280，故本腿只存屏内尺寸验往返
  {
    w.resize(760, 600);
    w.save_window_geometry();
    memex::client::MainWindow w2;
    CHECK(w2.width() == 760);
    CHECK(w2.height() == 600);
  }
  {
    // 列表态窄条不存档：清存档→窄窗 save（<480 拒存）→新窗回默认 1280×800
    QSettings(QCoreApplication::organizationName(),
              QCoreApplication::applicationName())
        .remove(QStringLiteral("window_geometry"));
    w.resize(320, 640);
    w.save_window_geometry();
    memex::client::MainWindow w3;
    CHECK(w3.width() == 1280);
    CHECK(w3.height() == 800);
  }

  // ⑥ 托盘退出语义（2026-10-11 用户实录：最小化到托盘的隐藏态点退出，
  //    进程不退——close() 不可见窗口不触发 lastWindowClosed，进程滞留
  //    无窗运行态）。quit_app＝显式 quit：隐藏态（show→hide 模拟托盘
  //    常驻）下 exec 内调用，exec 须立即返回 0；看门狗 3s 兜底＝没退出
  //    判负（修前语义正是滞留 here）。
  {
    w.show();
    w.hide(); // 最小化到托盘同态（窗口不可见）
    QTimer watchdog;
    watchdog.setSingleShot(true);
    int quit_deadline_hit = 0;
    QObject::connect(&watchdog, &QTimer::timeout, [&] {
      quit_deadline_hit = 1;
      QCoreApplication::exit(2);
    });
    watchdog.start(3000);
    QTimer::singleShot(0, &w, [&w] { w.quit_app(); });
    const int rc = QApplication::exec();
    CHECK(quit_deadline_hit == 0);
    CHECK(rc == 0);
  }

  if (g_failures == 0) {
    std::cout << "window layout tests: all passed\n";
    return 0;
  }
  std::cerr << "window layout tests: " << g_failures << " failure(s)\n";
  return 1;
}
