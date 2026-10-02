#!/usr/bin/env bash
# 服务端每日构建产物打包：Linux x86-64 二进制压缩包。
# 用法：package-server.sh <memex_server 二进制路径> <输出目录> <日期YYYYMMDD> <短SHA>
# 输出：<输出目录>/memex-server-linux-x64-<日期>-<短SHA>.tar.gz，资产名打印到 stdout。
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

mkdir -p "$PKG"
cp "$BIN" "$PKG/memex-server"

cat > "$PKG/使用说明.txt" <<'EOF'
Memex 协作服务端（Linux x86-64，每日构建）

- 查看版本：./memex-server --version
- 自检：./memex-server --self-test
- 运行：./memex-server [--port 24360]
  默认端口 24360 为暂定值，待网络侧确认后写入部署文档。

长连接端口对终端防火墙放行后，客户端在协作态登录即可接入；
直连态（UDP 2425 发现 / TCP 2426-2437 点对点）不依赖本服务端。
EOF

tar -czf "$OUT/$NAME.tar.gz" -C "$STAGE" "$NAME"
echo "$NAME.tar.gz"
