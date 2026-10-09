// 皮肤包体系（需求批⑮）实现：stored zip 读写＋CRC32 自算＋HMAC-SHA256 签名
// ＋清单解析校验。导入链顺序＝zip 结构→验签→清单字段→覆盖面（先验签再
// 解析：签名不过不碰 JSON）。任何失败只返回语义化文案，零副作用。
#include "theme_pack.hpp"

#include "theme.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QtGlobal>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>

namespace memex::client {
namespace {

// 内置固定签名密钥（HMAC-SHA256）。随二进制分发＝完整性/防坏包口径（防
// 篡改、防外来包误导入），不是机密性边界——此限制在 todo 进展笔如实注明。
const unsigned char kSkinPackKey[] = {
    0x9d, 0x3a, 0x51, 0xc2, 0xe8, 0x07, 0xb4, 0x66, 0x1f, 0xa9, 0xd0,
    0x35, 0x7c, 0x48, 0x8e, 0x12, 0xbb, 0x6f, 0x93, 0x25, 0xe1, 0x5a,
    0xc7, 0x0d, 0xf4, 0x39, 0x8b, 0x71, 0x2e, 0xad, 0x63, 0x90};

// —— 防坏包硬底线（导入侧全部强制）——
constexpr int kMaxEntries = 32;                    // 条目数上限
constexpr qint64 kMaxTotalBytes = 4 * 1024 * 1024; // 解压总大小上限
constexpr qint64 kMaxEntryBytes = 1024 * 1024;     // 单条解压大小上限
constexpr qint64 kMaxManifestBytes = 256 * 1024;   // manifest 大小上限
constexpr qint64 kMaxSignatureBytes = 1024;        // signature 大小上限

constexpr quint32 kEocdSig = 0x06054b50u;
constexpr quint32 kCdSig = 0x02014b50u;
constexpr quint32 kLocalSig = 0x04034b50u;

// IEEE CRC-32（zip 规范多项式 0xEDB88320），静态查表
quint32 crc32_of(const QByteArray& data) {
  static std::array<quint32, 256> table = [] {
    std::array<quint32, 256> t{};
    for (quint32 i = 0; i < 256; ++i) {
      quint32 c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      t[i] = c;
    }
    return t;
  }();
  quint32 crc = 0xFFFFFFFFu;
  const auto* p = reinterpret_cast<const unsigned char*>(data.constData());
  for (int i = 0; i < data.size(); ++i) {
    crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

void put_u16(QByteArray& out, quint16 v) {
  out.append(char(v & 0xFF));
  out.append(char((v >> 8) & 0xFF));
}

void put_u32(QByteArray& out, quint32 v) {
  out.append(char(v & 0xFF));
  out.append(char((v >> 8) & 0xFF));
  out.append(char((v >> 16) & 0xFF));
  out.append(char((v >> 24) & 0xFF));
}

quint16 get_u16(const QByteArray& d, int off) {
  return quint16(quint8(d[off])) | quint16(quint8(d[off + 1])) << 8;
}

quint32 get_u32(const QByteArray& d, int off) {
  return quint32(quint8(d[off])) | quint32(quint8(d[off + 1])) << 8 |
         quint32(quint8(d[off + 2])) << 16 |
         quint32(quint8(d[off + 3])) << 24;
}

// 条目名守卫：路径穿越（..）、绝对路径、盘符、反斜杠、空名一律拒
bool entry_name_dangerous(const QString& name) {
  if (name.isEmpty() || name == QStringLiteral(".") ||
      name == QStringLiteral("..")) {
    return true;
  }
  if (name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) ||
      name.contains(QLatin1Char(':')) || name.contains(QLatin1Char('\0'))) {
    return true;
  }
  const QStringList parts = name.split(QLatin1Char('/'));
  for (const QString& part : parts) {
    if (part == QStringLiteral("..")) {
      return true;
    }
  }
  return false;
}

// zip 规范字符串字段按「zip 非法（假签名格式的包）」统一文案
QString kBadZip =
    QStringLiteral("zip 结构损坏：不是有效的皮肤包（manifest.json ＋ "
                   "signature.txt）");

// 导入链第 1 步：解析 stored zip（EOCD 尾扫→中央目录遍历→本地头校验→
// CRC 校验），途中强制全部硬底线
struct SkinZipEntry {
  QString name;
  QByteArray data;
};

bool parse_skin_zip(const QByteArray& blob, QList<SkinZipEntry>* out,
                    QString* error) {
  // EOCD 从尾部扫描（注释最长 65535）
  int eocd = -1;
  const int scan_floor =
      qMax(0, blob.size() - 22 - 65535);
  for (int i = blob.size() - 22; i >= scan_floor; --i) {
    if (get_u32(blob, i) == kEocdSig) {
      const quint16 comment_len = get_u16(blob, i + 20);
      if (i + 22 + comment_len == blob.size()) {
        eocd = i;
        break;
      }
    }
  }
  if (eocd < 0) {
    *error = kBadZip;
    return false;
  }
  const quint16 count = get_u16(blob, eocd + 10);
  const quint32 cd_offset = get_u32(blob, eocd + 16);
  if (count > kMaxEntries) {
    *error = QStringLiteral("皮肤包条目数超限（最多 %1 项）")
                 .arg(kMaxEntries);
    return false;
  }
  if (cd_offset > quint32(blob.size())) {
    *error = kBadZip;
    return false;
  }
  qint64 total = 0;
  int p = int(cd_offset);
  for (int idx = 0; idx < count; ++idx) {
    if (p < 0 || p + 46 > blob.size() || get_u32(blob, p) != kCdSig) {
      *error = kBadZip;
      return false;
    }
    const quint16 method = get_u16(blob, p + 10);
    const quint32 crc = get_u32(blob, p + 16);
    const quint32 csize = get_u32(blob, p + 20);
    const quint32 usize = get_u32(blob, p + 24);
    const quint16 name_len = get_u16(blob, p + 28);
    const quint16 extra_len = get_u16(blob, p + 30);
    const quint16 comment_len = get_u16(blob, p + 32);
    const quint32 local_off = get_u32(blob, p + 42);
    if (p + 46 + name_len + extra_len + comment_len > blob.size()) {
      *error = kBadZip;
      return false;
    }
    SkinZipEntry entry;
    entry.name =
        QString::fromUtf8(blob.constData() + p + 46, name_len);
    if (entry_name_dangerous(entry.name)) {
      *error = QStringLiteral("皮肤包含非法条目名（路径穿越或绝对路径）：%1")
                   .arg(entry.name);
      return false;
    }
    if (method != 0) {
      *error = QStringLiteral("压缩方式不支持：皮肤包只收无压缩（stored）"
                              "格式，请用 memex 导出的原始包");
      return false;
    }
    if (usize != csize) {
      *error = kBadZip;
      return false;
    }
    if (csize > quint32(kMaxEntryBytes)) {
      *error = QStringLiteral("皮肤包单条目过大（上限 1 MiB）");
      return false;
    }
    total += qint64(csize);
    if (total > kMaxTotalBytes) {
      *error = QStringLiteral("皮肤包内容总量超限（上限 4 MiB）");
      return false;
    }
    // 本地头：签名＋长度字段，跳过其 name/extra 取数据。先单独判
    // local_off 越界再加 30——防 0xFFFF…+30 回绕成小值绕过检查
    if (local_off > quint32(blob.size()) ||
        local_off + 30 > quint32(blob.size()) ||
        get_u32(blob, int(local_off)) != kLocalSig) {
      *error = kBadZip;
      return false;
    }
    const quint16 local_name_len = get_u16(blob, int(local_off) + 26);
    const quint16 local_extra_len = get_u16(blob, int(local_off) + 28);
    const qint64 data_off =
        qint64(local_off) + 30 + local_name_len + local_extra_len;
    if (data_off + qint64(csize) > blob.size()) {
      *error = kBadZip;
      return false;
    }
    entry.data = blob.mid(data_off, int(csize));
    if (crc32_of(entry.data) != crc) {
      *error = QStringLiteral("皮肤包数据损坏（CRC 校验失败：%1）")
                   .arg(entry.name);
      return false;
    }
    out->append(entry);
    p += 46 + name_len + extra_len + comment_len;
  }
  return true;
}

QColor color_from_hex_argb(const QString& hex) { return QColor(hex); }

}  // namespace

QString validate_skin_manifest(const SkinPackManifest& manifest) {
  if (manifest.name.isEmpty()) {
    return QStringLiteral("清单缺少字段：name");
  }
  if (manifest.name.size() > 64) {
    return QStringLiteral("包名过长（上限 64 字符）");
  }
  if (manifest.name.contains(QLatin1Char('/')) ||
      manifest.name.contains(QLatin1Char('\\')) ||
      manifest.name.contains(QLatin1Char(':')) ||
      manifest.name.contains(QLatin1Char('\n')) ||
      manifest.name.contains(QLatin1Char('\r')) ||
      manifest.name == ThemeManager::customThemeName()) {
    return QStringLiteral("包名含非法字符或与内置槽位重名：%1")
        .arg(manifest.name);
  }
  if (ThemeManager::builtin_themes().contains(manifest.name)) {
    return QStringLiteral("包名与内置主题重名（%1），请改名后重试")
        .arg(manifest.name);
  }
  static const QRegularExpression kSemver(
      QStringLiteral("^[0-9]+\\.[0-9]+\\.[0-9]+$"));
  if (!kSemver.match(manifest.version).hasMatch()) {
    return QStringLiteral("版本号不是合法 semver（应为 X.Y.Z）：%1")
        .arg(manifest.version);
  }
  if (manifest.author.isEmpty() || manifest.author.size() > 128) {
    return QStringLiteral("作者字段缺失或过长（1–128 字符）");
  }
  if (!ThemeManager::builtin_themes().contains(manifest.base)) {
    return QStringLiteral("基线主题不存在（须为内置主题名）：%1")
        .arg(manifest.base);
  }
  const QStringList allowed = ThemeManager::customizable_tokens();
  for (auto it = manifest.overrides.constBegin();
       it != manifest.overrides.constEnd(); ++it) {
    if (!allowed.contains(it.key())) {
      return QStringLiteral(
                 "覆盖令牌不在可改色放行面（品牌橙与语义色锁死）：%1")
          .arg(it.key());
    }
    if (!it.value().isValid()) {
      return QStringLiteral("令牌 %1 的色值无效").arg(it.key());
    }
  }
  return {};
}

QByteArray skin_manifest_json(const SkinPackManifest& manifest) {
  QJsonObject overrides;
  for (auto it = manifest.overrides.constBegin();
       it != manifest.overrides.constEnd(); ++it) {
    overrides.insert(it.key(), it.value().name(QColor::HexArgb));
  }
  QJsonObject root;
  root.insert(QStringLiteral("format"), QStringLiteral("memex-skin-pack"));
  root.insert(QStringLiteral("format_version"), 1);
  root.insert(QStringLiteral("name"), manifest.name);
  root.insert(QStringLiteral("version"), manifest.version);
  root.insert(QStringLiteral("author"), manifest.author);
  root.insert(QStringLiteral("base"), manifest.base);
  root.insert(QStringLiteral("overrides"), overrides);
  return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

SkinPackManifest parse_skin_manifest(const QByteArray& json, QString* error) {
  // error 参数可空：错误统一先进本地串，出口再拷给调用方
  QString error_storage;
  auto fail = [&error_storage, error](const QString& msg) {
    error_storage = msg;
    if (error) {
      *error = msg;
    }
    return SkinPackManifest{};
  };
  QJsonParseError parse_error{};
  const QJsonDocument doc =
      QJsonDocument::fromJson(json, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
    return fail(QStringLiteral("清单 JSON 解析失败：%1")
                    .arg(parse_error.errorString()));
  }
  const QJsonObject root = doc.object();
  if (root.value(QStringLiteral("format")).toString() !=
      QStringLiteral("memex-skin-pack")) {
    return fail(QStringLiteral("不是 memex 皮肤包（format 字段缺失或不符）"));
  }
  SkinPackManifest manifest;
  const auto str_field = [&root](const char* key) {
    const QJsonValue v = root.value(QLatin1String(key));
    return v.isString() ? v.toString() : QString{};
  };
  manifest.name = str_field("name");
  manifest.version = str_field("version");
  manifest.author = str_field("author");
  manifest.base = str_field("base");
  if (!root.value(QStringLiteral("name")).isString()) {
    return fail(QStringLiteral("清单缺少字段或类型错误：name"));
  }
  if (!root.value(QStringLiteral("version")).isString()) {
    return fail(QStringLiteral("清单缺少字段或类型错误：version"));
  }
  if (!root.value(QStringLiteral("author")).isString()) {
    return fail(QStringLiteral("清单缺少字段或类型错误：author"));
  }
  if (!root.value(QStringLiteral("base")).isString()) {
    return fail(QStringLiteral("清单缺少字段或类型错误：base"));
  }
  const QJsonValue overrides_value = root.value(QStringLiteral("overrides"));
  if (!overrides_value.isObject()) {
    return fail(QStringLiteral("清单缺少字段或类型错误：overrides"));
  }
  const QJsonObject overrides = overrides_value.toObject();
  for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it) {
    if (!it.value().isString()) {
      return fail(QStringLiteral("覆盖项 %1 的色值不是字符串").arg(it.key()));
    }
    const QColor color = color_from_hex_argb(it.value().toString());
    if (!color.isValid()) {
      return fail(QStringLiteral("覆盖项 %1 的色值不是合法 HexArgb").arg(
          it.key()));
    }
    manifest.overrides.insert(it.key(), color);
  }
  error_storage = validate_skin_manifest(manifest);
  if (error) {
    *error = error_storage;  // 成功写空串（清调用方残留），失败写语义化文案
  }
  if (!error_storage.isEmpty()) {
    return SkinPackManifest{};
  }
  return manifest;
}

QByteArray sign_skin_bytes(const QByteArray& manifest_json) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  HMAC(EVP_sha256(), kSkinPackKey, int(sizeof(kSkinPackKey)) - 1,
       reinterpret_cast<const unsigned char*>(manifest_json.constData()),
       size_t(manifest_json.size()), digest, &digest_len);
  return QByteArray(reinterpret_cast<const char*>(digest),
                    int(digest_len))
      .toHex();
}

QByteArray build_skin_zip(
    const QList<QPair<QString, QByteArray>>& entries, QString* error) {
  struct EntryPlan {
    QByteArray name;
    quint32 crc = 0;
    quint32 size = 0;
    quint32 local_offset = 0;
  };
  QList<EntryPlan> plans;
  QByteArray out;
  for (const auto& entry : entries) {
    // 写侧不做条目名策略检查：这是裸格式写入缝（导出路径只写
    // manifest.json／signature.txt 两个安全名，测试用它构造攻击包——
    // 安全边界在导入侧 parse_skin_zip）
    EntryPlan plan;
    plan.name = entry.first.toUtf8();
    plan.crc = crc32_of(entry.second);
    plan.size = quint32(entry.second.size());
    plan.local_offset = quint32(out.size());
    plans.append(plan);
    // 本地文件头（method 0＝stored；时间字段置零，导出可复现）
    put_u32(out, kLocalSig);
    put_u16(out, 20);  // version needed
    put_u16(out, 0);   // flags
    put_u16(out, 0);   // method = stored
    put_u16(out, 0);   // mod time
    put_u16(out, 0);   // mod date
    put_u32(out, plan.crc);
    put_u32(out, plan.size);
    put_u32(out, plan.size);
    put_u16(out, quint16(plan.name.size()));
    put_u16(out, 0);  // extra len
    out.append(plan.name);
    out.append(entry.second);
  }
  const quint32 cd_offset = quint32(out.size());
  for (const EntryPlan& plan : plans) {
    put_u32(out, kCdSig);
    put_u16(out, 20);  // version made by
    put_u16(out, 20);  // version needed
    put_u16(out, 0);   // flags
    put_u16(out, 0);   // method
    put_u16(out, 0);   // mod time
    put_u16(out, 0);   // mod date
    put_u32(out, plan.crc);
    put_u32(out, plan.size);
    put_u32(out, plan.size);
    put_u16(out, quint16(plan.name.size()));
    put_u16(out, 0);  // extra len
    put_u16(out, 0);  // comment len
    put_u16(out, 0);  // disk start
    put_u16(out, 0);  // internal attrs
    put_u32(out, 0);  // external attrs
    put_u32(out, plan.local_offset);
    out.append(plan.name);
  }
  const quint32 cd_size = quint32(out.size()) - cd_offset;
  put_u32(out, kEocdSig);
  put_u16(out, 0);  // disk number
  put_u16(out, 0);  // cd start disk
  put_u16(out, quint16(plans.size()));
  put_u16(out, quint16(plans.size()));
  put_u32(out, cd_size);
  put_u32(out, cd_offset);
  put_u16(out, 0);  // comment len
  return out;
}

QByteArray export_skin_pack(const SkinPackManifest& manifest,
                            QString* error) {
  const QString invalid = validate_skin_manifest(manifest);
  if (!invalid.isEmpty()) {
    if (error) {
      *error = invalid;
    }
    return {};
  }
  const QByteArray json = skin_manifest_json(manifest);
  QList<QPair<QString, QByteArray>> entries;
  entries.append({QStringLiteral("manifest.json"), json});
  entries.append({QStringLiteral("signature.txt"), sign_skin_bytes(json)});
  return build_skin_zip(entries, error);
}

SkinPackImport import_skin_pack(const QByteArray& zip_bytes) {
  SkinPackImport result;
  // 第 1 步：zip 结构＋硬底线
  QList<SkinZipEntry> entries;
  QString error;
  if (!parse_skin_zip(zip_bytes, &entries, &error)) {
    result.error = error;
    return result;
  }
  QByteArray manifest_bytes;
  QByteArray signature_bytes;
  for (const SkinZipEntry& entry : entries) {
    if (entry.name == QStringLiteral("manifest.json")) {
      manifest_bytes = entry.data;
    } else if (entry.name == QStringLiteral("signature.txt")) {
      signature_bytes = entry.data;
    }  // 其余条目忽略（计数与总量已受限）
  }
  if (manifest_bytes.isEmpty()) {
    result.error = QStringLiteral("包内缺少 manifest.json");
    return result;
  }
  if (signature_bytes.isEmpty()) {
    result.error = QStringLiteral("包内缺少 signature.txt");
    return result;
  }
  if (manifest_bytes.size() > kMaxManifestBytes) {
    result.error = QStringLiteral("清单过大（上限 256 KiB）");
    return result;
  }
  if (signature_bytes.size() > kMaxSignatureBytes) {
    result.error = QStringLiteral("签名文件过大（上限 1 KiB）");
    return result;
  }
  // 第 2 步：验签（先验签再解析）
  const QByteArray expected = sign_skin_bytes(manifest_bytes);
  const QByteArray provided =
      QByteArray(signature_bytes).trimmed().toLower();
  if (provided != expected) {
    result.error = QStringLiteral(
        "签名校验失败：包被篡改或不是本应用导出的皮肤包");
    return result;
  }
  // 第 3 步：清单解析＋字段校验＋覆盖面校验
  result.manifest = parse_skin_manifest(manifest_bytes, &result.error);
  result.ok = result.error.isEmpty();
  if (!result.ok) {
    result.manifest = SkinPackManifest{};
  }
  return result;
}

int compare_skin_versions(const QString& a, const QString& b) {
  const QStringList pa = a.split(QLatin1Char('.'));
  const QStringList pb = b.split(QLatin1Char('.'));
  for (int i = 0; i < 3; ++i) {
    const qlonglong va = i < pa.size() ? pa.at(i).toLongLong() : 0;
    const qlonglong vb = i < pb.size() ? pb.at(i).toLongLong() : 0;
    if (va != vb) {
      return va < vb ? -1 : 1;
    }
  }
  return 0;
}

}  // namespace memex::client
