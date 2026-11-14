#!/usr/bin/env bash
# 覆盖率复核流水线（todo.md:766——COV-98 复查条目可复核）：
#   重跑 = CI 同口径 gcovr（.github/workflows/ci.yml「Coverage report」步逐字）。
# 产物：
#   build-ci/coverage.xml（Cobertura，与 CI 上传 codecov 同格式）
#   build-ci/coverage-summary.txt（行覆盖文字摘要——百分比以此为准）
# 卫生步：剔除「源文件已不存在」的陈旧覆盖产物（gcda＋gcno；LAY-001/002/003
#   拆分后旧目标残留）。CI 干净 runner 每次从零构建无此项；本机增量树若只剔
#   gcda，gcovr 8.x 仍会把 gcno-only 编译单元按 0% 计入——总数被旧文件撑大。
# 依赖：cmake --preset ci 构建 + ctest --preset ci 全量已跑（gcda 才齐）、gcovr。
# 用法：scripts/coverage_report.sh [构建目录，默认 build-ci]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build-ci}"

log() { echo "[coverage] $*"; }

[ -d "$BUILD" ] || { echo "构建目录不存在: $BUILD" >&2; exit 2; }
command -v gcovr >/dev/null || { echo "缺 gcovr" >&2; exit 2; }
cd "$ROOT"

# 陈旧覆盖产物剔除：build/<proj>/CMakeFiles/<tgt>.dir/<proj 内相对源路径>.gcda|gcno
# → 源不在磁盘即删（gcda＋同基名 gcno 一并）；_autogen/（moc 生成源在
# build 树）与含 .. 的路径不碰。gcno-only（编译过但从未跑过）同样计入。
stale=0
while IFS= read -r -d '' g; do
  rel="${g#"$BUILD"/}"
  proj="${rel%%/*}"
  rest="${rel#*CMakeFiles/}"
  rest="${rest#*/}"
  base="${g%.*}"
  src="$proj/${rest%.*}"
  case "$src" in
    *_autogen/*|*..*) continue ;;
  esac
  if [ ! -f "$src" ]; then
    [ -e "$base.gcda" ] || [ -e "$base.gcno" ] || [ -e "$base.o" ] || continue
    rm -f "$base.gcda" "$base.gcno" "$base.o"
    stale=$((stale + 1))
  fi
done < <(find "$BUILD" \( -name '*.gcda' -o -name '*.gcno' \) -print0)
[ "$stale" -eq 0 ] || log "剔除陈旧覆盖产物 $stale 组（源文件已不存在）"

# CI「Coverage report」步逐字（gcovr 调用不改一个字）
gcovr --xml -o "$BUILD/coverage.xml" \
  -f 'common/' -f 'server/' -f 'client/' \
  -e 'third_party/' -e 'vcpkg_installed*/' -e 'build[^/]*/' -e 'tests/'
gcovr --filter 'common/' --filter 'server/' --filter 'client/' \
  --exclude 'third_party/' --exclude 'vcpkg_installed*/' \
  --exclude 'build[^/]*/' --exclude 'tests/' \
  --txt-metric line --txt "$BUILD/coverage-summary.txt"
log "报告: $BUILD/coverage.xml + $BUILD/coverage-summary.txt"
cat "$BUILD/coverage-summary.txt"
