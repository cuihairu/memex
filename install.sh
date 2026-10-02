#!/usr/bin/env bash
# Memex 一键安装（Linux / macOS）——从每日构建（nightly Release，匿名直链取件）安装。
#
# 用法：
#   # 客户端（默认；Linux 装 deb/rpm，macOS 装 dmg 到 /Applications）：
#   curl -fsSL https://raw.githubusercontent.com/cuihairu/memex/main/install.sh | bash
#   # 服务端（Linux deb/rpm 需 root；macOS 装 dmg 内 pkg 到 /usr/local/bin）：
#   curl -fsSL https://raw.githubusercontent.com/cuihairu/memex/main/install.sh | bash -s -- --server
#   # 本地脚本形态（可检视后再跑）：./install.sh [--server] [--help]
#
# 口径（与 cockpit/falcon/unidict 同规格）：
#   - 检测 OS 与 CPU 架构，不认识的组合明确报错，绝不猜下载地址；
#   - 匿名直链（GitHub Release 资产无需登录），失败即停；
#   - Linux 包管理器顺序：deb（dpkg/apt）→ rpm（dnf/yum/zypper）→ 无包管理器
#     的发行版回落 tar.gz（经 GitHub API 解析最新资产名）；重跑=升级（幂等）；
#   - macOS：客户端 dmg 拖入 /Applications，服务端 dmg 内 pkg 经 installer 安装；
#   - 装后 --version 真验证（输出版本行才算成功）。
# 测试接缝：MEMEX_REPO / MEMEX_BASE_URL（换源），MEMEX_TEST_OS / MEMEX_TEST_ARCH
# （覆盖 uname 探测，供无对应系统的 CI/本地验证）。
set -euo pipefail

REPO="${MEMEX_REPO:-cuihairu/memex}"
BASE_URL="${MEMEX_BASE_URL:-https://github.com/${REPO}/releases/download/nightly}"
COMPONENT=client

info() { printf '==> %s\n' "$*"; }
die()  { printf '错误：%s\n' "$*" >&2; exit 1; }

usage() {
  cat <<EOF
Memex 一键安装（从每日构建取件，匿名可下载）

用法: install.sh [--client|--server] [--help]

  --client    安装桌面客户端（默认）
  --server    安装协作服务端
  --help      显示本帮助

环境变量:
  MEMEX_REPO      目标仓库（默认 cuihairu/memex）
  MEMEX_BASE_URL  资产直链基址（默认 .../releases/download/nightly）
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --client) COMPONENT=client ;;
    --server) COMPONENT=server ;;
    --help|-h) usage; exit 0 ;;
    *) die "未知参数：$1（--help 查看用法）" ;;
  esac
  shift
done

command -v curl >/dev/null 2>&1 || die "需要 curl（未安装）"

OS="${MEMEX_TEST_OS:-$(uname -s)}"
ARCH_RAW="${MEMEX_TEST_ARCH:-$(uname -m)}"

# —— 平台判定（不认识的组合明确报错，绝不猜测资产名）——
case "$OS:$ARCH_RAW" in
  Linux:x86_64|Linux:amd64)    PLATFORM=linux-x64 ;;
  Darwin:arm64)                PLATFORM=macos-arm64 ;;
  Darwin:x86_64)               die "macOS Intel（x86_64）：每日构建暂只提供 Apple Silicon（arm64）包" ;;
  Linux:aarch64|Linux:arm64)   die "Linux arm64：每日构建暂只提供 x86_64（x64）包" ;;
  Windows:*|MINGW*:*|MSYS*:*)  die "Windows 请改用 install.ps1（PowerShell 一键安装）" ;;
  *)                           die "不支持的系统/架构：$OS / $ARCH_RAW" ;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# —— 下载：404（资产未发布/改名）与网络失败分开报错 ——
download_url() { # download_url <url> <落盘路径> <资产名>
  local url="$1" dest="$2" name="$3" code
  info "下载 $name"
  code="$(curl -fsSL -o "$dest" -w '%{http_code}' "$url" 2>/dev/null || true)"
  [ -n "$code" ] || code=000
  if [ "$code" = 404 ]; then
    die "资产不存在（HTTP 404）：$name——今日构建可能尚未发布，可到 ${BASE_URL%/nightly}/nightly 手动取件"
  fi
  if [ "$code" != 200 ]; then
    die "下载失败（HTTP ${code}）：$url"
  fi
  [ -s "$dest" ] || die "下载文件为空：$name"
}

# —— 装后 --version 真验证：输出须以期望前缀开头 ——
verify() { # verify <二进制路径> <期望前缀>
  local out
  [ -x "$1" ] || die "安装后验证失败：$1 不存在"
  out="$("$1" --version 2>/dev/null || true)"
  case "$out" in
    "$2"*) info "已验证：$out" ;;
    *)    die "安装后验证失败：$1 --version 输出异常（得到：${out:-空}）" ;;
  esac
}

# —— root 执行：已是 root 直跑；否则免密 sudo；都没有则明确指引（不静默降级）——
as_root() {
  if [ "$(id -u)" = 0 ]; then
    "$@"
  elif sudo -n true 2>/dev/null; then
    sudo "$@"
  else
    die "需要 root 权限执行：$*（请以 sudo 重跑本脚本）"
  fi
}

