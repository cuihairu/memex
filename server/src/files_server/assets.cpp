// 品牌物料·表情包·头像（LAY-003 拆分）：读面免鉴权 PNG/表情、
// 本人素材面、四档头像上传下载与品牌物料设置。

#include "files_server/conn.hpp"

namespace memex::server {

// PNG 校验：magic 8 字节＋大小 ≤512KiB＋IHDR 尺寸 ≤2048²。回空串=过，
// 否则回 {status, error}
std::pair<int, std::string> FileConn::png_asset_check(
    const std::string& bytes) {
  static const unsigned char kPngMagic[8] = {0x89, 0x50, 0x4E, 0x47,
                                             0x0D, 0x0A, 0x1A, 0x0A};
  if (bytes.size() < 8 ||
      std::memcmp(bytes.data(), kPngMagic, 8) != 0) {
    return {415, "素材须为 PNG 格式"};
  }
  if (bytes.size() > kBrandAssetMax) {
    return {413, "素材超大小上限（512KiB）"};
  }
  if (bytes.size() >= 24) {
    const auto be32 = [&](std::size_t off) {
      return (static_cast<std::uint32_t>(
                  static_cast<unsigned char>(bytes[off])) << 24) |
             (static_cast<std::uint32_t>(
                  static_cast<unsigned char>(bytes[off + 1])) << 16) |
             (static_cast<std::uint32_t>(
                  static_cast<unsigned char>(bytes[off + 2])) << 8) |
             static_cast<std::uint32_t>(
                 static_cast<unsigned char>(bytes[off + 3]));
    };
    // IHHR 尺寸在固定偏移 16..24（大端宽、高各 4 字节）
    const std::uint32_t w = be32(16);
    const std::uint32_t h = be32(20);
    if (w == 0 || h == 0 || w > 2048 || h > 2048) {
      return {413, "素材尺寸超限（≤2048×2048）"};
    }
  }
  return {0, ""};
}

// 表情素材常量（需求批②）：GIF 动图上限 1MiB
static constexpr std::size_t kEmojiAssetMax = 1024 * 1024;
// 表情图片嗅探（需求批②）：PNG/JPEG/GIF magic + 大小上限。
// 回空串=过；否则回 {status, error}
std::pair<int, std::string> FileConn::emoji_asset_check(
    const std::string& bytes) {
  static const unsigned char kPngMagic[8] = {0x89, 0x50, 0x4E, 0x47,
                                             0x0D, 0x0A, 0x1A, 0x0A};
  if (bytes.size() < 8) {
    return {415, "素材字节过短"};
  }
  const bool png =
      std::memcmp(bytes.data(), kPngMagic, 8) == 0;
  const bool jpeg = static_cast<unsigned char>(bytes[0]) == 0xFF &&
                    static_cast<unsigned char>(bytes[1]) == 0xD8 &&
                    static_cast<unsigned char>(bytes[2]) == 0xFF;
  const bool gif = bytes.compare(0, 6, "GIF87a") == 0 ||
                   bytes.compare(0, 6, "GIF89a") == 0;
  if (!png && !jpeg && !gif) {
    return {415, "表情须为 PNG/JPEG/GIF 图片"};
  }
  if (bytes.size() > kEmojiAssetMax) {
    return {413, "表情超大小上限（1MiB）"};
  }
  return {0, ""};
}
// 嗅探结果 → Content-Type（emoji_asset_check 已校验过，兜底 png）
const char* FileConn::emoji_content_type(const std::string& bytes) {
  static const unsigned char kPngMagic[8] = {0x89, 0x50, 0x4E, 0x47,
                                             0x0D, 0x0A, 0x1A, 0x0A};
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
      static_cast<unsigned char>(bytes[1]) == 0xD8) {
    return "image/jpeg";
  }
  if (bytes.compare(0, 6, "GIF87a") == 0 ||
      bytes.compare(0, 6, "GIF89a") == 0) {
    return "image/gif";
  }
  (void)kPngMagic;
  return "image/png";
}
void FileConn::respond_png(int status, const std::vector<unsigned char>& bytes) {
  respond_image(status, "image/png", bytes);
}
// 图片字节回包（需求批②：表情素材 png/jpeg/gif 同一出口）
void FileConn::respond_image(int status, const char* content_type,
                   const std::vector<unsigned char>& bytes) {
  std::ostringstream head;
  head << "HTTP/1.1 " << status
       << (status == 200 ? " OK" : " Not Found") << "\r\n"
       << "Content-Type: " << content_type << "\r\n"
       << "Content-Length: " << bytes.size() << "\r\n"
       << "Cache-Control: no-cache\r\n"
       << "Connection: close\r\n\r\n";
  const std::string body(bytes.begin(), bytes.end());
  write_raw(std::make_shared<const std::string>(head.str()), false,
            [this, body](std::error_code ec) {
              if (ec) return;
              write_raw(
                  std::make_shared<const std::string>(body), true,
                  [](std::error_code) {});
            });
}
// —— 表情包素材（需求批②）：个人素材面，判权＝仅本人 ——
void FileConn::route_emoji_upload(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  const std::string name = query_param(query_, "name");
  if (name.empty()) {
    respond_json(400, {{"ok", false}, {"error", "缺少素材名（?name=）"}});
    return;
  }
  if (body.empty()) {
    respond_json(400, {{"ok", false}, {"error", "素材字节不可空"}});
    return;
  }
  const auto [status, err] = emoji_asset_check(body);
  if (status != 0) {
    respond_json(status, {{"ok", false}, {"error", err}});
    return;
  }
  const std::vector<unsigned char> bytes(body.begin(), body.end());
  const std::int64_t id =
      impl_.store.emoji_add(account, name, bytes, now_ms());
  if (id < 0) {
    respond_json(500, {{"ok", false}, {"error", "素材写入失败"}});
    return;
  }
  std::cout << "[MEMEX] files emoji upload by=" << account
            << " name=" << name << " bytes=" << body.size() << std::endl;
  respond_json(200, {{"ok", true}, {"id", id}});
}
void FileConn::route_emoji_list() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json arr = json::array();
  for (const auto& a : impl_.store.emoji_list(account)) {
    arr.push_back({{"id", a.id}, {"name", a.name}, {"size", a.size},
                   {"ts_ms", a.ts_ms}});
  }
  respond_json(200, {{"ok", true}, {"assets", arr}});
}
void FileConn::route_emoji_download() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  const std::int64_t id =
      std::strtoll(query_param(query_, "id").c_str(), nullptr, 10);
  std::vector<unsigned char> bytes;
  std::string name;
  if (id <= 0 || !impl_.store.emoji_bytes(id, account, bytes, name)) {
    respond_json(404, {{"ok", false}, {"error", "素材不存在或非本人"}});
    return;
  }
  respond_image(200, emoji_content_type(std::string(bytes.begin(), bytes.end())),
                bytes);
}
void FileConn::route_emoji_delete(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  if (!j.is_object() || !j.contains("id") || !j["id"].is_number()) {
    respond_json(400, {{"ok", false}, {"error", "缺少字段：id"}});
    return;
  }
  const std::int64_t id = j["id"].get<std::int64_t>();
  if (!impl_.store.emoji_delete(id, account)) {
    respond_json(404, {{"ok", false}, {"error", "素材不存在或非本人"}});
    return;
  }
  std::cout << "[MEMEX] files emoji delete by=" << account << " id=" << id
            << std::endl;
  respond_json(200, {{"ok", true}});
}
// —— 用户头像（需求批⑫）：四档 32/64/128/256，客户端裁剪并逐档编码 ——
// 上传：?size=32|64|128|256；raw-bytes POST。判权＝本人属主（目标账号
// 可显式指定但必须与本人一致——他人 403；缺省即本人）。
void FileConn::route_avatar_upload(const std::string& body) {
  const std::string me = account_or_respond();
  if (me.empty()) return;
  const std::string target = query_param(query_, "account");
  if (!target.empty() && target != me) {
    respond_json(403, {{"ok", false},
                       {"error", "头像仅限本人设置（他人 403）"}});
    return;
  }
  const std::string account = me;
  const int size =
      safe_int_param(query_param(query_, "size"), 0, 4096);
  if (size == 0) {
    respond_json(400, {{"ok", false}, {"error", "缺少档位（?size=）"}});
    return;
  }
  if (!impl_.store.avatar_size_valid(size)) {
    respond_json(400, {{"ok", false},
                       {"error", "档位仅支持 32/64/128/256"}});
    return;
  }
  if (body.empty()) {
    respond_json(400, {{"ok", false}, {"error", "头像字节不可空"}});
    return;
  }
  const auto [status, err] = emoji_asset_check(body);
  if (status != 0) {
    respond_json(status, {{"ok", false}, {"error", err}});
    return;
  }
  const std::vector<unsigned char> bytes(body.begin(), body.end());
  const std::string mime =
      emoji_content_type(std::string(bytes.begin(), bytes.end()));
  if (!impl_.store.avatar_put(account, size, mime, bytes, now_ms())) {
    respond_json(500, {{"ok", false}, {"error", "头像写入失败"}});
    return;
  }
  std::cout << "[MEMEX] files avatar upload by=" << account
            << " size=" << size << " bytes=" << body.size() << std::endl;
  respond_json(200, {{"ok", true}, {"size", size},
                     {"ver", impl_.store.avatar_ver(account)}});
}
// 读（登录成员可读＝组织内互见）：?account=＋?size=
void FileConn::route_avatar_download() {
  const std::string me = account_or_respond();
  if (me.empty()) return;
  const std::string target = query_param(query_, "account");
  const std::string want = target.empty() ? me : target;
  const int size = safe_int_param(query_param(query_, "size"), 0, 4096);
  if (!impl_.store.avatar_size_valid(size)) {
    respond_json(400, {{"ok", false},
                       {"error", "档位仅支持 32/64/128/256"}});
    return;
  }
  std::vector<unsigned char> bytes;
  std::string mime;
  if (!impl_.store.avatar_bytes(want, size, bytes, mime)) {
    respond_json(404, {{"ok", false}, {"error", "该账号未设置头像"}});
    return;
  }
  respond_image(200, mime.empty() ? "image/png" : mime.c_str(), bytes);
}
// 删除：仅本人（body {"account": X} 缺省=本人；他人 403）；
// 无头像可删 404（与 emoji 面同口径）
void FileConn::route_avatar_delete(const std::string& body) {
  const std::string me = account_or_respond();
  if (me.empty()) return;
  std::string target;
  if (!body.empty()) {
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (j.is_object() && j.contains("account") && j["account"].is_string()) {
      target = j["account"].get<std::string>();
    }
  }
  if (!target.empty() && target != me) {
    respond_json(403, {{"ok", false},
                       {"error", "头像仅限本人删除（他人 403）"}});
    return;
  }
  if (!impl_.store.avatar_clear(me)) {
    respond_json(404, {{"ok", false}, {"error", "无头像可删"}});
    return;
  }
  std::cout << "[MEMEX] files avatar delete by=" << me << std::endl;
  respond_json(200, {{"ok", true}, {"ver", 0}});
}
void FileConn::route_branding_get() {
  const auto b = impl_.store.branding_get();
  respond_json(200, {{"ok", true},
                     {"company_name", b.company_name},
                     {"accent", b.accent},
                     {"slogan", b.slogan},
                     {"has_logo", !b.logo.empty()},
                     {"has_splash", !b.splash.empty()},
                     {"version", b.version}});
}
void FileConn::route_branding_asset(const char* /*name*/, bool logo) {
  const auto b = impl_.store.branding_get();
  const auto& bytes = logo ? b.logo : b.splash;
  if (bytes.empty()) {
    respond_json(404, {{"ok", false}, {"error", "素材未配置"}});
    return;
  }
  respond_png(200, bytes);
}
void FileConn::route_branding_set(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  if (!branding_can_manage(account)) {
    respond_json(403, {{"ok", false}, {"error", "品牌设置归 org-admin"}});
    return;
  }
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  if (!j.is_object()) {
    respond_json(400, {{"ok", false}, {"error", "请求体须为对象"}});
    return;
  }
  if (j.contains("company_name") && !j["company_name"].is_string()) {
    respond_json(400, {{"ok", false}, {"error", "company_name 须为字符串"}});
    return;
  }
  if (j.contains("accent") && !j["accent"].is_string()) {
    respond_json(400, {{"ok", false}, {"error", "accent 须为字符串"}});
    return;
  }
  if (j.contains("slogan") && !j["slogan"].is_string()) {
    respond_json(400, {{"ok", false}, {"error", "slogan 须为字符串"}});
    return;
  }
  // 素材清除（品牌设置页「清除 logo/splash」按钮）：布尔门；清除即
  // 一次独立动作＝一个版本（文本三件与清除同请求时按序各 bump）
  if ((j.contains("clear_logo") && !j["clear_logo"].is_boolean()) ||
      (j.contains("clear_splash") && !j["clear_splash"].is_boolean())) {
    respond_json(400,
                 {{"ok", false}, {"error", "clear_logo/clear_splash 须为布尔"}});
    return;
  }
  const bool clear_logo = j.value("clear_logo", false);
  const bool clear_splash = j.value("clear_splash", false);
  // 字段缺省=nullopt（不动）；显式空串=清空
  const auto pick = [&](const char* key) -> std::optional<std::string> {
    if (!j.contains(key)) return std::nullopt;
    return j[key].get<std::string>();
  };
  const std::optional<std::string> name = pick("company_name");
  const std::optional<std::string> accent = pick("accent");
  const std::optional<std::string> slogan = pick("slogan");
  // accent 形态门：#rrggbb（显式空串=清空放行）
  if (accent.has_value() && !accent->empty()) {
    bool ok = accent->size() == 7 && (*accent)[0] == '#';
    for (std::size_t i = 1; ok && i < 7; ++i) {
      const char c = (*accent)[i];
      ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
    }
    if (!ok) {
      respond_json(400,
                   {{"ok", false}, {"error", "accent 须为 #rrggbb 形态"}});
      return;
    }
  }
  std::int64_t version = 0;
  bool wrote = false;
  if (name.has_value() || accent.has_value() || slogan.has_value()) {
    version = impl_.store.branding_set(name, accent, slogan, now_ms());
    if (version < 0) {
      respond_json(500, {{"ok", false}, {"error", "品牌配置写入失败"}});
      return;
    }
    wrote = true;
  }
  if (clear_logo) {
    version = impl_.store.branding_clear_logo();
    if (version < 0) {
      respond_json(500, {{"ok", false}, {"error", "品牌配置写入失败"}});
      return;
    }
    wrote = true;
  }
  if (clear_splash) {
    version = impl_.store.branding_clear_splash();
    if (version < 0) {
      respond_json(500, {{"ok", false}, {"error", "品牌配置写入失败"}});
      return;
    }
    wrote = true;
  }
  if (!wrote) {
    respond_json(400, {{"ok", false}, {"error", "无可写品牌字段"}});
    return;
  }
  std::cout << "[MEMEX] files branding set by=" << account
            << " version=" << version << std::endl;
  respond_json(200, {{"ok", true}, {"version", version}});
}
void FileConn::route_branding_asset_upload(const std::string& body, bool logo) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  if (!branding_can_manage(account)) {
    respond_json(403, {{"ok", false}, {"error", "品牌设置归 org-admin"}});
    return;
  }
  if (body.empty()) {
    respond_json(400, {{"ok", false}, {"error", "素材字节不可空"}});
    return;
  }
  const auto [status, err] = png_asset_check(body);
  if (status != 0) {
    respond_json(status, {{"ok", false}, {"error", err}});
    return;
  }
  const std::vector<unsigned char> bytes(body.begin(), body.end());
  const std::int64_t version =
      logo ? impl_.store.branding_set_logo(bytes, now_ms())
           : impl_.store.branding_set_splash(bytes, now_ms());
  if (version < 0) {
    respond_json(500, {{"ok", false}, {"error", "素材写入失败"}});
    return;
  }
  std::cout << "[MEMEX] files branding " << (logo ? "logo" : "splash")
            << " upload by=" << account << " bytes=" << body.size()
            << " version=" << version << std::endl;
  respond_json(200, {{"ok", true}, {"version", version}});
}

} // namespace memex::server
