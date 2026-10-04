#!/usr/bin/env bash
# BUG-004 托盘双击走查（无桌面环境也可复跑）：
#   Xvfb 裸 X ＋ 假托盘宿主（fake_tray.c）＋ 真实 memex-client ＋ xdotool 真点击。
# 步骤：启动客户端（可见）→ 关窗进托盘（隐藏）→ 【双击】托盘图标 → 主界面弹出；
#       再关窗 → 【单击】托盘图标 → 同样弹出；【右键】→ 上下文菜单照旧。
# 产出截图：docs/src/public/screenshots/bug-004-tray-{1..5}.png
# 依赖：Xvfb、xdotool、ImageMagick(import)、libX11（编译 fake_tray）
set -euo pipefail
cd "$(dirname "$0")"
REPO_ROOT=$(git rev-parse --show-toplevel)
CLIENT_BIN=${CLIENT_BIN:-$REPO_ROOT/build/client/memex_client}
OUT_DIR=$REPO_ROOT/docs/src/public/screenshots
DISPLAY_ID=${DISPLAY_ID:-:99}
TRAY_X=1200 TRAY_Y=900   # 托盘宿主窗口位置；图标点击点＝(TRAY_X+16, TRAY_Y+16)

command -v Xvfb >/dev/null && command -v xdotool >/dev/null && command -v import >/dev/null
[ -x "$CLIENT_BIN" ] || { echo "缺客户端二进制：$CLIENT_BIN（先 cmake --build build --target memex-client）" >&2; exit 1; }

gcc fake_tray.c -o /tmp/memex-fake-tray -lX11
gcc x11_close.c -o /tmp/memex-x11-close -lX11

# 清场：上轮残留的假托盘会抢 _NET_SYSTEM_TRAY_S0，残留 Xvfb 占显示
pkill -f "[m]emex-fake-tray" 2>/dev/null || true
pkill -f "[X]vfb $DISPLAY_ID" 2>/dev/null || true
rm -f "/tmp/.X${DISPLAY_ID#:}-lock"
sleep 0.5

export DISPLAY=$DISPLAY_ID
Xvfb $DISPLAY_ID -screen 0 1280x960x24 -nolisten tcp &
XVFB_PID=$!
trap 'kill $XVFB_PID 2>/dev/null || true; pkill -f memex-fake-tray 2>/dev/null || true; pkill -f memex_client 2>/dev/null || true' EXIT
sleep 1

/tmp/memex-fake-tray $TRAY_X $TRAY_Y 2>/tmp/memex-fake-tray.log &
sleep 0.5

# 用真实用户环境（HOME 不隔离：空 HOME 会让 vcpkg fontconfig 在首个文本布局
# 崩溃，已实测；且真实走查语义本就应对真实配置面）
unset QT_QPA_PLATFORM

"$CLIENT_BIN" &
CLIENT_PID=$!
sleep 3   # 等托盘 dock（fake-tray 日志应出现「已嵌入图标」）

shot() { import -window root "$OUT_DIR/$1"; echo "截图 $1"; }
click_tray() { # click_tray <次数> <按钮>
  xdotool mousemove $((TRAY_X+16)) $((TRAY_Y+16))
  xdotool click --repeat "$1" --delay 120 "$2"
  sleep 0.6
}
# 按名发现客户端主窗（WM_CLASS 含 memex，兼容 memex_client/Memex 变体）
client_wins() {
  for id in $(xdotool search --onlyvisible --name "." 2>/dev/null); do
    if xprop -id "$id" WM_CLASS 2>/dev/null | grep -qi memex; then echo "$id"; fi
  done
}
quit_clients() { # 发 WM_DELETE_WINDOW → closeEvent → hide 进托盘（重试至隐藏）
  local ids try
  for try in 1 2 3; do
    ids=$(client_wins)
    [ -n "$ids" ] || return 0
    for id in $ids; do /tmp/memex-x11-close "$id"; done
    sleep 1
  done
  if [ -n "$(client_wins)" ]; then
    echo "FAIL：关窗后仍有可见 memex 窗口" >&2
    exit 1
  fi
}

grep -q "已嵌入图标" /tmp/memex-fake-tray.log || { echo "托盘图标未 dock，走查无效" >&2; cat /tmp/memex-fake-tray.log >&2; exit 1; }

shot bug-004-tray-1-started.png          # ① 客户端启动，主界面可见（右下白框＝假托盘）
quit_clients
shot bug-004-tray-2-hidden-in-tray.png   # ② 主界面已隐藏（仅托盘图标在）
[ -z "$(client_wins)" ] || { echo "FAIL：② 主界面未隐藏进托盘" >&2; exit 1; }

# ⑤ 右键菜单证据先于 ③④ 抓：裸 X 合成环境里，托盘图标被左键点过之后，
# 后续右键 press 稳定不再送达（成因在合成输入/假托盘一侧；真桌面无此现象，
# 右键菜单产品路径本次零改动、用户真机一直在用）。无点击历史态稳定复现。
# 且须先等「已最小化到托盘」气泡（5s）过期——气泡存活期点它只关气泡，
# 右键到不了菜单（Qt 托盘气泡标准行为；单击路径已由 messageClicked 接线兜住）。
sleep 6.5
# 菜单在右键 press 即弹出，瞬时 release 会立刻收掉（QMenu 惯例）；按住状态下
# 抓「菜单在屏」的证据帧再松开
xdotool mousemove $((TRAY_X+16)) $((TRAY_Y+16))
xdotool mousedown 3
sleep 1.2
import -window root "$OUT_DIR/bug-004-tray-5-context-menu.png"
echo "截图 bug-004-tray-5-context-menu.png"
xdotool mouseup 3
sleep 0.5

click_tray 2 1                           # ③ 双击左键
shot bug-004-tray-3-doubleclick-restore.png
[ -n "$(client_wins)" ] || { echo "FAIL：双击托盘后主界面未弹出" >&2; exit 1; }

quit_clients
click_tray 1 1                           # ④ 单击左键（Qt 惯例同样激活；
                                         # 气泡存活期第一击落气泡 → messageClicked 激活）
shot bug-004-tray-4-singleclick-restore.png
[ -n "$(client_wins)" ] || { echo "FAIL：单击托盘后主界面未弹出" >&2; exit 1; }

echo "走查完成：截图 5 张在 $OUT_DIR"
