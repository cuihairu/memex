// 皮肤包体系（需求批⑮）：.zip＝manifest.json＋signature.txt 两入口
// （unknown 条目忽略但计数受限）。导入链＝zip 结构→验签（先验签再解析）→
// 清单字段→覆盖面（与 ⑭ apply_overrides 同口径：品牌橙与语义族锁死）。
// 防坏包硬底线：条目名路径穿越拒、条目数/清单大小/总大小上限、全程离线。
// 签名＝内置固定密钥 HMAC-SHA256（完整性/防坏包口径，非对抗性认证——
// 密钥随二进制分发，此边界如实注明）。zip 只写 stored（无压缩）条目：
// 自包含零第三方组件（CRC32 自算、HMAC 用已在链的 OpenSSL::Crypto），
// 非 stored 压缩方式导入按「压缩方式不支持」语义化拒绝。
#pragma once

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QList>
#include <QString>

namespace memex::client {

// 皮肤包清单：包名／semver 版本／作者／基线主题名／中性令牌覆盖表
struct SkinPackManifest {
  QString name;
  QString version;
  QString author;
  QString base;
  QHash<QString, QColor> overrides;

  bool same_as(const SkinPackManifest& o) const {
    return name == o.name && version == o.version && author == o.author &&
           base == o.base && overrides == o.overrides;
  }
};

// 构造＋静态校验（名称/版本格式/作者/基线名/覆盖面）。校验不过返回错误
// 文案（空串＝合法）
QString validate_skin_manifest(const SkinPackManifest& manifest);

// 清单 JSON 解析＋校验（导入链第 3 步；字段缺失/格式错给语义化文案）
SkinPackManifest parse_skin_manifest(const QByteArray& json, QString* error);

// 清单→规范 JSON（导出第 1 步；overrides 按 HexArgb）
QByteArray skin_manifest_json(const SkinPackManifest& manifest);

// 内置固定密钥 HMAC-SHA256 签名（hex 小写）——防篡改防坏包
QByteArray sign_skin_bytes(const QByteArray& manifest_json);

// —— zip 层（stored 条目）——
// 通用写入缝：任意条目列表打成一个 stored zip（导出与测试构造坏包共用）
QByteArray build_skin_zip(const QList<QPair<QString, QByteArray>>& entries,
                          QString* error);
// 导出：清单 JSON＋签名 → 标准皮肤包 zip（manifest.json＋signature.txt）
QByteArray export_skin_pack(const SkinPackManifest& manifest, QString* error);

// 导入结果：ok=false 时 error 为语义化文案（任何失败零副作用）
struct SkinPackImport {
  bool ok = false;
  QString error;
  SkinPackManifest manifest;
};

// 导入链：zip 结构→验签→清单解析→校验。不写盘、不改现有主题。
SkinPackImport import_skin_pack(const QByteArray& zip_bytes);

// semver（X.Y.Z）比较：<0 a 更旧，0 同版，>0 a 更新
int compare_skin_versions(const QString& a, const QString& b);

}  // namespace memex::client
