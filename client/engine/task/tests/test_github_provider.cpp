// R27-3 GitHub Issues provider 单测（假传输回放，不打外网）：L1 键校验、
// L2 拉列表解析（滤 PR/detailUrl 必带/外部现态）、L3 建单与关单、
// 认证头与 API 形态断言（method/url/Authorization/Bearer）、错误腿
//（401/404/坏 JSON/缺号响应）。
#include <QCoreApplication>
#include <QUrl>

#include <iostream>

#include <engine/task/github_provider.hpp>

using memex::client::ExternalTask;
using memex::client::GitHubIssuesProvider;
using memex::client::HttpFn;
using memex::client::TaskHttp;

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

// 假传输：回放一个预设响应并记下最后一次请求的形态
class FakeHttp : public TaskHttp {
 public:
  QString method, err;
  QUrl url;
  QByteArray body;
  QList<QPair<QByteArray, QByteArray>> headers;
  int next_status = 200;
  QByteArray next_body = "{}";

  void request(const QString& m, const QUrl& u,
               const QList<QPair<QByteArray, QByteArray>>& h,
               const QByteArray& b, const HttpFn& done) override {
    method = m;
    url = u;
    headers = h;
    body = b;
    done(next_status, next_body, err);
  }

  QByteArray header(const char* name) const {
    for (const auto& p : headers) {
      if (p.first == name) return p.second;
    }
    return {};
  }
};

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  FakeHttp http;
  GitHubIssuesProvider prov(QStringLiteral("cuihairu/memex"),
                            QStringLiteral("ghp-token123"), &http);

  // —— L1：键须为数字、repo 缺失拒 ——
  CHECK(prov.detail_url(QStringLiteral("12")) ==
        QStringLiteral("https://github.com/cuihairu/memex/issues/12"));
  CHECK(prov.detail_url(QStringLiteral("abc")).isEmpty());
  CHECK(prov.detail_url(QString()).isEmpty());
  CHECK(prov.detail_url(QStringLiteral("12"), QStringLiteral("o/r")) ==
        QStringLiteral("https://github.com/o/r/issues/12")); // project 覆盖
  CHECK(prov.capabilities() ==
        (memex::client::kCapL1Jump | memex::client::kCapL2Read |
         memex::client::kCapL3Write));
  CHECK(prov.auth_kind() == memex::client::TaskProviderAuth::kToken);

  // —— L2：拉列表解析（含 PR 行滤除＋detailUrl 必带＋现态）——
  http.next_body =
      "[{\"number\":7,\"title\":\"修部署重试\",\"state\":\"open\","
      "\"html_url\":\"https://github.com/cuihairu/memex/issues/7\"},"
      "{\"number\":8,\"title\":\"pr 项\",\"state\":\"open\","
      "\"html_url\":\"https://github.com/cuihairu/memex/pull/8\","
      "\"pull_request\":{\"url\":\"x\"}}]";
  QVector<ExternalTask> items;
  QString list_err = QStringLiteral("未回调");
  prov.list([&](bool ok, const QVector<ExternalTask>& its, const QString& e) {
    list_err = e;
    if (ok) items = its;
  });
  CHECK(list_err.isEmpty());
  CHECK(items.size() == 1); // PR 行已滤
  CHECK(items.first().key == QStringLiteral("7"));
  CHECK(items.first().title == QStringLiteral("修部署重试"));
  CHECK(items.first().detail_url ==
        QStringLiteral("https://github.com/cuihairu/memex/issues/7"));
  CHECK(!items.first().done);
  CHECK(http.method == QStringLiteral("GET"));
  CHECK(http.url.toString().contains(
      QStringLiteral("api.github.com/repos/cuihairu/memex/issues")));
  CHECK(http.url.toString().contains(QStringLiteral("state=open")));
  CHECK(http.header("Authorization") == QByteArray("Bearer ghp-token123"));
  CHECK(http.header("Accept") == QByteArray("application/vnd.github+json"));
  CHECK(!http.header("User-Agent").isEmpty());

  // —— L2 错误腿：401 透传失败 ——
  http.next_status = 401;
  bool l2_ok = true;
  prov.list([&](bool ok, const QVector<ExternalTask>&, const QString& e) {
    l2_ok = ok;
    if (!ok) CHECK(!e.isEmpty());
  });
  CHECK(!l2_ok);
  // 坏 JSON 拒
  http.next_status = 200;
  http.next_body = "not-json";
  l2_ok = true;
  prov.list([&](bool ok, const QVector<ExternalTask>&, const QString&) {
    l2_ok = ok;
  });
  CHECK(!l2_ok);

  // —— L3 建：POST 体与响应解析 ——
  http.next_status = 201;
  http.next_body =
      "{\"number\":42,\"html_url\":"
      "\"https://github.com/cuihairu/memex/issues/42\"}";
  QString new_key, create_err = QStringLiteral("未回调");
  prov.create(QStringLiteral("新缺陷"), QStringLiteral("正文"),
              [&](bool ok, const QString& k, const QString& e) {
                create_err = e;
                if (ok) new_key = k;
              });
  CHECK(create_err.isEmpty());
  CHECK(new_key == QStringLiteral("42"));
  CHECK(http.method == QStringLiteral("POST"));
  CHECK(http.body.contains("\"title\":\"新缺陷\""));
  CHECK(http.body.contains("\"body\":\"正文\""));

  // 建单失败（422）不回键
  http.next_status = 422;
  bool c_ok = true;
  prov.create(QStringLiteral("x"), QString(),
              [&](bool ok, const QString&, const QString& e) {
                c_ok = ok;
                if (!ok) CHECK(!e.isEmpty());
              });
  CHECK(!c_ok);
  // 201 但响应缺号 → 拒（不造假键）
  http.next_status = 201;
  http.next_body = "{}";
  c_ok = true;
  prov.create(QStringLiteral("x"), QString(),
              [&](bool ok, const QString&, const QString&) { c_ok = ok; });
  CHECK(!c_ok);

  // —— L3 关单：PATCH state=closed ——
  http.next_status = 200;
  http.next_body = "{}";
  bool done_ok = false;
  QString done_err = QStringLiteral("未回调");
  prov.complete(QStringLiteral("42"),
                [&](bool ok, const QString& k, const QString& e) {
                  done_ok = ok;
                  done_err = e;
                  CHECK(k == QStringLiteral("42"));
                });
  CHECK(done_ok && done_err.isEmpty());
  CHECK(http.method == QStringLiteral("PATCH"));
  CHECK(http.url.toString().endsWith(
      QStringLiteral("/repos/cuihairu/memex/issues/42")));
  CHECK(http.body.contains("\"state\":\"closed\""));
  // 非数字键本地拒（不打网）
  const QString keep = http.method;
  bool k_ok = true;
  prov.complete(QStringLiteral("abc"),
                [&](bool ok, const QString&, const QString&) { k_ok = ok; });
  CHECK(!k_ok);
  CHECK(http.method == keep); // 未发请求

  if (g_failures == 0) {
    std::cout << "test_github_provider: all checks passed\n";
    return 0;
  }
  std::cout << "test_github_provider: " << g_failures << " check(s) FAILED\n";
  return 1;
}
