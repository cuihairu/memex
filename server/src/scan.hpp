// R23-5 上传安全扫描钩子：上传受理序里的可插拔判定点（接口在本仓，
// 引擎接入属部署面）。判定语义（设计裁量留档 todo）：
//  - verdict=clean 放行；infected 拒收 422（字节不落、流水照记、明示文案）
//  - 默认直通（PassThrough）：钩子位存在但行为零变化——外网入口的防护
//    先落类型/大小与二次验证（显式开启），杀毒引擎接 clamd 等另批
//  - 秒传命中跳过扫描：同哈希同归属内容不变，判定幂等；历史行的清理
//    归过期清理面（不在受理序重扫）
#pragma once

#include <string>

namespace memex::server {

class UploadScanner {
 public:
  enum class Verdict { Clean, Infected };

  virtual ~UploadScanner() = default;
  // 扫描一次上传（字节已在内存；file_name 仅作报告上下文）。const：
  // 受理序里以 const 指针持有调用
  virtual Verdict scan(const std::string& file_name,
                       const std::string& body) const = 0;
};

// 默认实现：直通（R23-5 钩子位；引擎接入时替换，受理序不变）
class PassThroughScanner final : public UploadScanner {
 public:
  Verdict scan(const std::string&, const std::string&) const override {
    return Verdict::Clean;
  }
};

} // namespace memex::server
