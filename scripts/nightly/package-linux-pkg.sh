#!/usr/bin/env bash
# Linux 安装包（deb / rpm）：从同源 tar.gz 产物二次打包，包内布局与 tar 解包
# 即用完全一致（单一事实源，两边永不漂移）。
# 用法：package-linux-pkg.sh <deb|rpm> <client|server> <tar.gz 路径> <输出目录> [版本x.y.z]
# 产物（固定资产名，install.sh 直链取件；身份口径见 docs/src/guide/report.md A23）：
#   client → memex-client-linux-x64.deb / memex-client-linux-x64.rpm
#   server → memex-server-linux-x64.deb / memex-server-linux-x64.rpm
# 布局：/opt/memex/<component>/（tar 内容原样）+ /usr/bin/memex-<component>
#       （入口包装，客户端转发到随包 memex-client.sh）+ 客户端另附 .desktop。
# 自包含口径：Qt/protobuf 等运行库均在 /opt/memex 内，不声明外部 Depends，
# 目标机只需 glibc（与 tar 产物同一底线）。
set -euo pipefail

FMT="$1"
COMPONENT="$2"
TARBALL="$3"
OUT="$4"
# 版本：优先参数（nightly 从根 CMakeLists 提取）；客户端 tar 名可反解，服务端名无版本
VERSION="${5:-$(basename "$TARBALL" | sed -n 's/^MemexClient-\([0-9][0-9.]*\)-.*/\1/p')}"
VERSION="${VERSION:-0.1.0}"
DATE="$(TZ=Asia/Shanghai date +%Y%m%d)"

case "$FMT:$COMPONENT" in
  deb:client|deb:server|rpm:client|rpm:server) ;;
  *) echo "错误：用法 package-linux-pkg.sh <deb|rpm> <client|server> <tar.gz> <输出目录> [版本]" >&2; exit 2 ;;
esac
[ -f "$TARBALL" ] || { echo "错误：tar 产物不存在：$TARBALL" >&2; exit 2; }
mkdir -p "$OUT"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

DESCRIPTION_client="Memex 内网办公即时通讯——Qt 桌面客户端（直连态零配置即用；登录协作态全量归档）。"
DESCRIPTION_server="Memex 内网办公即时通讯——协作服务端（消息转发与全量归档、组织架构、策略下发）。"

# —— 载荷树（deb/rpm 共用）——
PAYLOAD="$STAGE/payload"
mkdir -p "$PAYLOAD/opt/memex/$COMPONENT" "$PAYLOAD/usr/bin"
tar -xzf "$TARBALL" -C "$PAYLOAD/opt/memex/$COMPONENT" --strip-components=1

if [ "$COMPONENT" = client ]; then
  cat > "$PAYLOAD/usr/bin/memex-client" <<'EOF'
#!/bin/sh
# 入口包装：转发到随包运行库入口（LD_LIBRARY_PATH / QT_PLUGIN_PATH / 缓存隔离）
exec /opt/memex/client/memex-client.sh "$@"
EOF
  chmod 755 "$PAYLOAD/usr/bin/memex-client"
  mkdir -p "$PAYLOAD/usr/share/applications"
  cat > "$PAYLOAD/usr/share/applications/memex-client.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Memex
Comment=内网办公即时通讯（直连态零配置即用；协作态全量归档）
GenericName=内网办公即时通讯
Exec=memex-client
Icon=memex
Terminal=false
Categories=Network;InstantMessaging;
StartupWMClass=memex-client
StartupNotify=true
EOF
else
  cat > "$PAYLOAD/usr/bin/memex-server" <<'EOF'
#!/bin/sh
# 入口包装：转发到随包运行库入口（有 lib/ 时 LD_LIBRARY_PATH 生效）
exec /opt/memex/server/memex-server.sh "$@"
EOF
  chmod 755 "$PAYLOAD/usr/bin/memex-server"
fi

ASSET="memex-$COMPONENT-linux-x64"

if [ "$FMT" = deb ]; then
  eval "DESCRIPTION=\$DESCRIPTION_$COMPONENT"
  mkdir -p "$PAYLOAD/DEBIAN"
  cat > "$PAYLOAD/DEBIAN/control" <<EOF
Package: memex-$COMPONENT
Version: ${VERSION}+${DATE}
Section: net
Priority: optional
Architecture: amd64
Maintainer: cuihairu <cuihairu@users.noreply.github.com>
Description: ${DESCRIPTION}
EOF
  # --root-owner-group：非 root 构建也能落 root:root 属主（CI 免 fakeroot）
  dpkg-deb --root-owner-group --build "$PAYLOAD" "$OUT/$ASSET.deb"
  echo "$ASSET.deb"
  exit 0
fi

# —— rpm ——
# spec 的 %install 从暂存载荷树拷入 buildroot（无源码编译动作）
eval "DESCRIPTION=\$DESCRIPTION_$COMPONENT"
SPEC="$STAGE/memex-$COMPONENT.spec"
cat > "$SPEC" <<EOF
Name: memex-$COMPONENT
Version: ${VERSION}.${DATE}
Release: 1
Summary: Memex $COMPONENT (nightly)
License: Apache-2.0
URL: https://github.com/cuihairu/memex
BuildArch: x86_64
# 自包含口径（与 deb 侧手写 control 无 Depends 对齐）：不扫描 ELF 自动生成
# libc/libstdc++ 等依赖——运行库随包在 /opt/memex，目标机只需 glibc 底线，
# 且 rpmdb 无记录的环境（如构建机走查）也能直装
AutoReq: no
AutoProv: no
%description
${DESCRIPTION}

%prep

%build

%install
cp -a "$PAYLOAD/opt" "%{buildroot}/opt"
cp -a "$PAYLOAD/usr" "%{buildroot}/usr"

%files
/opt/memex/$COMPONENT
/usr/bin/memex-$COMPONENT
EOF
if [ "$COMPONENT" = client ]; then
  echo "/usr/share/applications/memex-client.desktop" >> "$SPEC"
fi
echo "%changelog" >> "$SPEC"

# _topdir 收进暂存：不污染 ~/rpmbuild；--target 定死 x86_64
RPMTOP="$STAGE/rpmbuild"
mkdir -p "$RPMTOP/BUILD" "$RPMTOP/RPMS" "$RPMTOP/SOURCES" "$RPMTOP/SPECS"
rpmbuild -bb --target x86_64 \
  --define "_topdir $RPMTOP" \
  "$SPEC" >/dev/null
cp "$RPMTOP/RPMS/x86_64/memex-$COMPONENT-"*.rpm "$OUT/$ASSET.rpm"
echo "$ASSET.rpm"