# ============================ Linux ============================
install_linux() {
  local pkg="memex-$COMPONENT-$PLATFORM"
  if command -v dpkg >/dev/null 2>&1; then
    download_url "$BASE_URL/$pkg.deb" "$TMP/$pkg.deb" "$pkg.deb"
    info "安装 deb（重跑=升级）"
    as_root dpkg -i "$TMP/$pkg.deb"
    verify "/usr/bin/memex-$COMPONENT" "memex-$COMPONENT"
    info "安装完成：/usr/bin/memex-$COMPONENT（随包 Qt/运行库在 /opt/memex/$COMPONENT）"
    return
  fi
  if command -v rpm >/dev/null 2>&1; then
    download_url "$BASE_URL/$pkg.rpm" "$TMP/$pkg.rpm" "$pkg.rpm"
    info "安装 rpm（重跑=升级）"
    if command -v dnf >/dev/null 2>&1; then
      as_root dnf -y install "$TMP/$pkg.rpm"
    elif command -v yum >/dev/null 2>&1; then
      as_root yum -y install "$TMP/$pkg.rpm"
    elif command -v zypper >/dev/null 2>&1; then
      as_root zypper --non-interactive install "$TMP/$pkg.rpm"
    else
      as_root rpm -Uvh --replacepkgs "$TMP/$pkg.rpm"
    fi
    verify "/usr/bin/memex-$COMPONENT" "memex-$COMPONENT"
    info "安装完成：/usr/bin/memex-$COMPONENT（随包运行库在 /opt/memex/$COMPONENT）"
    return
  fi

  # 无 dpkg/rpm 的发行版：tar.gz 回落（经 API 解析带日期-短SHA 的最新资产名）
  local api url tgz top
  info "未检测到 dpkg/rpm 包管理器，回落 tar.gz 直装（/opt/memex）"
  api="$(curl -fsSL "https://api.github.com/repos/$REPO/releases/tags/nightly" 2>/dev/null)" \
    || die "解析 nightly 资产列表失败（GitHub API）"
  if [ "$COMPONENT" = client ]; then
    url="$(printf '%s' "$api" | tr ',' '\n' | sed -n 's/.*"browser_download_url": *"\([^"]*\)".*/\1/p' \
      | grep '/MemexClient-' | grep '\.tar\.gz' | head -n1 || true)"
  else
    url="$(printf '%s' "$api" | tr ',' '\n' | sed -n 's/.*"browser_download_url": *"\([^"]*\)".*/\1/p' \
      | grep '/memex-server-linux-x64-' | grep '\.tar\.gz' | head -n1 || true)"
  fi
  [ -n "$url" ] || die "未在 nightly Release 找到 memex-$COMPONENT 的 tar.gz 资产"
  tgz="$TMP/$(basename "$url")"
  download_url "$url" "$tgz" "$(basename "$url")"
  mkdir -p "$TMP/x"
  tar -xzf "$tgz" -C "$TMP/x"
  top="$(ls "$TMP/x")"
  [ -f "$TMP/x/$top/memex-$COMPONENT.sh" ] || die "解包结构异常：未找到 memex-$COMPONENT.sh"
  info "安装到 /opt/memex/$COMPONENT（重跑=升级）"
  as_root mkdir -p /opt/memex
  as_root rm -rf "/opt/memex/$COMPONENT"
  as_root cp -R "$TMP/x/$top" "/opt/memex/$COMPONENT"
  as_root sh -c "printf '%s\n' '#!/bin/sh' 'exec /opt/memex/$COMPONENT/memex-$COMPONENT.sh \"\$@\"' > /usr/local/bin/memex-$COMPONENT"
  as_root chmod 755 "/usr/local/bin/memex-$COMPONENT"
  verify "/usr/local/bin/memex-$COMPONENT" "memex-$COMPONENT"
  info "安装完成：/usr/local/bin/memex-$COMPONENT"
}

# ============================ macOS ============================
install_macos() {
  local asset="memex-$COMPONENT-$PLATFORM.dmg" mnt="$TMP/mnt"
  download_url "$BASE_URL/$asset" "$TMP/$asset" "$asset"
  mkdir -p "$mnt"
  info "挂载 dmg 并安装（重跑=升级覆盖）"
  hdiutil attach "$TMP/$asset" -nobrowse -readonly -mountpoint "$mnt" >/dev/null \
    || die "dmg 挂载失败：$asset"
  if [ "$COMPONENT" = client ]; then
    [ -d "$mnt/Memex.app" ] || { hdiutil detach "$mnt" -quiet || true; die "dmg 内未找到 Memex.app"; }
    as_root rm -rf /Applications/Memex.app
    as_root ditto "$mnt/Memex.app" /Applications/Memex.app
    hdiutil detach "$mnt" -quiet || true
    # 下载产物带 Gatekeeper 隔离属性：安装时清掉，否则首次打开被拦
    as_root xattr -cr /Applications/Memex.app 2>/dev/null \
      || info "提示：若打开被 Gatekeeper 拦截，请手动执行 xattr -cr /Applications/Memex.app"
    verify "/Applications/Memex.app/Contents/MacOS/Memex" "memex-client"
    info "安装完成：/Applications/Memex.app（启动台或 open /Applications/Memex.app）"
  else
    local pkg="$mnt/memex-$COMPONENT-$PLATFORM.pkg"
    [ -f "$pkg" ] || { hdiutil detach "$mnt" -quiet || true; die "dmg 内未找到 $pkg"; }
    as_root installer -pkg "$pkg" -target /
    hdiutil detach "$mnt" -quiet || true
    verify "/usr/local/bin/memex-server" "memex-server"
    info "安装完成：/usr/local/bin/memex-server"
  fi
}

case "$OS" in
  Linux)   install_linux ;;
  Darwin)  install_macos ;;
  *)       die "不支持的系统：$OS" ;;
esac

info "全部完成。重跑本脚本即为升级（幂等）。"
