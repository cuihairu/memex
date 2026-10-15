// 主窗窗口尺寸随布局态绑定冒烟（布局令＋Telegram 机制：无对话必坍缩到
// 列表态尺寸；最小尺寸与几何存档都按布局态分档——会话态宽窗不盖回无会话
// 态，2026-10-15 缺陷修复）。离屏真 MainWindow 九腿：①构造＝启动列表态
// （钳 320 宽，非默认横窗）；②点列表项开会话→上限释放＋会话态宽 1280；
// ③再点同一行回列表态→上限复归＋窗宽收回 320；④再开会话→会话态宽；
// ⑤按态存档往返（列表态存档恢复列表态尺寸含高度）；⑥会话态存档隔离
// （存了 760×600 会话档，新窗启动仍 320×列表档高——回归腿）＋开会话恢复
// 会话档；⑦旧单键存档迁移归会话态（启动不受影响）；⑧列表列占满右缘无
// 空白；⑨托盘隐藏态 quit_app→exec 立即返回。最大化态不设上限（受屏幕
// 几何支配），不在本测内。
#include <QApplication>
#include <QListWidget>
#include <QMainWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>
#include <unistd.h>

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

QSettings& test_settings() {
  static QSettings s(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  return s;
}

// 在列表里造一行并程序化点击（与 test_report_dialog 同款发 itemClicked）
void click_row(QListWidget* list, const char* name) {
  auto* row = new QListWidgetItem(QString::fromUtf8(name), list);
  row->setData(Qt::UserRole, QStringLiteral("dev-lp"));
  row->setData(Qt::UserRole + 1, QStringLiteral("direct"));
  emit list->itemClicked(row);
}

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  // QSettings 隔离（org/app 独立命名带 pid＋Ini/Native 双格式路径重定向到
  // 临时目录）：主窗内部 QSettings 走默认构造/双参构造＝NativeFormat——
  // 只重定向 IniFormat 管不住它们（上轮实录：存档漏进 ~/.config 跨跑串味，
  // 默认几何断言假红）；双格式都重定向＋跑前 clear＝彻底净室。BrandKit
  // 缓存同口径（env 须在首次 ensure_cache_dir 前设置）
  QCoreApplication::setOrganizationName(QStringLiteral("memex-test"));
  QCoreApplication::setApplicationName(
      QStringLiteral("win-layout-test-%1").arg(static_cast<int>(::getpid())));
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  const QByteArray settings_dir =
      tmp.filePath(QStringLiteral("settings")).toUtf8();
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir);
  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                     settings_dir);
  qputenv("MEMEX_TEST_BRAND_DIR",
          tmp.filePath(QStringLiteral("brand-cache")).toUtf8());
  test_settings().clear();

  memex::client::MainWindow w;

  // ① 构造＝启动列表态（布局令：无对话必坍缩到列表态尺寸——宽 320＝
  //    rail 60＋列表列 260，右缘上限同步收紧；高度沿用默认 800）
  CHECK(w.maximumWidth() == 60 + 260);
  CHECK(w.width() == 320);
  CHECK(w.height() == 800);

  // ② 点列表项开会话：上限释放（会话态自由拉宽，右窗由 chat 面吸收）；
  //    窄窗（320<520）展开到会话态宽（无存档＝默认 1280×当前高）
  auto* list = w.findChild<QListWidget*>(QStringLiteral("device_list"));
  CHECK(list);
  click_row(list, "测试设备");
  CHECK(w.maximumWidth() == QWIDGETSIZE_MAX);
  CHECK(w.width() == 1280);
  CHECK(w.height() == 800);

  // ③ 再点同一行＝回列表态（再点当前会话＝收起）：上限复归＋窗宽收回 320
  emit list->itemClicked(list->item(0));
  CHECK(w.maximumWidth() == 60 + 260);
  CHECK(w.width() == 320);
  CHECK(w.height() == 800);

  // ④ 关会话再开会话：窄窗（320）恢复会话态宽（＝默认宽 1280）
  click_row(list, "测试设备");
  CHECK(w.width() == 1280);

  // ⑤ 按态存档往返：列表态存档恢复列表态尺寸（含高度）——列表态收窄到
  //    320×640 存档，新窗启动即 320×640（非默认 800 高）
  emit list->itemClicked(list->item(0)); // 回列表态
  w.resize(320, 640);
  w.save_window_geometry();
  {
    memex::client::MainWindow w2;
    CHECK(w2.maximumWidth() == 60 + 260);
    CHECK(w2.width() == 320);
    CHECK(w2.height() == 640);

    // ⑥ 会话态存档隔离（本缺陷回归腿）：会话态存 760×600 档后，新窗启动
    //    仍按列表态档＝320×640（会话宽不盖回无会话态）；开会话才恢复
    //    760×600。注：离屏测试屏 800×800，restoreGeometry 会按目标屏钳宽，
    //    存档尺寸须落屏内
    auto* list2 = w2.findChild<QListWidget*>(QStringLiteral("device_list"));
    CHECK(list2);
    click_row(list2, "测试设备"); // 展开（无会话档＝1280×640）
    w2.resize(760, 600);
    w2.save_window_geometry(); // 会话态→chat 键
    memex::client::MainWindow w3;
    CHECK(w3.width() == 320);  // 启动列表态：不吃会话档
    CHECK(w3.height() == 640); // 列表态档的高度
    auto* list3 = w3.findChild<QListWidget*>(QStringLiteral("device_list"));
    CHECK(list3);
    click_row(list3, "测试设备");
    CHECK(w3.width() == 760);  // 开会话才恢复会话态档
    CHECK(w3.height() == 600);

    // ⑦ 旧单键存档迁移归会话态：旧版（单键时代）只存过 ≥480 宽的窗——
    //    宽档必属会话态；启动列表态不受影响（仍 320×800 默认），开会话
    //    才吃到旧档
    test_settings().remove(QStringLiteral("window_geometry_list"));
    test_settings().remove(QStringLiteral("window_geometry_chat"));
    {
      QMainWindow probe;
      probe.resize(700, 560);
      test_settings().setValue(QStringLiteral("window_geometry"),
                               probe.saveGeometry());
      test_settings().sync();
    }
    memex::client::MainWindow w4;
    CHECK(w4.width() == 320);  // 旧档不进启动列表态
    CHECK(w4.height() == 800);
    auto* list4 = w4.findChild<QListWidget*>(QStringLiteral("device_list"));
    CHECK(list4);
    click_row(list4, "测试设备");
    CHECK(w4.width() == 700);  // 开会话恢复旧档
    CHECK(w4.height() == 560);
  }

  // ⑧ 列表态右缘无空白（2026-10-14 用户报）：列表列 Expanding 占满 rail
  //    之外的全部宽度；开会话后收回 260、chat 面吸收。须 show 激活布局
  //    才测得真实宽度（断言用相对占满，独立实例不动前腿）。存档键全清
  //    （含⑦写的旧单键）＝本腿走纯默认几何
  test_settings().remove(QStringLiteral("window_geometry_list"));
  test_settings().remove(QStringLiteral("window_geometry_chat"));
  test_settings().remove(QStringLiteral("window_geometry"));
  {
    memex::client::MainWindow w5;
    w5.show();
    app.processEvents();
    auto* side_col = w5.findChild<QWidget*>(QStringLiteral("side_column"));
    auto* rail = w5.findChild<QWidget*>(QStringLiteral("rail_nav"));
    CHECK(side_col);
    CHECK(rail);
    CHECK(side_col->width() == w5.width() - rail->width()); // 列表占满，无空白
    auto* list5 = w5.findChild<QListWidget*>(QStringLiteral("device_list"));
    CHECK(list5);
    click_row(list5, "测试设备");
    app.processEvents();
    CHECK(side_col->width() == 260); // 会话态：列表列收回固定宽
    w5.hide();
  }

  // ⑨ 托盘退出语义（2026-10-11 用户实录：最小化到托盘的隐藏态点退出，
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
