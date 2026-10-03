#!/usr/bin/env bash
# macOS 每日构建产物打包（Apple Silicon / arm64）。
# 用法：
#   package-macos.sh client <Memex.app 路径> <输出目录> [版本x.y.z]
#   package-macos.sh server <memex_server 二进制路径> <输出目录> [版本x.y.z]
# 产物（固定资产名，install.sh 直链取件）：
#   client → memex-client-macos-arm64.dmg（内含 Memex.app，拖入 /Applications 即用）
#   server → memex-server-macos-arm64.dmg（内含 memex-server.pkg，双击安装到
#            /usr/local/bin/memex-server——pkg 形态满足系统「引导安装」口径）
# 依赖：macdeployqt（官方 Qt 树 $QT_ROOT_DIR/bin，由 install-qt-action 提供）、
#       hdiutil / codesign / pkgbuild（系统自带）。ad-hoc 签名（无开发者证书，
#       Gatekeeper 见随包说明的 xattr 口径）。
set -euo pipefail

COMPONENT="$1"
SRC="$2"
OUT="$3"
VERSION="${4:-0.1.0}"
ARCH=arm64
ASSET="memex-$COMPONENT-macos-$ARCH"

case "$COMPONENT" in client|server) ;; *)
  echo "错误：用法 package-macos.sh <client|server> <源路径> <输出目录> [版本]" >&2
  exit 2 ;;
esac
mkdir -p "$OUT"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

cat > "$STAGE/使用说明.txt" <<EOF
Memex ${COMPONENT}（macOS arm64，每日构建）

- 版本查看：
$( [ "$COMPONENT" = client ] && echo '  /Applications/Memex.app/Contents/MacOS/Memex --version' || echo '  memex-server --version（安装后）' )
- 未做开发者证书签名（ad-hoc）：下载的 dmg/app 带 Gatekeeper 隔离属性，
  首次打开前请执行：
    xattr -cr /Applications/Memex.app
EOF

if [ "$COMPONENT" = client ]; then
  [ -d "$SRC" ] || { echo "错误：未找到 .app：$SRC" >&2; exit 2; }

  # Qt 运行库进 bundle（官方 Qt 树的 macdeployqt；vcpkg 侧依赖为静态链入）
  MACDEPLOYQT="${QT_ROOT_DIR:-}/bin/macdeployqt"
  [ -x "$MACDEPLOYQT" ] || MACDEPLOYQT="$(command -v macdeployqt || true)"
  [ -n "$MACDEPLOYQT" ] || { echo "错误：未找到 macdeployqt（QT_ROOT_DIR 未设或不在 PATH）" >&2; exit 1; }
  "$MACDEPLOYQT" "$SRC" -always-overwrite

  # ad-hoc 签名：无证书环境下让 bundle 自洽（校验可过，Gatekeeper 仍走 xattr 口径）
  codesign --force --deep --sign - "$SRC"
  codesign -vv "$SRC"

  DMGROOT="$STAGE/dmg"
  mkdir -p "$DMGROOT"
  cp -R "$SRC" "$DMGROOT/"
  cp "$STAGE/使用说明.txt" "$DMGROOT/"
  hdiutil create -volname "Memex" -srcfolder "$DMGROOT" \
    -format UDZO -ov "$OUT/$ASSET.dmg" >/dev/null
  echo "$ASSET.dmg"
  exit 0
fi

# —— server：pkg（/usr/local/bin/memex-server）再装进 dmg ——
[ -f "$SRC" ] || { echo "错误：未找到二进制：$SRC" >&2; exit 2; }
PKGROOT="$STAGE/pkgroot"
mkdir -p "$PKGROOT/usr/local/bin"
cp "$SRC" "$PKGROOT/usr/local/bin/memex-server"
pkgbuild --root "$PKGROOT" \
  --identifier site.cuihairu.memex.server \
  --version "$VERSION" \
  --install-location / \
  "$STAGE/$ASSET.pkg" >/dev/null

DMGROOT="$STAGE/dmg"
mkdir -p "$DMGROOT"
cp "$STAGE/$ASSET.pkg" "$DMGROOT/"
cp "$STAGE/使用说明.txt" "$DMGROOT/"
hdiutil create -volname "Memex Server" -srcfolder "$DMGROOT" \
  -format UDZO -ov "$OUT/$ASSET.dmg" >/dev/null
echo "$ASSET.dmg"
