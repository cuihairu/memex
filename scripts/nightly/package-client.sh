#!/usr/bin/env bash
# Qt 桌面客户端每日构建产物打包：Linux x86-64 压缩包，随包 Qt 运行库与插件。
# 布局：memex-client（二进制）+ lib/（递归收集的动态依赖）+ plugins/（Qt 插件）
#       + memex-client.sh（入口脚本，LD_LIBRARY_PATH / QT_PLUGIN_PATH 指向随包内容）。
# 用法：package-client.sh <memex_client 二进制路径> <输出目录> <日期YYYYMMDD> <短SHA>
set -euo pipefail

BIN="$1"
OUT="$2"
DATE="$3"
SHA="$4"

NAME="memex-client-linux-x64-${DATE}-${SHA}"

# 暂存独立于调用目录（mktemp + 退出清理），避免旧暂存物混入产物
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
PKG="$STAGE/$NAME"

mkdir -p "$PKG/lib"
cp "$BIN" "$PKG/memex-client"

# 递归收集动态依赖；glibc 核心运行时不随包（目标机必备，随包反而冲突）。
declare -A visited=()
queue=("$BIN")
while [ ${#queue[@]} -gt 0 ]; do
  cur="${queue[0]}"
  queue=("${queue[@]:1}")
  while read -r dep; do
    [ -n "$dep" ] || continue
    case "$dep" in
      */ld-linux-*|*/libc.so.*|*/libm.so.*|*/libpthread*|*/libdl*|*/librt.so.*) continue ;;
    esac
    [ -n "${visited[$dep]:-}" ] && continue
    visited[$dep]=1
    cp -L "$dep" "$PKG/lib/"
    while read -r d2; do
      [ -n "${visited[$d2]:-}" ] || queue+=("$d2")
    done < <(ldd "$dep" 2>/dev/null | awk '$3 ~ /^\// {print $3}')
  done < <(ldd "$cur" 2>/dev/null | awk '$3 ~ /^\// {print $3}')
done

# Qt 插件：平台（xcb/offscreen）、SQLite 驱动、图像格式、控件样式、图标引擎。
QT_PLUGINS_DIR="$(qmake6 -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
if [ -z "$QT_PLUGINS_DIR" ] || [ ! -d "$QT_PLUGINS_DIR" ]; then
  for cand in /usr/lib/x86_64-linux-gnu/qt6/plugins /usr/lib/qt6/plugins; do
    if [ -d "$cand" ]; then QT_PLUGINS_DIR="$cand"; break; fi
  done
fi
if [ -n "$QT_PLUGINS_DIR" ] && [ -d "$QT_PLUGINS_DIR" ]; then
  mkdir -p "$PKG/plugins"
  for sub in platforms sqldrivers imageformats styles iconengines; do
    if [ -d "$QT_PLUGINS_DIR/$sub" ]; then
      cp -r "$QT_PLUGINS_DIR/$sub" "$PKG/plugins/"
    fi
  done
else
  echo "警告：未找到 Qt 插件目录，随包插件缺失" >&2
fi

cat > "$PKG/memex-client.sh" <<'EOF'
#!/bin/sh
# Memex 客户端入口：优先使用随包运行库与插件
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$DIR/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
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
