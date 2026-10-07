// R27-3 飞书任务 provider 单测（假传输回放，不打外网）：tenant 令牌流
//（缓存＋信封 code!=0 拒＋缺 token 拒＋到期重取）、L3 建任务（Bearer 头/
// summary 体/guid 解析/缺 guid 拒/业务 code!=0 透传）、完成（PATCH 尾段/
// is_completed 体/空 guid 本地拒）。L1/L2 未声明的空 detail_url 断言。
#include <QCoreApplication>
#include <QUrl>

#include <iostream>

#include <engine/task/feishu_provider.hpp>

using memex::client::ExternalTask;
using memex::client::FeishuTaskProvider;
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

class FakeHttp : public TaskHttp {
 public:
  struct Call {
    QString method;
    QUrl url;
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;
  };
  QVector<Call> calls;
  QByteArray token_body =
      "{\"code\":0,\"tenant_access_token\":\"fs-tok\",\"expire\":7200}";
  QByteArray create_body =
      "{\"code\":0,\"data\":{\"task\":{\"guid\":\"g-77\","
      "\"summary\":\"新任务\",\"url\":\"https://feishu.example/task/g-77\"}}}";
  QByteArray done_body = "{\"code\":0}";

  void request(const QString& m, const QUrl& u,
               const QList<QPair<QByteArray, QByteArray>>& h,
               const QByteArray& b, const HttpFn& done) override {
    calls.append({m, u, b, h});
    if (u.path().contains(QStringLiteral("tenant_access_token"))) {
      done(200, token_body, QString());
      return;
    }
    QByteArray resp;
    if (m == QStringLiteral("POST")) resp = create_body;
    if (m == QStringLiteral("PATCH")) resp = done_body;
    done(200, resp, QString());
  }

  QByteArray last_header(const char* name) const {
    for (const auto& p : calls.last().headers) {
      if (p.first == name) return p.second;
    }
    return {};
  }
};

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  FakeHttp http;
  FeishuTaskProvider prov(QStringLiteral("cli_a"), QStringLiteral("sec"),
                          &http);

  // 能力声明：仅 L3；detail_url 恒空（L1 未声明）
  CHECK(prov.capabilities() == memex::client::kCapL3Write);
  CHECK(!prov.can_jump());
  CHECK(!prov.can_read());
  CHECK(prov.detail_url(QStringLiteral("g-1")).isEmpty());
  CHECK(prov.auth_kind() == memex::client::TaskProviderAuth::kToken);

  // —— L3 建：先取令牌再建，Bearer 头与 summary 体 ——
  QString new_key, create_err = QStringLiteral("未回调");
  prov.create(QStringLiteral("新任务"), QStringLiteral("说明"),
              [&](bool ok, const QString& k, const QString& e) {
                create_err = e;
                if (ok) new_key = k;
              });
  CHECK(create_err.isEmpty());
  CHECK(new_key == QStringLiteral("g-77"));
  CHECK(http.calls.size() == 2);
  CHECK(http.calls[0].url.path() ==
        QStringLiteral("/open-apis/auth/v3/tenant_access_token/internal"));
  CHECK(http.calls[0].body.contains("\"app_id\":\"cli_a\""));
  CHECK(http.calls[1].url.path() == QStringLiteral("/open-apis/task/v2/tasks"));
  CHECK(http.calls[1].method == QStringLiteral("POST"));
  CHECK(http.last_header("Authorization") == QByteArray("Bearer fs-tok"));
  CHECK(http.calls[1].body.contains("\"summary\":\"新任务\""));
  CHECK(http.calls[1].body.contains("\"description\":\"说明\""));

  // —— 令牌缓存：再次业务调用不再取令牌 ——
  prov.create(QStringLiteral("二"), QString(),
              [](bool, const QString&, const QString&) {});
  CHECK(http.calls.size() == 3);
  CHECK(http.calls[2].url.path() == QStringLiteral("/open-apis/task/v2/tasks"));

  // —— 到期重取（expire=0 → 缓存即时过期）——
  FakeHttp http2;
  FeishuTaskProvider prov2(QStringLiteral("cli_a"), QStringLiteral("sec"),
                           &http2);
  http2.token_body =
      "{\"code\":0,\"tenant_access_token\":\"t0\",\"expire\":0}";
  prov2.complete(QStringLiteral("g1"),
                 [](bool, const QString&, const QString&) {});
  prov2.complete(QStringLiteral("g2"),
                 [](bool, const QString&, const QString&) {});
  CHECK(http2.calls.size() == 4); // 令牌＋PATCH＋令牌＋PATCH
  CHECK(http2.calls[0].url.path().contains(
      QStringLiteral("tenant_access_token")));
  CHECK(http2.calls[2].url.path().contains(
      QStringLiteral("tenant_access_token")));

  // —— 业务信封 code!=0 透传拒（建与完成两腿）——
  http.create_body = "{\"code\":99991663,\"msg\":\"bad app secret\"}";
  bool c_ok = true;
  QString c_err;
  prov.create(QStringLiteral("x"), QString(),
              [&](bool ok, const QString&, const QString& e) {
                c_ok = ok;
                c_err = e;
              });
  CHECK(!c_ok);
  CHECK(c_err.contains(QStringLiteral("bad app secret")));
  http.create_body = "{\"code\":0,\"data\":{\"task\":{\"guid\":\"g-77\"}}}";
  http.done_body = "{\"code\":230001,\"msg\":\"no perm\"}";
  bool d_ok = true;
  prov.complete(QStringLiteral("g-77"),
                [&](bool ok, const QString&, const QString& e) {
                  d_ok = ok;
                  c_err = e;
                });
  CHECK(!d_ok);
  CHECK(c_err.contains(QStringLiteral("no perm")));
  http.done_body = "{\"code\":0}";

  // —— 响应缺 guid 拒 ——
  http.create_body = "{\"code\":0,\"data\":{\"task\":{}}}";
  c_ok = true;
  prov.create(QStringLiteral("x"), QString(),
              [&](bool ok, const QString&, const QString&) { c_ok = ok; });
  CHECK(!c_ok);

  // —— 完成：PATCH 端点尾段与 is_completed 体；空 guid 本地拒不发网 ——
  bool done_ok = false;
  prov.complete(QStringLiteral("g-77"),
                [&](bool ok, const QString& k, const QString&) {
                  done_ok = ok;
                  CHECK(k == QStringLiteral("g-77"));
                });
  CHECK(done_ok);
  CHECK(http.calls.last().method == QStringLiteral("PATCH"));
  CHECK(http.calls.last().url.path() ==
        QStringLiteral("/open-apis/task/v2/tasks/g-77"));
  CHECK(http.calls.last().body.contains("\"is_completed\":true"));
  const int n = http.calls.size();
  bool k_ok = true;
  prov.complete(QStringLiteral(" "),
                [&](bool ok, const QString&, const QString&) { k_ok = ok; });
  CHECK(!k_ok);
  CHECK(http.calls.size() == n);

  if (g_failures == 0) {
    std::cout << "test_feishu_provider: all checks passed\n";
    return 0;
  }
  std::cout << "test_feishu_provider: " << g_failures
            << " check(s) FAILED\n";
  return 1;
}
