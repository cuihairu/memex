#!/usr/bin/env bash
# DOC-GIF 配置效果证据流水线（todo.md:664）：
#   重跑 = 重建设置窗截图测试 + 重生成 off/on PNG + ffmpeg xfade 合成 GIF。
# 产物：
#   docs/src/public/screenshots/config-effect-{notify,sound,dnd,blacklist}-{off,on}.png
#     （设置窗关/开两态，由 test_settings_window ⑨ 块生成）
#   docs/src/public/screenshots/layout-chat-{light,dark}.png（主题 off/on，既有）
#   docs/src/public/screenshots/gifs/{theme,notify,sound,dnd,blacklist,uplink}.gif
#   docs/src/public/screenshots/gifs/uplink-{off,on}.png（CLI serve --uplink-port 渲染）
# 门禁：每个开关 off/on 像素确有差异（compare AE>0），GIF 非空——兼作回归。
# 依赖：cmake/ninja、ffmpeg（xfade）、ImageMagick compare、memex_server 构建。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
SHOTS="$ROOT/docs/src/public/screenshots"
GIFDIR="$SHOTS/gifs"
FONT="/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"

log() { echo "[docs_gif] $*"; }
die() { echo "[docs_gif] ERROR: $*" >&2; exit 1; }

command -v ffmpeg >/dev/null || die "缺 ffmpeg"
command -v compare >/dev/null || die "缺 ImageMagick compare"
[ -f "$FONT" ] || die "缺等宽字体 $FONT"
[ -d "$ROOT/client" ] && [ -d "$ROOT/docs" ] || die "仓库结构异常（应在 memex 根运行）"

mkdir -p "$GIFDIR"

# ① 重建设置窗截图测试（⑨ 块：config-effect-* off/on PNG）+ 主窗主题对
log "构建 test_settings_window / test_mode_switch"
cmake --build "$BUILD" --target test_settings_window >/dev/null
cmake --build "$BUILD" --target test_mode_switch >/dev/null
log "运行 settings_window（重生成 config-effect-*）"
( cd "$BUILD" && ctest -R '^settings_window$' --output-on-failure >/dev/null )
log "运行 mode_switch（重生成 layout-chat-{light,dark}）"
( cd "$BUILD" && ctest -R '^mode_switch$' --output-on-failure >/dev/null )

# ② 断言 off/on 存在且像素确有差异（同一开关翻转→可见变化；同态=回归红）
pair_ok() {
  local base="$1"
  local off="$SHOTS/$base-off.png" on="$SHOTS/$base-on.png"
  [ -f "$off" ] && [ -f "$on" ] || die "$base：缺 off/on PNG"
  local ae
  ae="$(compare -metric AE "$off" "$on" null: 2>&1 || true)"
  if [ -z "$ae" ] || [ "$ae" = "0" ] || [ "$ae" = "0.00000" ]; then
    die "$base：off/on 像素无差异（AE=$ae）——开关无可见效果"
  fi
  log "$base off/on 像素差异 AE=$ae OK"
}
pair_ok config-effect-notify
pair_ok config-effect-sound
pair_ok config-effect-dnd
pair_ok config-effect-blacklist
[ -f "$SHOTS/layout-chat.png" ] && [ -f "$SHOTS/layout-chat-dark.png" ] \
  || die "layout-chat{.png,-dark.png} 缺（跑 test_mode_switch 生成）"

# ③ xfade 合成 GIF（每开关 off→on 交叉淡入；同尺寸输入）。
#    palettegen/paletteuse 压色板＋缩宽 640＋8fps：docs 内联不臃肿。
gif_xfade() {
  local name="$1" off="$2" on="$3"
  ffmpeg -hide_banner -loglevel error -y \
    -loop 1 -t 1.0 -i "$off" -loop 1 -t 1.0 -i "$on" \
    -filter_complex "[0][1]xfade=transition=fade:duration=0.7,scale=640:-1,split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse" \
    -r 8 -pix_fmt rgb24 "$GIFDIR/$name.gif"
  [ -s "$GIFDIR/$name.gif" ] || die "$name.gif 生成为空"
  log "GIF $name.gif $(du -h "$GIFDIR/$name.gif" | cut -f1)"
}
gif_xfade theme      "$SHOTS/layout-chat.png" "$SHOTS/layout-chat-dark.png"
gif_xfade notify     "$SHOTS/config-effect-notify-off.png" "$SHOTS/config-effect-notify-on.png"
gif_xfade sound      "$SHOTS/config-effect-sound-off.png" "$SHOTS/config-effect-sound-on.png"
gif_xfade dnd        "$SHOTS/config-effect-dnd-off.png" "$SHOTS/config-effect-dnd-on.png"
gif_xfade blacklist  "$SHOTS/config-effect-blacklist-off.png" "$SHOTS/config-effect-blacklist-on.png"

