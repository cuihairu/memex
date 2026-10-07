// R27-3 provider 传输抽象：真 provider（GitHub/飞书/钉钉）的 HTTP 面
// 走此接口——测试注入假传输回放固定响应（不打外网），运行期用
// QNetworkAccessManager 实现（FilesClient 同款）。
#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;

namespace memex::client {

using HttpFn = std::function<void(
    int status, const QByteArray& body, const QString& error)>;

class TaskHttp {
 public:
  virtual ~TaskHttp() = default;
  // 单发请求：method GET/POST/PATCH/PUT；收尾必回调一次（网络错
  // status=0 带 error 文案）。实现须保回调在发出方对象存续期内到达。
  virtual void request(const QString& method, const QUrl& url,
                       const QList<QPair<QByteArray, QByteArray>>& headers,
                       const QByteArray& body, const HttpFn& done) = 0;
};

// QtNetwork 实现（QObject 只为回调上下文；非拥有 nam，调用方保其存续）
class QtNetworkTaskHttp : public QObject, public TaskHttp {
 public:
  explicit QtNetworkTaskHttp(QNetworkAccessManager* nam) : nam_(nam) {}
  void request(const QString& method, const QUrl& url,
               const QList<QPair<QByteArray, QByteArray>>& headers,
               const QByteArray& body, const HttpFn& done) override;

 private:
  QNetworkAccessManager* nam_;
};

} // namespace memex::client
