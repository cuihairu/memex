#!/usr/bin/env bash
# Qt 桌面客户端每日构建产物打包：Linux x86-64 压缩包，随包 Qt 运行库与插件。
# 布局：memex-client（二进制）+ lib/（递归收集的动态依赖）+ plugins/（Qt 插件）
#       + memex-client.sh（入口脚本，LD_LIBRARY_PATH / QT_PLUGIN_PATH 指向随包内容）。
# 用法：package-client.sh <memex_client 二进制路径> <输出目录> <日期YYYYMMDD> <短SHA> [版本x.y.z]
# 资产名口径（A23）：MemexClient-x.y.z-linux-x64-<日期>-<短SHA>.tar.gz。
# 版本未传时取 0.1.0（与根 CMakeLists PROJECT_VERSION 同源，nightly 显式传入）。
set -euo pipefail

BIN="$1"
OUT="$2"
DATE="$3"
SHA="$4"
VERSION="${5:-0.1.0}"

NAME="MemexClient-${VERSION}-linux-x64-${DATE}-${SHA}"

# 暂存独立于调用目录（mktemp + 退出清理），避免旧暂存物混入产物
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
PKG="$STAGE/$NAME"

mkdir -p "$PKG/lib"
cp "$BIN" "$PKG/memex-client"

# Qt 插件：平台（xcb/offscreen）、SQLite 驱动、图像格式、控件样式、图标引擎。
# 来源必须与构建所用 Qt 同源（vcpkg 安装树，x64-linux-dynamic triplet）——
# 系统 Qt 插件混入会因版本错位崩溃。
TRIPLET="${VCPKG_TARGET_TRIPLET:-x64-linux-dynamic}"
REAL_BIN="$(readlink -f "$BIN")"
for cand in "${VCPKG_INSTALLED_DIR:-}/$TRIPLET/Qt6/plugins" \
            "$(cd "$(dirname "$REAL_BIN")/../.." && pwd)/vcpkg_installed/$TRIPLET/Qt6/plugins"; do
  if [ -n "$cand" ] && [ -d "$cand" ]; then QT_PLUGINS_DIR="$cand"; break; fi
done
if [ -z "${QT_PLUGINS_DIR:-}" ]; then
  echo "错误：未找到 vcpkg Qt 插件目录（vcpkg_installed/$TRIPLET/Qt6/plugins），拒绝出包" >&2
  exit 1
fi
mkdir -p "$PKG/plugins"
for sub in platforms sqldrivers imageformats styles iconengines; do
  if [ -d "$QT_PLUGINS_DIR/$sub" ]; then
    cp -r "$QT_PLUGINS_DIR/$sub" "$PKG/plugins/"
  fi
done

# Qt 运行库所在目录（vcpkg_installed/<triplet>/lib，与插件目录同树推导）：
# 收集阶段的 ldd 解析链必须带上它。插件自身的 RUNPATH（$ORIGIN/../../../lib）
# 按 Qt 上游安装布局算，在本包布局下指向包外——不带 vcpkg 树则 libQt6XcbQpa
# 等"主二进制不直接链接"的 Qt 库会落到系统缓存（混入异构 Qt，启动即崩）。
VCPKG_LIB="$QT_PLUGINS_DIR/../../lib"   # <root>/vcpkg_installed/<triplet>/lib
[ -d "$VCPKG_LIB" ] || VCPKG_LIB=""

# 递归收集动态依赖；Qt 插件由 dlopen 加载，其依赖树必须一并随包，
# 否则目标机上 xcb 等平台插件起不来。解析时把包内 lib/ 与 vcpkg 安装树
# 前置到 LD_LIBRARY_PATH——只留包内 lib/ 不够：处理插件时其 Qt 依赖尚未
# 全部入包，会命中系统 Qt 并覆盖随包版本（版本错位即崩）。
# glibc 核心运行时不随包（目标机必备，随包反而冲突）。
RESOLVE_PATH="$PKG/lib${VCPKG_LIB:+:$VCPKG_LIB}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
declare -A visited=()
queue=("$BIN")
for plug in "$PKG"/plugins/*/*.so; do
  [ -f "$plug" ] && queue+=("$plug")
done
while [ ${#queue[@]} -gt 0 ]; do
  cur="${queue[0]}"
  queue=("${queue[@]:1}")
  while read -r dep; do
    [ -n "$dep" ] || continue
    case "$dep" in
      */ld-linux-*|*/libc.so.*|*/libm.so.*|*/libpthread*|*/libdl*|*/librt.so.*) continue ;;
      "$PKG"/lib/*) continue ;;  # 解析已命中包内库，其依赖首入时已收集
    esac
    [ -n "${visited[$dep]:-}" ] && continue
    visited[$dep]=1
    cp -L "$dep" "$PKG/lib/"
    while read -r d2; do
      [ -n "${visited[$d2]:-}" ] || queue+=("$d2")
    done < <(LD_LIBRARY_PATH="$RESOLVE_PATH" \
             ldd "$dep" 2>/dev/null | awk '$3 ~ /^\// {print $3}')
  done < <(LD_LIBRARY_PATH="$RESOLVE_PATH" \
           ldd "$cur" 2>/dev/null | awk '$3 ~ /^\// {print $3}')
done

cat > "$PKG/memex-client.sh" <<'EOF'
#!/bin/sh
# Memex 客户端入口：优先使用随包运行库与插件
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$DIR/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
# fontconfig 缓存隔离到包内：随包 fontconfig 与用户机既有缓存可能格式错位
# （错位会在文字排版路径直接崩进程），隔离后首次启动自建缓存
export XDG_CACHE_HOME="$DIR/cache"
mkdir -p "$XDG_CACHE_HOME" 2>/dev/null || true
exec "$DIR/memex-client" "$@"
EOF
chmod +x "$PKG/memex-client.sh"

cat > "$PKG/使用说明.txt" <<'EOF'
Memex Qt 桌面客户端（Linux x86-64，每日构建，随包 Qt 运行库）

- 启动：./memex-client.sh
  入口脚本自动加载随包运行库与插件，桌面环境直接运行。
- 冒烟自检（无界面环境）：
  QT_QPA_PLATFORM=offscreen ./memex-client.sh --smoke
- 查看版本：./memex-client --version

安装即用进入直连态（UDP 2425 发现 / TCP 2426-2437 点对点，仅本机留档）；
填服务器地址登录后进入协作态（全量归档、可检索）。
EOF

tar -czf "$OUT/$NAME.tar.gz" -C "$STAGE" "$NAME"
echo "$NAME.tar.gz"
