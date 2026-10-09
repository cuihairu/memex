#!/usr/bin/env bash
# T6.2 鸿蒙（HarmonyOS NEXT，Qt 桌面矩阵项）客户端：OHOS 交叉构建 + HAP 打包驱动。
# 只写代码不验证口径（todo.md 卷首注）：本脚本未在真机/无 SDK 环境跑通，属
# 「待真机验证」；外部依赖卡点（DevEco Studio、签名证书、真机）见
# packaging/ohos/README.md。
#
# 环境要求（仓库外，均需 export）：
#   OHOS_SDK_ROOT  OpenHarmony/HarmonyOS SDK 根目录（native/build/cmake/
#                  ohos.toolchain.cmake 必在；vcpkg arm64-ohos triplet 亦读此变量）
#   QT_OHOS_ROOT   Qt for OpenHarmony 安装前缀（Qt 6.12+ 官方鸿蒙目标包，
#                  lib/cmake/Qt6 必在；官方 Qt 无鸿蒙预编译包时须按
#                  doc.qt.io qt-6.12 harmonyos-building 自行交叉编译 Qt）
#   QT_HOST_PATH   同版本桌面宿主 Qt 前缀（交叉 moc/rcc 等 host 工具；预编译
#                  鸿蒙 Qt 已内嵌 host 路径时可省）
#
# 产出：build-client-ohos/client/ 下客户端产物与 <target>_make_hap 经
# harmonydeployqt → hvigor 出的 HAP（Qt 6.12 口径，technology preview）。
# 签名需外部证书（hap-sign-tool / DevEco），未签名 HAP 无法真机直装。
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

: "${OHOS_SDK_ROOT:?需先 export OHOS_SDK_ROOT=<OpenHarmony SDK 根目录>}"
: "${QT_OHOS_ROOT:?需先 export QT_OHOS_ROOT=<Qt for OpenHarmony 安装前缀>}"
[ -f "$OHOS_SDK_ROOT/native/build/cmake/ohos.toolchain.cmake" ] || {
  echo "OHOS_SDK_ROOT 下无 native/build/cmake/ohos.toolchain.cmake：$OHOS_SDK_ROOT" >&2
  exit 1
}
[ -f "$QT_OHOS_ROOT/lib/cmake/Qt6/Qt6Config.cmake" ] || {
  echo "QT_OHOS_ROOT 下无 lib/cmake/Qt6/Qt6Config.cmake（应为鸿蒙目标 Qt）：$QT_OHOS_ROOT" >&2
  exit 1
}

cmake -S "$REPO" --preset client-release-ohos
cmake --build "$REPO/build-client-ohos" --target memex_client

# HAP 打包：Qt 6.12 起目标平台 OHOS 自动注册 make_hap 目标（harmonydeployqt
# 收集客户端 .so + libqohos.so 平台插件 + Qt 模块 → hvigor 组装）。
cmake --build "$REPO/build-client-ohos" --target memex_client_make_hap

echo
echo "HAP 已产出：在 $REPO/build-client-ohos 下查 harmonydeployqt 输出目录（*.hap）。"
echo "真机安装前须签名（应用签名证书为外部卡点，见 packaging/ohos/README.md）。"
