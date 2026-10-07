// R27-3 首批 provider 之二：钉钉待办（open.dingtalk.com 待办任务 API，
// 企业自建应用 appKey/appSecret → accessToken）。能力声明 L2 拉列表＋
// L3 建/完成回写；L1 不声明——钉钉待办无稳定 web 详情页形态，详情
// 跳转走 list 条目自带的 detailUrl 字段（可空，空=该条目不可跳）。
// 形态出处：open.dingtalk.com「创建钉钉待办任务/更新待办/查询待办列表」
//（2026-10-08 核实端点与鉴权头 x-acs-dingtalk-access-token；字段名以
// 文档为准，未真连验证——本机无租户凭据，假传输锁我方行为）。
// unionId 由配置面给出（获取流程属设置页，留后续如实注明）。
#pragma once

#include <QString>

#include "engine/task/task_http.hpp"
#include "engine/task/task_provider.hpp"

namespace memex::client {

class DingtalkTodoProvider : public TaskProvider {
 public:
  DingtalkTodoProvider(QString app_key, QString app_secret, QString union_id,
                       TaskHttp* http)
      : app_key_(std::move(app_key)),
        app_secret_(std::move(app_secret)),
        union_id_(std::move(union_id)),
        http_(http) {}

  QString id() const override { return QStringLiteral("dingtalk-todo"); }
  QString name() const override { return QStringLiteral("钉钉待办"); }
  TaskProviderAuth auth_kind() const override {
    return TaskProviderAuth::kToken;
  }
  int capabilities() const override { return kCapL2Read | kCapL3Write; }

  // L1 未声明：钉钉待办详情无法从键构造稳定 URL（恒空串）
  QString detail_url(const QString& key,
                     const QString& project = QString()) const override;

  // L2：先取 accessToken 再 GET /v1.0/todo/users/{uid}/tasks
  void list(const TaskListFn& done) const override;
  // L3：POST /v1.0/todo/users/{uid}/tasks（subject/detailUrl/dueTime）
  void create(const QString& title, const QString& note,
              const TaskWriteFn& done) override;
  // L3：PUT /v1.0/todo/users/{uid}/tasks/{key} {isDone:true}
  void complete(const QString& key, const TaskWriteFn& done) override;

 private:
  void with_token(const std::function<void(const QString&, const QString&)>&
                      next) const; // 取缓存令牌→回调（token, error）
  QUrl api_url(const QString& path) const;

  QString app_key_;
  QString app_secret_;
  QString union_id_;
  TaskHttp* http_;

  // 令牌缓存（会话内存；expireIn 到期或未知即重取）
  mutable QString cached_token_;
  mutable qint64 token_expire_ms_{0};
};

} // namespace memex::client
