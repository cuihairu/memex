#!/usr/bin/env bash
# 服务端每日构建产物打包：Linux x86-64 二进制压缩包，随包非系统动态依赖
#（protobuf/crypto/sqlite3 等经 vcpkg 动态链入，裸二进制在无构建树的目标机
# 上起不来——RPATH 指向构建机 vcpkg_installed 树）。
# 布局：memex-server（二进制）+ lib/（递归收集的非系统依赖）
#       + memex-server.sh（入口脚本，LD_LIBRARY_PATH 指向随包 lib）。
# 用法：package-server.sh <memex_server 二进制路径> <输出目录> <日期YYYYMMDD> <短SHA>
# 输出：<输出目录>/memex-server-linux-x64-<日期>-<短SHA>.tar.gz，资产名打印到 stdout。
# glibc 核心运行时不随包（目标机必备，随包反而冲突）。
set -euo pipefail

BIN="$1"
OUT="$2"
DATE="$3"
SHA="$4"

NAME="memex-server-linux-x64-${DATE}-${SHA}"

# 暂存独立于调用目录（mktemp + 退出清理），避免旧暂存物混入产物
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
PKG="$STAGE/$NAME"

mkdir -p "$PKG/lib"
cp "$BIN" "$PKG/memex-server"

# 递归收集动态依赖。二进制 RPATH 已指向构建树 vcpkg_installed（ldd 直接解析
# 到随链版本）；系统自带的基础库（glibc/libstdc++ 等）不随包。
declare -A visited=()
queue=("$PKG/memex-server")
while [ ${#queue[@]} -gt 0 ]; do
  cur="${queue[0]}"
  queue=("${queue[@]:1}")
  while read -r dep; do
    [ -n "$dep" ] || continue
    case "$dep" in
      */ld-linux-*|*/libc.so.*|*/libm.so.*|*/libpthread*|*/libdl*|*/librt.so.*) continue ;;
      */libstdc++.so.*|*/libgcc_s.so.*) continue ;;
      "$PKG"/lib/*) continue ;;
    esac
    [ -n "${visited[$dep]:-}" ] && continue
    visited[$dep]=1
    cp -L "$dep" "$PKG/lib/"
    while read -r d2; do
      [ -n "${visited[$d2]:-}" ] || queue+=("$d2")
    done < <(ldd "$dep" 2>/dev/null | awk '$3 ~ /^\// {print $3}')
  done < <(ldd "$cur" 2>/dev/null | awk '$3 ~ /^\// {print $3}')
done
# 没有任何非系统依赖时收掉空目录（纯静态链入的构建树）
rmdir "$PKG/lib" 2>/dev/null || true

cat > "$PKG/memex-server.sh" <<'EOF'
#!/bin/sh
# Memex 服务端入口：优先使用随包运行库（有 lib/ 时）
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -d "$DIR/lib" ]; then
  export LD_LIBRARY_PATH="$DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
exec "$DIR/memex-server" "$@"
EOF
chmod +x "$PKG/memex-server.sh"

cat > "$PKG/使用说明.txt" <<'EOF'
Memex 协作服务端（Linux x86-64，每日构建，随包动态依赖）

- 查看版本：./memex-server --version
- 自检：./memex-server --self-test
- 运行：./memex-server.sh serve [--port 24360] [--db <路径>]
  入口脚本自动加载随包运行库；默认端口 24360 为暂定值，待网络侧确认后写入部署文档。

长连接端口对终端防火墙放行后，客户端在协作态登录即可接入；
直连态（UDP 2425 发现 / TCP 2426-2437 点对点）不依赖本服务端。
EOF

tar -czf "$OUT/$NAME.tar.gz" -C "$STAGE" "$NAME"
echo "$NAME.tar.gz"