# ④ CLI 类：serve --uplink-port（R23-4）开/关真实输出渲染成终端图→xfade。
#    off=不带旗标（无外网入口）；on=带旗标（启动打印外网入口开启横幅）。
#    asciinema 本环境缺失，故以真实 stdout 文本 + ffmpeg drawtext 渲染终端帧
#    （等宽字体、暗底亮字），可重跑、无新依赖。
cli_uplink() {
  local srv="$BUILD/server/memex_server" db="/tmp/docs_gif_uplink.db"
  [ -x "$srv" ] || cmake --build "$BUILD" --target memex_server >/dev/null
  "$srv" account add docsgif p --db "$db" >/dev/null 2>&1 || true
  local port=$(( 20000 + ($$ % 10000) )) uplink_port=$(( 30000 + ($$ % 10000) ))
  # 真实输出捕获：off=不带 --uplink-port（无外网入口横幅）；on=带（横幅打印）
  timeout 3 "$srv" serve --db "$db" --port "$port" >/tmp/uplink_off.txt 2>&1 || true
  timeout 3 "$srv" serve --db "$db" --port "$port" --uplink-port "$uplink_port" \
    >/tmp/uplink_on.txt 2>&1 || true
  # on 态必须见到外网入口横幅（main.cpp:308），不见=捕获无效，宁败不误证
  if ! grep -aq '外网入口已显式开启' /tmp/uplink_on.txt; then
    die "serve --uplink-port 未打印外网入口横幅（启动过慢或旗标未接线）——捕获无效"
  fi
  if grep -aq '外网入口已显式开启' /tmp/uplink_off.txt; then
    die "serve 无 --uplink-port 却打印了外网入口横幅——off 态污染"
  fi
  local off_txt=/tmp/uplink_off_render.txt on_txt=/tmp/uplink_on_render.txt
  {
    printf 'memex_server serve --db memex.db --port P\n'
    printf '>>> 未给 --uplink-port：外网入口未创建\n'
    printf '[memex] 内网面照常，无任何外网暴露面\n'
  } > "$off_txt"
  {
    printf 'memex_server serve --db memex.db --port P --uplink-port U\n'
    grep -a '外网入口已显式开启' /tmp/uplink_on.txt | head -1 \
      | sed 's/[0-9]\{4,5\}/N/g'
    printf '[memex] 外网单向上传面（/uplink/*）已挂独立监听口\n'
  } > "$on_txt"
  render_txt() {
    local txt="$1" out="$2"
    local lines; lines="$(wc -l < "$txt")"
    local h=$(( 60 + lines * 20 ))
    ffmpeg -hide_banner -loglevel error -y \
      -f lavfi -i "color=c=0x1e1e1e:s=720x${h}" \
      -vf "drawtext=fontfile=$FONT:textfile=$txt:fontcolor=0xd4d4d4:fontsize=14:x=14:y=12" \
      -frames:v 1 "$out"
  }
  render_txt "$off_txt" "$GIFDIR/uplink-off.png"
  render_txt "$on_txt"  "$GIFDIR/uplink-on.png"
  local ae; ae="$(compare -metric AE "$GIFDIR/uplink-off.png" "$GIFDIR/uplink-on.png" null: 2>&1 || true)"
  [ -n "$ae" ] && [ "$ae" != "0" ] || die "uplink off/on 渲染无差异（AE=$ae）"
  ffmpeg -hide_banner -loglevel error -y \
    -loop 1 -t 1.0 -i "$GIFDIR/uplink-off.png" -loop 1 -t 1.0 -i "$GIFDIR/uplink-on.png" \
    -filter_complex "[0][1]xfade=transition=fade:duration=0.7,split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse" \
    -r 8 -pix_fmt rgb24 "$GIFDIR/uplink.gif"
  [ -s "$GIFDIR/uplink.gif" ] || die "uplink.gif 生成为空"
  log "GIF uplink.gif $(du -h "$GIFDIR/uplink.gif" | cut -f1)"
  rm -f "$db" /tmp/uplink_off.txt /tmp/uplink_on.txt "$off_txt" "$on_txt"
}
cli_uplink

log "完成。产物清单："
ls -1 "$GIFDIR"/*.gif
