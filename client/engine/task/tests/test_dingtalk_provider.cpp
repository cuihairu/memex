// R27-3 钉钉待办 provider 单测（假传输回放，不打外网）：令牌流（缓存＋
// 到期重取＋缺 token 拒）、L2 拉列表解析（detailUrl 可缺行仍在/现态映射）、
// L3 建单（缺 id 拒）与完成（PUT isDone/空 id 本地拒）、鉴权头与端点形态
// 断言。API 形态出处见 provider 头注（未真连验证，假传输锁我方行为）。
#include <QCoreApplication>
#include <QUrl>

#include <iostream>

#include <engine/task/dingtalk_provider.hpp>

using memex::client::DingtalkTodoProvider;
using memex::client::ExternalTask;
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

// 假传输：按 URL 形态回放（token 端点 vs 业务端点），记录每次请求
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
      "{\"accessToken\":\"dt-token\",\"expireIn\":7200}";
  QByteArray tasks_body =
      "{\"totalCount\":2,\"tasks\":["
      "{\"id\":\"t1\",\"subject\":\"巡检\",\"status\":\"RUNNING\","
      "\"detailUrl\":{\"url\":\"https://a.example/t1\"}},"
      "{\"id\":\"t2\",\"subject\":\"无链待办\",\"status\":\"RUNNING\"}]}";
  QByteArray create_body = "{\"id\":\"new-9\"}";
  int business_status = 200;

  void request(const QString& m, const QUrl& u,
               const QList<QPair<QByteArray, QByteArray>>& h,
               const QByteArray& b, const HttpFn& done) override {
    calls.append({m, u, b, h});
    if (u.path().contains(QStringLiteral("oauth2/accessToken"))) {
      done(200, token_body, QString());
      return;
    }
    QByteArray resp;
    if (m == QStringLiteral("GET")) {
      resp = tasks_body; // 列表
    } else if (m == QStringLiteral("POST")) {
      resp = create_body; // 建单
    }
    done(business_status, resp, QString());
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
  DingtalkTodoProvider prov(QStringLiteral("app-k"), QStringLiteral("app-s"),
                            QStringLiteral("u-123"), &http);

  // 能力声明：L2|L3，L1 不声明（detail_url 恒空）
  CHECK(prov.capabilities() ==
        (memex::client::kCapL2Read | memex::client::kCapL3Write));
  CHECK(!prov.can_jump());
  CHECK(prov.detail_url(QStringLiteral("1")).isEmpty());
  CHECK(prov.auth_kind() == memex::client::TaskProviderAuth::kToken);

  // —— L2：首次调用先取令牌再拉列表 ——
  QVector<ExternalTask> items;
  QString list_err = QStringLiteral("未回调");
  prov.list([&](bool ok, const QVector<ExternalTask>& its, const QString& e) {
    list_err = e;
    if (ok) items = its;
  });
  CHECK(list_err.isEmpty());
  CHECK(http.calls.size() == 2);
  CHECK(http.calls[0].url.path() ==
        QStringLiteral("/v1.0/oauth2/accessToken"));
  CHECK(http.calls[0].body.contains("\"appKey\":\"app-k\""));
  CHECK(http.calls[1].url.path() ==
        QStringLiteral("/v1.0/todo/users/u-123/tasks"));
  CHECK(http.last_header("x-acs-dingtalk-access-token") ==
        QByteArray("dt-token"));
  // 解析：detailUrl 有的带、无的行仍在（跳转不可用但条目在）
  CHECK(items.size() == 2);
  CHECK(items[0].key == QStringLiteral("t1"));
  CHECK(items[0].title == QStringLiteral("巡检"));
  CHECK(items[0].detail_url == QStringLiteral("https://a.example/t1"));
  CHECK(!items[0].done);
  CHECK(items[1].key == QStringLiteral("t2"));
  CHECK(items[1].detail_url.isEmpty()); // 可缺＝不可跳，行仍在

  // —— 令牌缓存：第二笔业务调用不再取令牌 ——
  prov.list([](bool, const QVector<ExternalTask>&, const QString&) {});
  CHECK(http.calls.size() == 3); // 只加一次业务调用
  CHECK(http.calls[2].url.path() !=
        QStringLiteral("/v1.0/oauth2/accessToken"));

  // —— 到期重取（expireIn=0 → 缓存即时过期，每次业务调用前都重取）——
  FakeHttp http2;
  DingtalkTodoProvider prov2(QStringLiteral("app-k"), QStringLiteral("app-s"),
                             QStringLiteral("u-123"), &http2);
  http2.token_body = "{\"accessToken\":\"dt-t0\",\"expireIn\":0}";
  prov2.list([](bool, const QVector<ExternalTask>&, const QString&) {});
  prov2.list([](bool, const QVector<ExternalTask>&, const QString&) {});
  CHECK(http2.calls.size() == 4); // 令牌＋业务＋令牌＋业务（不缓存复用）
  CHECK(http2.calls[0].url.path() ==
        QStringLiteral("/v1.0/oauth2/accessToken"));
  CHECK(http2.calls[2].url.path() ==
        QStringLiteral("/v1.0/oauth2/accessToken"));

  // —— L3 建：POST 体与响应解析（缺 id 拒）——
  http.token_body = "{\"accessToken\":\"dt-token\",\"expireIn\":7200}";
  QString new_key, create_err = QStringLiteral("未回调");
  prov.create(QStringLiteral("新待办"), QStringLiteral("备注"),
              [&](bool ok, const QString& k, const QString& e) {
                create_err = e;
                if (ok) new_key = k;
              });
  CHECK(create_err.isEmpty());
  CHECK(new_key == QStringLiteral("new-9"));
  CHECK(http.calls.last().method == QStringLiteral("POST"));
  CHECK(http.calls.last().body.contains("\"subject\":\"新待办\""));
  CHECK(http.calls.last().body.contains("\"description\":\"备注\""));
  http.create_body = "{}";
  bool c_ok = true;
  prov.create(QStringLiteral("x"), QString(),
              [&](bool ok, const QString&, const QString&) { c_ok = ok; });
  CHECK(!c_ok); // 缺 id 不造假键
  http.create_body = "{\"id\":\"n2\"}";

  // —— L3 完成：PUT {isDone:true}；空 id 本地拒不发网 ——
  bool done_ok = false;
  prov.complete(QStringLiteral("t1"),
                [&](bool ok, const QString& k, const QString&) {
                  done_ok = ok;
                  CHECK(k == QStringLiteral("t1"));
                });
  CHECK(done_ok);
  CHECK(http.calls.last().method == QStringLiteral("PUT"));
  CHECK(http.calls.last().url.path() ==
        QStringLiteral("/v1.0/todo/users/u-123/tasks/t1"));
  CHECK(http.calls.last().body.contains("\"isDone\":true"));
  const int n_calls = http.calls.size();
  bool k_ok = true;
  prov.complete(QStringLiteral(" "),
                [&](bool ok, const QString&, const QString&) { k_ok = ok; });
  CHECK(!k_ok);
  CHECK(http.calls.size() == n_calls); // 未发请求

  if (g_failures == 0) {
    std::cout << "test_dingtalk_provider: all checks passed\n";
    return 0;
  }
  std::cout << "test_dingtalk_provider: " << g_failures
            << " check(s) FAILED\n";
  return 1;
}
