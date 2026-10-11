// 主窗列表态窗口宽度约束冒烟（2026-10-11 用户实录：列表态拖窗口右缘拉宽，
// 对话面板已收起，列表与右框之间撑出死空间、视觉与右框分离——拉宽只应
// 发生在会话态右窗可见时）。离屏真 MainWindow 四腿：构造＝列表态→右缘
// 上限收紧到左侧列内容宽（rail 60＋会话列 260＝320，拖右缘不再出空档）；
// 点列表项开会话→上限释放＋窗宽恢复会话态；再点同一行（＝当前会话）回
// 列表态→上限复归＋窗宽收回；托盘隐藏态 quit_app→exec 立即返回（退出
// 即显式 quit，不赖 lastWindowClosed——修前隐藏态 close 进程滞留）。
// 最大化态不设上限（受屏幕几何支配），不在本测内。
#include <QApplication>
#include <QListWidget>
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
  // QSettings 隔离（org/app 独立命名）：主窗构造会触 ThemeManager 等读
  // 配置——不污染开发机真配置面；BrandKit 缓存同口径（env 须在首次
  // ensure_cache_dir 前设置）
  QCoreApplication::setOrganizationName(QStringLiteral("memex-test"));
  QCoreApplication::setApplicationName(QStringLiteral("win-layout-test"));
  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  qputenv("MEMEX_TEST_BRAND_DIR",
          tmp.filePath(QStringLiteral("brand-cache")).toUtf8());

  memex::client::MainWindow w;

  // ① 构造＝列表态（布局令：启动无会话＝列表态长方形面板）：右缘上限＝
  //    左侧列内容宽，拖右缘不再撑出列表与右框之间的死空间
  CHECK(w.maximumWidth() == 60 + 260); // rail_nav kRailW + 会话列固定宽
  CHECK(w.width() <= w.maximumWidth());

  // ② 点列表项开会话（itemClicked 程序化发同款，见 test_report_dialog）：
  //    上限释放（会话态自由拉宽，右窗由 chat 面吸收）＋窄窗恢复会话态宽
  auto* list = w.findChild<QListWidget*>(QStringLiteral("device_list"));
  CHECK(list);
  auto* row = new QListWidgetItem(QStringLiteral("测试设备"), list);
  row->setData(Qt::UserRole, QStringLiteral("dev-lp"));
  row->setData(Qt::UserRole + 1, QStringLiteral("direct"));
  emit list->itemClicked(row);
  CHECK(w.maximumWidth() == QWIDGETSIZE_MAX);
  CHECK(w.width() == 960);

  // ③ 再点同一行＝回列表态（再点当前会话＝收起）：上限复归＋窗宽收回
  emit list->itemClicked(row);
  CHECK(w.maximumWidth() == 60 + 260);
  CHECK(w.width() == 320);

  // ④ 托盘退出语义（2026-10-11 用户实录：最小化到托盘的隐藏态点退出，
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
