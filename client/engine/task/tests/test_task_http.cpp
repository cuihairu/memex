// R27-3 QtNetwork 传输实现验收（本地真 TCP 桩，不打外网）：GET/POST/PUT/
// PATCH 四动词分派（PATCH 走 sendCustomRequest 腿）、自定义头透传、请求体
// 到达、200 回调三元组（status/body/err）、网络错腿（拒连 → status=0 带
// error 文案）。真 QNetworkAccessManager × QEventLoop，回调在循环内收口。
#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <iostream>
#include <map>
#include <memory>

#include <engine/task/task_http.hpp>

using memex::client::HttpFn;
using memex::client::QtNetworkTaskHttp;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " #cond    \
                << "\n";                                                  \
      ++g_failures;                                                       \
    }                                                                     \
  } while (false)

// 桩收到的最近一拍请求（方法/路径/头/体）
struct Captured {
  QString method;
  QString path;
  std::map<std::string, std::string> headers;
  QByteArray body;
  bool complete = false;
};

Captured g_last;

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);

  QTcpServer stub;
  CHECK(stub.listen(QHostAddress::LocalHost));
  const quint16 port = stub.serverPort();

  QObject::connect(&stub, &QTcpServer::newConnection, [&stub] {
    QTcpSocket* sock = stub.nextPendingConnection();
    // 每连接独立状态（读缓冲＋收口旗），不得跨连接残留
    auto buf = std::make_shared<QByteArray>();
    auto handled = std::make_shared<bool>(false);
    QObject::connect(sock, &QTcpSocket::readyRead, [sock, buf, handled] {
      *buf += sock->readAll();
      if (*handled) return;
      const auto head_end = buf->indexOf("\r\n\r\n");
      if (head_end < 0) return;
      const QByteArray head = buf->left(head_end);
      const QList<QByteArray> lines = head.split('\r');
      if (lines.isEmpty()) return;
      const QList<QByteArray> rl = lines[0].split(' ');
      if (rl.size() < 2) return;
      // Content-Length 门：体到齐才收口
      qint64 want_body = 0;
      for (int i = 1; i < lines.size(); ++i) {
        const QByteArray ln = lines[i].startsWith("\n") ? lines[i].mid(1)
                                                       : lines[i];
        const auto colon = ln.indexOf(':');
        if (colon < 0) continue;
        const QByteArray k = ln.left(colon).trimmed().toLower();
        if (k == "content-length")
          want_body = ln.mid(colon + 1).trimmed().toLongLong();
        else
          g_last.headers[k.toStdString()] =
              ln.mid(colon + 1).trimmed().toStdString();
      }
      const QByteArray body = buf->mid(head_end + 4);
      if (body.size() < want_body) return;
      g_last.method = QString::fromLatin1(rl[0]);
      g_last.path = QString::fromLatin1(rl[1]);
      g_last.body = body.left(want_body);
      g_last.complete = true;
      sock->write("HTTP/1.1 200 OK\r\nContent-Length: 7\r\n"
                  "Connection: close\r\n\r\nstub-ok");
      sock->disconnectFromHost();
      *handled = true;
    });
  });

  // 拒连腿用：起过再关，拿到一个当前无监听的端口
  QTcpServer dead;
  dead.listen(QHostAddress::LocalHost);
  const quint16 dead_port = dead.serverPort();
  dead.close();

  QNetworkAccessManager nam;
  QtNetworkTaskHttp http(&nam);

  auto roundtrip = [&](const QString& method, const QUrl& url,
                       const QList<QPair<QByteArray, QByteArray>>& headers,
                       const QByteArray& body, int* status,
                       QByteArray* out_body, QString* err) {
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    http.request(method, url, headers, body,
                 [&](int s, const QByteArray& b, const QString& e) {
                   *status = s;
                   *out_body = b;
                   *err = e;
                   loop.quit();
                 });
    timer.start(5000);
    loop.exec();
    CHECK(timer.isActive()); // 未超时＝回调按期到达
  };

  // —— GET：200 回调三元组 ——
  {
    g_last = Captured{};
    int status = -1;
    QByteArray body;
    QString err;
    roundtrip("GET", QUrl(QString("http://127.0.0.1:%1/ok").arg(port)), {},
              {}, &status, &body, &err);
    CHECK(status == 200);
    CHECK(body == "stub-ok");
    CHECK(err.isEmpty());
    CHECK(g_last.complete);
    CHECK(g_last.method == "GET");
    CHECK(g_last.path == "/ok");
  }

  // —— POST/PUT/PATCH：动词分派与体到达 ——
  {
    g_last = Captured{};
    int status = -1;
    QByteArray body;
    QString err;
    roundtrip("POST", QUrl(QString("http://127.0.0.1:%1/echo").arg(port)),
              {}, "post-body", &status, &body, &err);
    CHECK(status == 200);
    CHECK(err.isEmpty());
    CHECK(g_last.method == "POST");
    CHECK(g_last.body == "post-body");
  }
  {
    g_last = Captured{};
    int status = -1;
    QByteArray body;
    QString err;
    roundtrip("PUT", QUrl(QString("http://127.0.0.1:%1/echo").arg(port)),
              {}, "put-body", &status, &body, &err);
    CHECK(status == 200);
    CHECK(g_last.method == "PUT");
    CHECK(g_last.body == "put-body");
  }
  {
    g_last = Captured{};
    int status = -1;
    QByteArray body;
    QString err;
    roundtrip("PATCH", QUrl(QString("http://127.0.0.1:%1/echo").arg(port)),
              {}, "patch-body", &status, &body, &err);
    CHECK(status == 200);
    CHECK(g_last.method == "PATCH"); // sendCustomRequest 腿
    CHECK(g_last.body == "patch-body");
  }

  // —— 自定义头透传 ——
  {
    g_last = Captured{};
    int status = -1;
    QByteArray body;
    QString err;
    roundtrip("GET", QUrl(QString("http://127.0.0.1:%1/hdr").arg(port)),
              {{"X-Memex-Test", "hello"}, {"Authorization", "Bearer t"}},
              {}, &status, &body, &err);
    CHECK(status == 200);
    CHECK(g_last.headers.count("x-memex-test") == 1);
    CHECK(g_last.headers["x-memex-test"] == "hello");
    CHECK(g_last.headers["authorization"] == "Bearer t");
  }

  // —— 网络错腿：拒连 → status=0 带 error 文案 ——
  {
    int status = -1;
    QByteArray body;
    QString err;
    roundtrip("GET", QUrl(QString("http://127.0.0.1:%1/x").arg(dead_port)),
              {}, {}, &status, &body, &err);
    CHECK(status == 0);
    CHECK(!err.isEmpty());
  }

  if (g_failures == 0) std::cout << "task_http: all ok\n";
  else std::cout << "task_http: " << g_failures << " failures\n";
  return g_failures == 0 ? 0 : 1;
}
