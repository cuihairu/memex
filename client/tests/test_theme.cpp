// T4.9 主题切换验收（R19）＋需求批⑬ 默认 20 套：
// ① 令牌完备性与品牌橙恒定、亮暗两套关键面明显不同；
// ② 跟随系统解析：系统亮暗 → light／dark，手动选择覆盖系统；
// ③ 持久化往返：手动选择落盘，新实例读回（重启后保持）；
// ④ 令牌化 QSS 生成：占位符零残留、关键选择器齐、切换后样式变化（即时生效）；
// ⑤ 多主题扩展位：只注册一套令牌即可被选中／解析／落盘；
// ⑥ 默认 20 套（⑬）：22 套 builtin 全部令牌/排版完备、对比度门槛过、
//    品牌橙恒同、底色互异、字体/密度成套、tokens_for 出真令牌、
//    选择持久化往返、离屏渲染像素差（另可 MEMEX_THEME_SHOT_DIR 落实截）；
// ⑦ 应用级落地：调色板色与令牌一致、系统亮暗变化仅在跟随模式下重应用；
// ⑧ 设置页交互：列表点选暗色／⑬ 默认主题／跟随系统即时生效并回灌选中态；
//    ⑭ 面板改色（改色→自定义槽＋恢复默认）与毛玻璃开关（降级提示）；
// ⑨ 主窗接线：控件样式里的颜色全部来自令牌（零字面量）、主题切换后
//    控件样式与聊天区富文本同步重渲、「设置 → 主题…」菜单入口存在；
// ⑩ 自定义主题（⑭）：8 枚中性令牌放行面、品牌／语义拒绝、基线锁定＋
//    自动切槽、重启往返、恢复默认回基线、覆盖层丢失回落、纵深防御；
// ⑪ 毛玻璃特效（⑭）：默认关、开↔关落盘往返、不支持平台降级（生效态恒
//    false）、支持腿探针注入、apply 后不崩；
// ⑫ 皮肤包体系（⑮）：清单校验面、导出导入往返、篡改一字节拒、先验签再
//    解析（缺字段＋错签名报签名）、format 不符拒、防坏包硬底线（缺条目/
//    路径穿越/条目数/单条/总量/清单大小/压缩方式/非 zip）、semver 比较、
//    ThemeManager 安装流（自动选中＋令牌生效＋品牌恒同）、版本比较（同版
//    拒/低版拒/高版替换）、撞名拒、导出缝（包原样/内置·皮肤/自定义·改色/
//    不可导出腿）、重启重注册（坏包残留跳过）、设置页两钮路径（成功/篡改
//    拒/「（皮肤包）」标注/目录记忆/内置导出腿）。

#include "theme/common.hpp"

int main(int argc, char** argv) {
  QTemporaryDir tmp;
  if (!tmp.isValid()) return 1;
  qputenv("XDG_DATA_HOME", tmp.filePath(QStringLiteral("xdg")).toUtf8());
  qputenv("XDG_CONFIG_HOME",
          tmp.filePath(QStringLiteral("xdg-config")).toUtf8());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     tmp.filePath(QStringLiteral("settings")).toUtf8());

  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("memex-test"));
  QCoreApplication::setApplicationName(QStringLiteral("theme-test"));

  test_tokens();
  test_system_follow();
  test_persistence();
  test_stylesheet();
  test_theme_extension();
  test_default_themes();
  test_apply_to_app(app);
  test_settings_page(app);
  test_main_window_wiring(app);
  test_custom_theme();
  test_frosted_effect(app);
  test_skin_pack();

  if (g_failures == 0) {
    qInfo("test_theme: ALL PASS");
    return 0;
  }
  qCritical("test_theme: %d FAILURES", g_failures);
  return 1;
}
