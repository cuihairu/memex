// R27-3 首批 provider 之一：GitHub Issues（REST v3，PAT 令牌）。
// 能力声明满深度：L1 跳转（html_url 直出）＋L2 拉列表（open issues，
// 滤掉 PR）＋L3 双向（建 issue/关 issue 回写）。token 会话内存不落盘
//（与既有账号口令同水位；加密落盘随设置页走加密面，留后续如实注明）。
// 传输经 TaskHttp 抽象——测试注入假传输回放固定响应，不打外网。
#pragma once

#include <QString>

#include "engine/task/task_http.hpp"
#include "engine/task/task_provider.hpp"

namespace memex::client {

class GitHubIssuesProvider : public TaskProvider {
 public:
  // repo 绑定 "org/repo"；token 为 PAT（空=仅 L1 跳转可用的哑态）。
  // http 非拥有（调用方保存续，一般与 provider 同主同亡）。
  GitHubIssuesProvider(QString repo, QString token, TaskHttp* http)
      : repo_(std::move(repo)), token_(std::move(token)), http_(http) {}

  QString id() const override { return QStringLiteral("github-issue"); }
  QString name() const override { return QStringLiteral("GitHub Issues"); }
  TaskProviderAuth auth_kind() const override {
    return TaskProviderAuth::kToken;
  }
  int capabilities() const override {
    return kCapL1Jump | kCapL2Read | kCapL3Write;
  }

  // 键=issue 号；详情页 https://github.com/{repo}/issues/{key}
  QString detail_url(const QString& key,
                     const QString& project = QString()) const override;

  // L2：GET /repos/{repo}/issues?state=open&per_page=50（滤 PR）
  void list(const TaskListFn& done) const override;
  // L3：POST /repos/{repo}/issues {title,body} → number
  void create(const QString& title, const QString& note,
              const TaskWriteFn& done) override;
  // L3：PATCH /repos/{repo}/issues/{key} {state:"closed"}
  void complete(const QString& key, const TaskWriteFn& done) override;

 private:
  QUrl api_url(const QString& path) const;

  QString repo_;
  QString token_;
  TaskHttp* http_;
};

} // namespace memex::client
