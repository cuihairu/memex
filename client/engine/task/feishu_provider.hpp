// R27-3 首批 provider 之三：飞书任务（Task v2，自建应用 app_id/app_secret
// → tenant_access_token）。能力声明 L3 建任务/完成回写；L2 暂不声明——
// task v2 未见全量列表公开端点（查询按 guid 单查/过滤语义待查证，接入
// 留后续如实注明）；L1 暂不声明——详情链接走 create/查询回带的 task.url
// 字段（网页详情），detail_url(key) 无稳定可构造形态恒空。
// 形态出处：open.feishu.cn「任务 Task」（设计稿 2026-10-04 已核实 Task v2
// 创建/更新/查询可达；本机无租户凭据未真连验证，假传输锁我方行为）。
#pragma once

#include <QString>

#include <functional>

#include "engine/task/task_http.hpp"
#include "engine/task/task_provider.hpp"

namespace memex::client {

class FeishuTaskProvider : public TaskProvider {
 public:
  FeishuTaskProvider(QString app_id, QString app_secret, TaskHttp* http)
      : app_id_(std::move(app_id)),
        app_secret_(std::move(app_secret)),
        http_(http) {}

  QString id() const override { return QStringLiteral("feishu-task"); }
  QString name() const override { return QStringLiteral("飞书任务"); }
  TaskProviderAuth auth_kind() const override {
    return TaskProviderAuth::kToken;
  }
  int capabilities() const override { return kCapL3Write; }

  QString detail_url(const QString& key,
                     const QString& project = QString()) const override;

  // L3：先取 tenant token 再 POST /open-apis/task/v2/tasks
  //（{summary,description}）→ data.task.guid；回带 data.task.url 作详情
  void create(const QString& title, const QString& note,
              const TaskWriteFn& done) override;
  // L3：PATCH /open-apis/task/v2/tasks/{guid} {is_completed:true}
  void complete(const QString& key, const TaskWriteFn& done) override;

 private:
  void with_token(const std::function<void(const QString&, const QString&)>&
                      next) const;
  QUrl api_url(const QString& path) const;

  QString app_id_;
  QString app_secret_;
  TaskHttp* http_;

  mutable QString cached_token_;
  mutable qint64 token_expire_ms_{0};
};

} // namespace memex::client
