// T4.4 截图与标注验收（A17）：
// ① 标注画布：涂鸦／箭头／马赛克／文字命令渲染、撤销（像素级）、PNG 保存；
// ② 多屏区域数学：选区（虚拟屏设备像素）与单屏逻辑几何求交——跨屏、
//    负坐标、高 DPI（dpr=2）三种布局；
// ③ 端到端：注入截屏（测试缝）→ 注入标注命令（真实渲染路径）→ 确认 →
//    主窗发送 → 对端引擎收到文件；聊天区出现 [截图] 系统行；临时文件清理。
#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>

#include <functional>

#include <app/main_window.hpp>
#include <app/screenshot_tool.hpp>
#include <engine/direct/direct_engine.hpp>

using memex::client::AnnotationCanvas;
using memex::client::AnnotationCommand;
using memex::client::DirectEngine;
using memex::client::MainWindow;
using memex::client::region_to_local;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      qCritical("FAIL %s:%d %s", __FILE__, __LINE__, #cond);                 \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

bool wait_until(const std::function<bool()>& cond, int timeout_ms) {
  QElapsedTimer timer;
  timer.start();
  while (!cond()) {
    if (timer.elapsed() > timeout_ms) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    QThread::msleep(5);
  }
  return true;
}

bool reddish(QRgb c) { return qRed(c) > 150 && qGreen(c) < 100 && qBlue(c) < 100; }
bool greenish(QRgb c) { return qGreen(c) > 120 && qRed(c) < 100 && qBlue(c) < 100; }

// 构造测试图：白底＋噪声块（马赛克用例）＋黑块
QPixmap make_test_image() {
  QPixmap img(800, 600);
  img.fill(Qt::white);
  QPainter p(&img);
  p.fillRect(600, 400, 150, 120, Qt::black);
  QRandomGenerator rng(42);
  for (int y = 50; y < 150; ++y) {
    for (int x = 50; x < 150; ++x) {
      p.setPen(QColor(rng.generate() % 256, rng.generate() % 256,
                      rng.generate() % 256));
      p.drawPoint(x, y);
    }
  }
  p.end();
  return img;
}

int distinct_colors(const QImage& img, const QRect& r) {
  QSet<QRgb> colors;
  for (int y = r.top(); y < r.bottom(); ++y) {
    for (int x = r.left(); x < r.right(); ++x) {
      colors.insert(img.pixel(x, y));
    }
  }
  return colors.size();
}

// —— ① 标注画布 ——

void test_canvas() {
  AnnotationCanvas canvas;
  // 空画布拒绝命令
  AnnotationCommand pen;
  pen.type = AnnotationCommand::Type::kPen;
  pen.points = {QPoint(0, 0), QPoint(10, 10)};
  canvas.addCommand(pen);
  CHECK(canvas.commandCount() == 0);

  canvas.setImage(make_test_image());
  CHECK(canvas.hasImage());
  CHECK(canvas.imageSize() == QSize(800, 600));

  // 涂鸦：红色水平线
  pen.color = QColor(220, 40, 30);
  pen.width = 8;
  pen.points = {QPoint(100, 100), QPoint(400, 100)};
  canvas.addCommand(pen);
  CHECK(canvas.commandCount() == 1);
  QImage rendered = canvas.render().toImage();
  CHECK(reddish(rendered.pixel(250, 100)));
  CHECK(reddish(rendered.pixel(100, 100)));

  // 箭头：蓝色斜线，两端像素着色
  AnnotationCommand arrow;
  arrow.type = AnnotationCommand::Type::kArrow;
  arrow.color = QColor(30, 90, 200);
  arrow.width = 6;
  arrow.points = {QPoint(500, 100), QPoint(600, 200)};
  canvas.addCommand(arrow);
  rendered = canvas.render().toImage();
  CHECK(qBlue(rendered.pixel(500, 100)) > 150);
  CHECK(qBlue(rendered.pixel(600, 200)) > 150);

  // 马赛克：噪声块（50,50,100,100）像素化——颜色数骤降
  const QRect noise_rect(50, 50, 100, 100);
  const int before = distinct_colors(rendered, noise_rect);
  AnnotationCommand mosaic;
  mosaic.type = AnnotationCommand::Type::kMosaic;
  mosaic.rect = noise_rect;
  canvas.addCommand(mosaic);
  rendered = canvas.render().toImage();
  const int after = distinct_colors(rendered, noise_rect);
  CHECK(after <= 200);      // 块内同色：≤ (100/8+1)² ≈ 169
  CHECK(after * 4 < before); // 噪声上万 → 像素化后骤降

  // 文字：绿色「验收」落在 (100,300) 附近
  QImage before_text = rendered;
  AnnotationCommand text;
  text.type = AnnotationCommand::Type::kText;
  text.color = QColor(50, 150, 60);
  text.text = QStringLiteral("验收");
  text.text_pos = QPoint(100, 300);
  canvas.addCommand(text);
  rendered = canvas.render().toImage();
  int green_pixels = 0;
  for (int y = 290; y < 345; ++y) {
    for (int x = 90; x < 180; ++x) {
      if (greenish(rendered.pixel(x, y))) ++green_pixels;
    }
  }
  CHECK(green_pixels > 50);

  // 撤销文字：渲染回到加文字之前（像素级一致）
  CHECK(canvas.undo());
  CHECK(canvas.commandCount() == 3);
  CHECK(canvas.render().toImage() == before_text);
  // 撤销到底后返回 false
  canvas.undo();
  canvas.undo();
  canvas.undo();
  CHECK(canvas.commandCount() == 0);
  CHECK(!canvas.undo());
  CHECK(canvas.commandCount() == 0);

  // 保存 PNG：文件存在且尺寸一致
  canvas.addCommand(pen);
  const QDir dir = QDir::temp();
  const QString path =
      dir.filePath(QStringLiteral("memex-shot-test-%1.png")
                      .arg(QDateTime::currentMSecsSinceEpoch()));
  CHECK(canvas.save(path));
  CHECK(QFile::exists(path));
  QImage loaded(path);
  CHECK(!loaded.isNull());
  CHECK(loaded.size() == QSize(800, 600));
  QFile::remove(path);
}

// —— ② 多屏区域数学 ——

void test_region_math() {
  // 屏 A：逻辑 (-1920,0) 1920×1080 dpr 1（副屏在左，负坐标）
  // 屏 B：逻辑 (0,0) 960×540 dpr 2（高 DPI 主屏，设备像素 1920×1080）
  const QPoint a_top(-1920, 0);
  const QSize a_size(1920, 1080);
  const QPoint b_top(0, 0);
  const QSize b_size(960, 540);

  // 完全在 A 内（负坐标设备像素）
  CHECK(region_to_local(QRect(-1820, 50, 400, 300), a_top, a_size, 1.0) ==
        QRect(100, 50, 400, 300));
  // 完全在 B 内（dpr 2：设备像素 → 逻辑像素减半）
  CHECK(region_to_local(QRect(100, 100, 400, 200), b_top, b_size, 2.0) ==
        QRect(50, 50, 200, 100));
  // 跨屏：A 的右缘 → B 的左缘
  const QRect cross(-50, 100, 200, 200);
  CHECK(region_to_local(cross, a_top, a_size, 1.0) == QRect(1870, 100, 50, 200));
  CHECK(region_to_local(cross, b_top, b_size, 2.0) == QRect(0, 50, 75, 100));
  // 不相交 → 空
  CHECK(region_to_local(QRect(5000, 5000, 100, 100), a_top, a_size, 1.0)
            .isEmpty());
  // 非法 dpr → 空
  CHECK(region_to_local(QRect(0, 0, 10, 10), a_top, a_size, 0).isEmpty());
}

// —— ③ 端到端：注入截屏 → 标注 → 确认 → 发送 → 对端收到 ——

void test_end_to_end() {
  MainWindow window;
  window.show();

  const QString peer_id = QStringLiteral("dev-shot-peer");
  DirectEngine peer("dev-shot-peer", QString());
  QString received_path;
  QObject::connect(&peer, &DirectEngine::file_received, &peer,
                   [&](const QString&, const QString& path) {
                     received_path = path;
                   });
  CHECK(peer.start());
  CHECK(wait_until([&] { return window.has_direct_peer(peer_id); }, 10000));

  window.open_direct_peer(peer_id);
  CHECK(window.chat_html().contains(QStringLiteral("点对点会话")));

  // 注入截屏（测试缝：跳过真实取屏）→ 标注窗出现
  QPixmap fake(1200, 800);
  fake.fill(Qt::white);
  {
    QPainter p(&fake);
    p.fillRect(100, 100, 200, 200, QColor(30, 90, 200));
    p.end();
  }
  window.screenshot_tool()->startWithImage(fake);
  CHECK(wait_until([&] { return window.screenshot_tool()->canvas() != nullptr; },
                   5000));

  // 注入标注命令（真实渲染路径）：品牌橙涂鸦
  AnnotationCommand pen;
  pen.type = AnnotationCommand::Type::kPen;
  pen.color = QColor(QStringLiteral("#e16531"));
  pen.width = 10;
  pen.points = {QPoint(50, 50), QPoint(600, 400)};
  window.screenshot_tool()->canvas()->addCommand(pen);

  // 确认 → 主窗发送（走既有文件通道）
  window.screenshot_tool()->confirmCurrent();

  // 对端收到文件
  CHECK(wait_until([&] { return !received_path.isEmpty(); }, 15000));
  CHECK(QFileInfo(received_path).fileName().startsWith(
      QStringLiteral("memex-shot-")));
  CHECK(QFileInfo(received_path).suffix() == QStringLiteral("png"));
  QImage received(received_path);
  CHECK(!received.isNull());
  CHECK(received.size() == QSize(1200, 800)); // 全分辨率（含标注）
  // 标注确实进了发送的图：品牌橙像素存在
  bool has_orange = false;
  for (int y = 50; y < 400 && !has_orange; ++y) {
    for (int x = 50; x < 600; ++x) {
      if (received.pixelColor(x, y) == QColor(QStringLiteral("#e16531"))) {
        has_orange = true;
        break;
      }
    }
  }
  CHECK(has_orange);

  // 聊天区出现 [截图] 系统行
  CHECK(window.chat_html().contains(QStringLiteral("[截图]")));

  // 传输结束后临时文件清理（成功路径）
  CHECK(wait_until(
      [] {
        const QDir dir(QDir::tempPath() + QStringLiteral("/memex-screenshots"));
        return dir.isEmpty();
      },
      10000));
}

} // namespace

int main(int argc, char** argv) {
  {
    // 独立 UDP 发现口：并发测试进程互不串扰（生产默认口 2425 不受影响）
    QUdpSocket probe;
    if (probe.bind(QHostAddress::AnyIPv4, 0,
                   QAbstractSocket::ShareAddress |
                       QAbstractSocket::ReuseAddressHint)) {
      qputenv("MEMEX_TEST_DISCOVERY_PORT",
              QByteArray::number(probe.localPort()));
      probe.close();
    }
  }
  QTemporaryDir tmp;
  if (!tmp.isValid()) return 1;
  qputenv("XDG_DATA_HOME", tmp.filePath(QStringLiteral("xdg")).toUtf8());
  qputenv("XDG_CONFIG_HOME",
          tmp.filePath(QStringLiteral("xdg-config")).toUtf8());

  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("memex-test"));
  QCoreApplication::setApplicationName(QStringLiteral("screenshot-test"));

  test_canvas();
  test_region_math();
  test_end_to_end();

  if (g_failures == 0) {
    qInfo("test_screenshot: ALL PASS");
    return 0;
  }
  qCritical("test_screenshot: %d FAILURES", g_failures);
  return 1;
}
