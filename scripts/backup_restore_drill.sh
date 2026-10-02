#!/usr/bin/env bash
# T5.2 备份恢复演练（A14）：账号数据造数 → 离线文件复制备份 → 删库 → 恢复 → 校验。
# 用法：backup_restore_drill.sh <memex_server 二进制路径>
# 通过标准：恢复后 account list 与备份前完全一致；不一致退出码非零。
set -euo pipefail

BIN="$(readlink -f "$1")"
[ -x "$BIN" ] || { echo "memex_server 不可执行：$1" >&2; exit 2; }

DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT
DB="$DIR/memex-server.db"

"$BIN" account add alice pass1 --name 爱丽丝 --role admin --db "$DB" >/dev/null
"$BIN" account add bob pass2 --name 鲍勃 --db "$DB" >/dev/null
BEFORE="$("$BIN" account list --db "$DB")"
echo "备份前："; echo "$BEFORE"

cp "$DB" "$DIR/memex-server.db.bak"
rm "$DB"
cp "$DIR/memex-server.db.bak" "$DB"

AFTER="$("$BIN" account list --db "$DB")"
echo "恢复后："; echo "$AFTER"

[ "$BEFORE" == "$AFTER" ] || { echo "演练失败：恢复后账号清单不一致" >&2; exit 1; }
echo "演练通过：备份 → 恢复后数据完整"
