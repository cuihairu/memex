// test_theme 拆分模块（LAY-011）：本域场景体自原文件逐字搬移；公共面见 common.hpp。
#include "common.hpp"

namespace {
// —— ⑫ 皮肤包体系（⑮）：导出导入往返、先验签再解析、防坏包硬底线、
//    版本比较、ThemeManager 注册与重启往返、设置页两钮路径 ——
QByteArray signed_zip_json(const QByteArray& json) {
  return memex::client::build_skin_zip(
      {{QStringLiteral("manifest.json"), json},
       {QStringLiteral("signature.txt"), memex::client::sign_skin_bytes(json)}},
      nullptr);
}

memex::client::SkinPackManifest sample_manifest() {
  memex::client::SkinPackManifest m;
  m.name = QStringLiteral("晨雾·改·皮肤包");
  m.version = QStringLiteral("1.0.0");
  m.author = QStringLiteral("测试作者");
  m.base = QStringLiteral("light");
  m.overrides.insert(QStringLiteral("surface"), QColor(QStringLiteral("#223344")));
  m.overrides.insert(QStringLiteral("input_bg"), QColor(QStringLiteral("#aabbcc")));
  return m;
}
} // namespace

void test_skin_pack() {
  using memex::client::build_skin_zip;
  using memex::client::export_skin_pack;
  using memex::client::import_skin_pack;
  using memex::client::parse_skin_manifest;
  using memex::client::sign_skin_bytes;
  using memex::client::SkinPackImport;
  using memex::client::SkinPackManifest;

  // —— 静态校验面：包名／版本／基线／覆盖面 ——
  const SkinPackManifest sample = sample_manifest();
  CHECK(memex::client::validate_skin_manifest(sample).isEmpty());
  SkinPackManifest bad = sample;
  bad.name = QStringLiteral("light");
  CHECK(!memex::client::validate_skin_manifest(bad).isEmpty());  // 撞内置
  bad.name = ThemeManager::customThemeName();
  CHECK(!memex::client::validate_skin_manifest(bad).isEmpty());  // 撞自定义槽
  bad.name = QStringLiteral("a/b");
  CHECK(!memex::client::validate_skin_manifest(bad).isEmpty());  // 非法字符
  bad = sample;
  bad.version = QStringLiteral("1.2");
  CHECK(!memex::client::validate_skin_manifest(bad).isEmpty());  // 非 semver
  bad.base = QStringLiteral("不存在");
  CHECK(!memex::client::validate_skin_manifest(bad).isEmpty());
  bad = sample;
  bad.overrides.insert(QStringLiteral("brand"), QColor(QStringLiteral("#ff0000")));
  CHECK(!memex::client::validate_skin_manifest(bad).isEmpty());  // 品牌锁死
  bad.overrides.clear();
  bad.overrides.insert(QStringLiteral("danger"), QColor(QStringLiteral("#0000ff")));
  CHECK(!memex::client::validate_skin_manifest(bad).isEmpty());  // 语义锁死

  // —— 清单 JSON 往返＋缺字段／坏值（签名链之外的清单腿）——
  const QByteArray json = memex::client::skin_manifest_json(sample);
  QString err;
  const SkinPackManifest parsed = parse_skin_manifest(json, &err);
  CHECK(err.isEmpty());
  CHECK(parsed.same_as(sample));
  {
    QJsonDocument doc = QJsonDocument::fromJson(json);
    QJsonObject obj = doc.object();
    obj.remove(QStringLiteral("version"));
    parse_skin_manifest(QJsonDocument(obj).toJson(QJsonDocument::Compact), &err);
    CHECK(err.contains(QStringLiteral("version")));
    obj.insert(QStringLiteral("version"), QStringLiteral("not-semver"));
    parse_skin_manifest(QJsonDocument(obj).toJson(QJsonDocument::Compact), &err);
    CHECK(err.contains(QStringLiteral("semver")));
    obj.insert(QStringLiteral("version"), sample.version);
    obj.insert(QStringLiteral("overrides"),
               QJsonObject{{QStringLiteral("surface"),
                            QStringLiteral("#12")}});
    parse_skin_manifest(QJsonDocument(obj).toJson(QJsonDocument::Compact), &err);
    CHECK(err.contains(QStringLiteral("HexArgb")));
  }

  // —— 导出→导入往返一致（含中文名与多枚覆盖）——
  QString export_err;
  const QByteArray zip = export_skin_pack(sample, &export_err);
  CHECK(export_err.isEmpty());
  CHECK(!zip.isEmpty());
  const SkinPackImport round = import_skin_pack(zip);
  CHECK(round.ok);
  CHECK(round.error.isEmpty());
  CHECK(round.manifest.same_as(sample));

  // —— 篡改一字节拒（在 CRC 结构层即拒；签名层语义由重打包错签腿覆盖）——
  {
    QByteArray tampered = zip;
    const int json_pos = tampered.indexOf(json);
    CHECK(json_pos > 0);
    tampered[json_pos + 2] =
        static_cast<char>(quint8(tampered[json_pos + 2]) ^ 0x01);
    const SkinPackImport bad_pack = import_skin_pack(tampered);
    CHECK(!bad_pack.ok);
    CHECK(!bad_pack.error.isEmpty());
  }

  // —— 先验签再解析：缺字段＋错签名 → 报签名而非缺字段 ——
  {
    QJsonDocument doc = QJsonDocument::fromJson(json);
    QJsonObject obj = doc.object();
    obj.remove(QStringLiteral("version"));
    const QByteArray bad_json =
        QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QList<QPair<QString, QByteArray>> entries;
    entries.append({QStringLiteral("manifest.json"), bad_json});
    entries.append({QStringLiteral("signature.txt"),
                    QStringLiteral("deadbeef").toLatin1()});
    const SkinPackImport bad_pack = import_skin_pack(build_skin_zip(entries, nullptr));
    CHECK(!bad_pack.ok);
    CHECK(bad_pack.error.contains(QStringLiteral("签名")));
    // 同包＋正确签名 → 报缺字段（验签过了才轮到清单）
    const SkinPackImport missing = import_skin_pack(signed_zip_json(bad_json));
    CHECK(!missing.ok);
    CHECK(missing.error.contains(QStringLiteral("version")));
  }

  // —— format 字段不符（外来 JSON）拒 ——
  {
    QJsonDocument doc = QJsonDocument::fromJson(json);
    QJsonObject obj = doc.object();
    obj.remove(QStringLiteral("format"));
    const SkinPackImport foreign =
        import_skin_pack(signed_zip_json(QJsonDocument(obj).toJson(
            QJsonDocument::Compact)));
    CHECK(!foreign.ok);
    CHECK(foreign.error.contains(QStringLiteral("memex")));
  }

  // —— 防坏包硬底线 ——
  {
    // 缺 manifest.json / 缺 signature.txt
    QList<QPair<QString, QByteArray>> only_sig;
    only_sig.append({QStringLiteral("signature.txt"), sign_skin_bytes(json)});
    CHECK(!import_skin_pack(build_skin_zip(only_sig, nullptr)).ok);
    QList<QPair<QString, QByteArray>> only_manifest;
    only_manifest.append({QStringLiteral("manifest.json"), json});
    const SkinPackImport no_sig = import_skin_pack(build_skin_zip(only_manifest, nullptr));
    CHECK(!no_sig.ok);
    CHECK(no_sig.error.contains(QStringLiteral("signature")));
    // 路径穿越／绝对路径条目名
    QList<QPair<QString, QByteArray>> traversal;
    traversal.append({QStringLiteral("../evil/manifest.json"), json});
    traversal.append({QStringLiteral("signature.txt"), sign_skin_bytes(json)});
    const SkinPackImport evil = import_skin_pack(build_skin_zip(traversal, nullptr));
    CHECK(!evil.ok);
    CHECK(evil.error.contains(QStringLiteral("路径")));
    QList<QPair<QString, QByteArray>> absolute;
    absolute.append({QStringLiteral("/etc/manifest.json"), json});
    absolute.append({QStringLiteral("signature.txt"), sign_skin_bytes(json)});
    CHECK(!import_skin_pack(build_skin_zip(absolute, nullptr)).ok);
    // 条目数超限（40 条）
    QList<QPair<QString, QByteArray>> many;
    for (int i = 0; i < 40; ++i) {
      many.append({QStringLiteral("extra%1.bin").arg(i), QByteArray(8, 'x')});
    }
    many.append({QStringLiteral("manifest.json"), json});
    many.append({QStringLiteral("signature.txt"), sign_skin_bytes(json)});
    const SkinPackImport overflow = import_skin_pack(build_skin_zip(many, nullptr));
    CHECK(!overflow.ok);
    CHECK(overflow.error.contains(QStringLiteral("条目数")));
    // 单条过大（2 MiB）
    QList<QPair<QString, QByteArray>> huge;
    huge.append({QStringLiteral("manifest.json"),
                 QByteArray(2 * 1024 * 1024, 'x')});
    const SkinPackImport big = import_skin_pack(build_skin_zip(huge, nullptr));
    CHECK(!big.ok);
    CHECK(big.error.contains(QStringLiteral("单条")));
    // 内容总量超限（5 × 900 KiB > 4 MiB）
    QList<QPair<QString, QByteArray>> heavy;
    for (int i = 0; i < 5; ++i) {
      heavy.append({QStringLiteral("extra%1.bin").arg(i),
                    QByteArray(900 * 1024, 'x')});
    }
    heavy.append({QStringLiteral("manifest.json"), json});
    heavy.append({QStringLiteral("signature.txt"), sign_skin_bytes(json)});
    const SkinPackImport fat_import =
        import_skin_pack(build_skin_zip(heavy, nullptr));
    CHECK(!fat_import.ok);
    CHECK(fat_import.error.contains(QStringLiteral("总量")));
    // 清单过大（300 KiB，签名合法→在清单大小底线拒）
    const QByteArray big_json(300 * 1024, 'a');
    const SkinPackImport big_manifest = import_skin_pack(signed_zip_json(big_json));
    CHECK(!big_manifest.ok);
    CHECK(big_manifest.error.contains(QStringLiteral("清单过大")));
    // 非 stored 压缩方式（字节手术改 method 字段）
    QByteArray deflated = zip;
    const int cd = deflated.indexOf(QByteArrayLiteral("\x50\x4b\x01\x02"));
    const int local = deflated.indexOf(QByteArrayLiteral("\x50\x4b\x03\x04"));
    CHECK(cd >= 0 && local >= 0);
    deflated[cd + 10] = 0x08;
    deflated[local + 8] = 0x08;
    const SkinPackImport compressed = import_skin_pack(deflated);
    CHECK(!compressed.ok);
    CHECK(compressed.error.contains(QStringLiteral("压缩")));
    // 非 zip 字节
    CHECK(!import_skin_pack(QByteArrayLiteral("not a zip at all")).ok);
  }

  // —— semver 比较 ——
  CHECK(memex::client::compare_skin_versions(QStringLiteral("1.2.3"),
                                             QStringLiteral("1.10.0")) < 0);
  CHECK(memex::client::compare_skin_versions(QStringLiteral("2.0.0"),
                                             QStringLiteral("1.99.99")) > 0);
  CHECK(memex::client::compare_skin_versions(QStringLiteral("1.2.3"),
                                             QStringLiteral("1.2.3")) == 0);

  // —— ThemeManager 安装流：注册＋选中＋令牌生效＋版本比较 ——
  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp.path());

  ThemeManager manager;
  manager.set_system_dark_probe([] { return false; });
  manager.set_mode(QString::fromUtf8(ThemeManager::kLight));

  QString install_err;
  CHECK(manager.install_skin_pack(zip, &install_err));
  CHECK(install_err.isEmpty());
  CHECK(manager.mode() == sample.name);  // 导入成功自动选中
  CHECK(manager.has_theme(sample.name));
  CHECK(manager.skin_pack_themes().contains(sample.name));
  CHECK(manager.tokens().surface.name() == QStringLiteral("#223344"));
  CHECK(manager.tokens().input_bg.name() == QStringLiteral("#aabbcc"));
  CHECK(manager.tokens().text.name() ==
        ThemeManager::tokens_for(QStringLiteral("light")).text.name());
  CHECK(manager.tokens().brand.name() == QStringLiteral("#e16531"));  // 品牌恒同
  CHECK(manager.tokens().is_complete());

  // 版本比较：同版本／低版本拒（如实注明语义＝须严格更高），高版本替换
  {
    QString err2;
    CHECK(!manager.install_skin_pack(zip, &err2));  // 同版本
    CHECK(err2.contains(QStringLiteral("更高")));
    SkinPackManifest older = sample;
    older.version = QStringLiteral("0.9.0");
    CHECK(!manager.install_skin_pack(export_skin_pack(older, nullptr), &err2));
    CHECK(err2.contains(QStringLiteral("更高")));
    SkinPackManifest newer = sample;
    newer.version = QStringLiteral("1.1.0");
    newer.overrides.insert(QStringLiteral("surface"),
                           QColor(QStringLiteral("#445566")));
    CHECK(manager.install_skin_pack(export_skin_pack(newer, nullptr), &err2));
    CHECK(manager.tokens().surface.name() == QStringLiteral("#445566"));
  }

  // 撞非包扩展主题：注册表占用拒绝，不覆盖
  {
    ThemeTokens foreign_tokens;
    foreign_tokens.surface = QColor(QStringLiteral("#123123"));
    manager.register_theme(QStringLiteral("外部扩展"), foreign_tokens);
    SkinPackManifest clash = sample;
    clash.name = QStringLiteral("外部扩展");
    clash.version = QStringLiteral("9.0.0");
    QString err3;
    CHECK(!manager.install_skin_pack(export_skin_pack(clash, nullptr), &err3));
    CHECK(err3.contains(QStringLiteral("占用")));
  }

  // —— 导出缝：皮肤包原样；内置「·皮肤」；自定义槽「·改色」；不可导出腿 ——
  {
    const SkinPackManifest current = manager.current_skin_manifest();
    // 皮肤包主题原样导出（此刻已是 1.1.0 替换装）
    CHECK(current.name == sample.name);
    CHECK(current.version == QStringLiteral("1.1.0"));
    CHECK(current.overrides.value(QStringLiteral("surface")) ==
          QColor(QStringLiteral("#445566")));
    // 内置主题：「<名>·皮肤」无覆盖；导出包可再导入并注册
    manager.set_mode(QString::fromUtf8(ThemeManager::kLight));
    const SkinPackManifest builtin_out = manager.current_skin_manifest();
    CHECK(builtin_out.name == QStringLiteral("light·皮肤"));
    CHECK(builtin_out.base == QStringLiteral("light"));
    CHECK(builtin_out.overrides.isEmpty());
    const QByteArray builtin_zip =
        export_skin_pack(builtin_out, nullptr);
    const SkinPackImport reimport = import_skin_pack(builtin_zip);
    CHECK(reimport.ok);
    CHECK(reimport.manifest.same_as(builtin_out));
    QString install_err4;
    CHECK(manager.install_skin_pack(builtin_zip, &install_err4));
    CHECK(manager.mode() == QStringLiteral("light·皮肤"));
    CHECK(manager.tokens().surface.name() ==
          ThemeManager::tokens_for(QStringLiteral("light")).surface.name());
    // 自定义槽：先回内置基线再改色（基线为皮肤包时导出按口径拒绝，
    // 不在本腿覆盖）；带覆盖表导出并往返
    manager.set_mode(QString::fromUtf8(ThemeManager::kLight));
    CHECK(manager.set_custom_override(QStringLiteral("surface"),
                                      QColor(QStringLiteral("#010203"))));
    const SkinPackManifest custom_out = manager.current_skin_manifest();
    CHECK(custom_out.name == QStringLiteral("light·改色"));
    CHECK(custom_out.base == QStringLiteral("light"));
    CHECK(custom_out.overrides.value(QStringLiteral("surface")) ==
          QColor(QStringLiteral("#010203")));
    const SkinPackImport custom_re =
        import_skin_pack(export_skin_pack(custom_out, nullptr));
    CHECK(custom_re.ok);
    CHECK(custom_re.manifest.same_as(custom_out));
    // 不可导出腿：非包扩展主题
    manager.set_mode(QStringLiteral("外部扩展"));
    QString export_error;
    CHECK(manager.current_skin_manifest(&export_error).name.isEmpty());
    CHECK(!export_error.isEmpty());
    manager.set_mode(sample.name);  // 收尾回到包主题：重启腿从持久化选中态读起
  }

  // —— 重启往返：已装包重注册＋选中持久化；坏包残留跳过不阻断 ——
  {
    QSettings writer;
    writer.setValue(QStringLiteral("appearance/skins/坏包/manifest"),
                    QStringLiteral("{not json"));
    ThemeManager m2;
    m2.set_system_dark_probe([] { return false; });
    CHECK(m2.mode() == sample.name);  // 版本 1.1.0 已是选中态
    CHECK(m2.skin_pack_themes().contains(sample.name));
    CHECK(m2.tokens().surface.name() == QStringLiteral("#445566"));
    CHECK(!m2.skin_pack_themes().contains(QStringLiteral("坏包")));
    CHECK(m2.has_theme(QStringLiteral("light·皮肤")));  // 首包同批重注册
  }

  // —— 设置页两钮路径（免对话框缝）＋「（皮肤包）」标注＋目录记忆 ——
  // 隔离口径：ThemeManager 落盘走 QSettings(org,app)＝NativeFormat（全 app
  // 统一，setDefaultFormat/setPath(INI) 只影响 QSettings() 默认构造）——
  // 真隔离＝清同一 conf
  {
    QTemporaryDir ui_tmp;
    CHECK(ui_tmp.isValid());
    QSettings(QCoreApplication::organizationName(),
              QCoreApplication::applicationName())
        .clear();
    ThemeManager ui_manager;
    ui_manager.set_system_dark_probe([] { return false; });
    ThemeSettingsPage page(&ui_manager);
    QPushButton* import_btn =
        page.findChild<QPushButton*>(QStringLiteral("importSkinButton"));
    QPushButton* export_btn =
        page.findChild<QPushButton*>(QStringLiteral("exportSkinButton"));
    QLabel* status = page.findChild<QLabel*>(QStringLiteral("skinStatus"));
    CHECK(import_btn && export_btn && status);
    CHECK(status->text().isEmpty());

    // 导入：好包→成功状态＋自动选中＋列表项带「（皮肤包）」
    const QString pack_path =
        ui_tmp.filePath(QStringLiteral("pack.zip"));
    QFile pack_file(pack_path);
    CHECK(pack_file.open(QIODevice::WriteOnly));
    pack_file.write(zip);
    pack_file.close();
    CHECK(page.import_skin_from_path(pack_path));
    CHECK(status->text().contains(QStringLiteral("成功")));
    CHECK(ui_manager.mode() == sample.name);
    QListWidget* list = page.findChild<QListWidget*>(QStringLiteral("themeModes"));
    CHECK(list);
    bool labeled = false;
    for (int i = 0; i < list->count(); ++i) {
      if (list->item(i)->data(Qt::UserRole).toString() == sample.name) {
        CHECK(list->item(i)->text().contains(QStringLiteral("皮肤包")));
        labeled = true;
      }
    }
    CHECK(labeled);
    // 目录记忆落盘（app_settings 同口径：QSettings(org,app)＝NativeFormat）
    QSettings memory(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
    CHECK(memory.value(QStringLiteral("ui/skin_pack_dir")).toString() ==
          QFileInfo(pack_path).absolutePath());

    // 导入：篡改包→语义化拒绝＋模式不变（单字节翻转在 CRC 层拒）
    {
      QByteArray tampered = zip;
      const int json_pos = tampered.indexOf(json);
      tampered[json_pos + 4] =
          static_cast<char>(quint8(tampered[json_pos + 4]) ^ 0x02);
      const QString bad_path = ui_tmp.filePath(QStringLiteral("bad.zip"));
      QFile bad_file(bad_path);
      CHECK(bad_file.open(QIODevice::WriteOnly));
      bad_file.write(tampered);
      bad_file.close();
      CHECK(!page.import_skin_from_path(bad_path));
      CHECK(!status->text().isEmpty());
      CHECK(ui_manager.mode() == sample.name);
    }

    // 导出：皮肤包原样落文件，可再导入往返
    const QString out_path = ui_tmp.filePath(QStringLiteral("out.zip"));
    CHECK(page.export_skin_to_path(out_path));
    CHECK(status->text().contains(QStringLiteral("已导出")));
    CHECK(QFile(out_path).size() > 0);
    QFile back_file(out_path);
    CHECK(back_file.open(QIODevice::ReadOnly));
    const SkinPackImport back = import_skin_pack(back_file.readAll());
    CHECK(back.ok);
    CHECK(back.manifest.same_as(sample));

    // 内置导出腿（选中 light）：文件可再导入注册「light·皮肤」
    ui_manager.set_mode(QString::fromUtf8(ThemeManager::kLight));
    CHECK(ui_manager.mode() == QString::fromUtf8(ThemeManager::kLight));
    const QString light_out = ui_tmp.filePath(QStringLiteral("light.zip"));
    CHECK(page.export_skin_to_path(light_out));
    CHECK(QFile(light_out).size() > 0);
  }
}
