#!/usr/bin/env bash
# 干净机器全量恢复演练（评审 P0-3，todo 452）：单库＋对象卷双资产成对备份
# → 清场（库与桶归零＝模拟干净机器）→ 成对恢复 → files verify 一致性核对
# → 核对工具自证（删真实字节必报不一致）→ 权限关系抽查（本人可下、他人 403）。
# 依赖：docker（RustFS 一键容器）、curl。端口可用环境变量覆盖：
#   API_PORT=19290 CONSOLE_PORT=19291 FILES_PORT=18443 ./files_backup_drill.sh <bin>
# 用法：files_backup_drill.sh <memex_server 二进制路径>
set -euo pipefail

BIN="$(readlink -f "$1")"
[ -x "$BIN" ] || { echo "memex_server 不可执行：$1" >&2; exit 2; }
command -v docker >/dev/null 2>&1 || { echo "需要 docker" >&2; exit 2; }
command -v curl >/dev/null 2>&1 || { echo "需要 curl" >&2; exit 2; }

DIR="$(mktemp -d)"
API_PORT="${API_PORT:-19290}"
CONSOLE_PORT="${CONSOLE_PORT:-19291}"
FILES_PORT="${FILES_PORT:-18443}"
COLLAB_PORT="${COLLAB_PORT:-18236}" # 演示专用协作口（默认口常被真实部署占用）
S3="http://127.0.0.1:$API_PORT"
BUCKET=memex-drill
AK=minioadmin
SK=minioadmin
DATA="$DIR/data"
COMPOSE="$DIR/compose.yml"
DB="$DIR/memex-server.db"
FILES_URL="http://127.0.0.1:$FILES_PORT"
SERVE_PID=""
cleanup() {
  [ -n "$SERVE_PID" ] && kill "$SERVE_PID" 2>/dev/null || true
  "$BIN" storage down "$COMPOSE" >/dev/null 2>&1 || true
  # 数据文件属 uid 10001，宿主 rm 删不动——镜像内以属主身份清（e2e 同款）
  docker run --rm --entrypoint /bin/sh -v "$DATA:/d" rustfs/rustfs:latest \
    -c 'rm -rf /d/* /d/.rustfs.sys' >/dev/null 2>&1 || true
  rm -rf "$DIR" 2>/dev/null || true
}
trap cleanup EXIT

say() { echo "[drill] $*"; }

# —— 1. RustFS 起（宿主目录挂载：备份＝目录 tar） ——
mkdir -p "$DATA"
chmod 777 "$DATA" # rustfs 容器 uid 10001 可写（生产口径为 chown 10001:10001）
"$BIN" storage compose --out "$COMPOSE" --data-dir "$DATA" \
  --api-port "$API_PORT" --console-port "$CONSOLE_PORT" >/dev/null
# 生成器写死 container_name: memex-rustfs（真实部署口径）；宿主可能已有
# 在跑的同名容器（演练绝不碰它）——演练副本改名隔离
sed -i 's/container_name: memex-rustfs$/container_name: memex-rustfs-drill/' "$COMPOSE"
"$BIN" storage up "$COMPOSE" >/dev/null
"$BIN" storage health "$S3" --timeout 60 >/dev/null
say "RustFS 就绪 $S3（数据目录 $DATA）"

# —— 2. 服务端起（单库＋文件面），造数：账号×2＋个人上传×3 ——
"$BIN" account add alice pass-alice --name 爱丽丝 --role admin --db "$DB" >/dev/null
"$BIN" account add bob pass-bob --name 鲍勃 --db "$DB" >/dev/null
MEMEX_S3_ENDPOINT="$S3" MEMEX_S3_BUCKET="$BUCKET" \
MEMEX_S3_ACCESS_KEY="$AK" MEMEX_S3_SECRET_KEY="$SK" \
  "$BIN" serve --port "$COLLAB_PORT" --files-port "$FILES_PORT" --db "$DB" \
  >"$DIR/serve.log" 2>&1 &
SERVE_PID=$!
for i in $(seq 1 40); do
  curl -sf "$FILES_URL/files/health" >/dev/null 2>&1 && break
  [ "$i" = 40 ] && { echo "服务端未就绪，serve 日志：" >&2; cat "$DIR/serve.log" >&2; exit 1; }
  sleep 1
done
say "服务端就绪（files :$FILES_PORT，库 $DB）"

printf 'drill-alpha-content\n' >"$DIR/f1.bin"
printf 'drill-beta-content-longer-body\n' >"$DIR/f2.bin"
printf 'bob-private-note\n' >"$DIR/f3.bin"

tok_of() {
  curl -sf -X POST "$FILES_URL/files/session" \
    -d "{\"account\":\"$1\",\"password\":\"$2\"}" |
    sed -n 's/.*"token":"\([a-f0-9]*\)".*/\1/p'
}
tok_alice="$(tok_of alice pass-alice)"
tok_bob="$(tok_of bob pass-bob)"
[ -n "$tok_alice" ] && [ -n "$tok_bob" ] || { echo "会话令牌未取到" >&2; exit 1; }

up_id() { # up_id <token> <文件> <名>
  curl -sf -X POST "$FILES_URL/files/upload?target=me" \
    -H "Authorization: Bearer $1" -H "X-File-Name: $3" \
    --data-binary "@$2" | sed -n 's/.*"id":\([0-9]*\).*/\1/p'
}
id1="$(up_id "$tok_alice" "$DIR/f1.bin" f1.bin)"
id2="$(up_id "$tok_alice" "$DIR/f2.bin" f2.bin)"
id3="$(up_id "$tok_bob" "$DIR/f3.bin" f3.bin)"
[ -n "$id1" ] && [ -n "$id2" ] && [ -n "$id3" ] || { echo "上传未受理" >&2; exit 1; }
say "造数完成：alice×2 ＋ bob×1（id $id1/$id2/$id3）"

# —— 3. 成对备份：库 cp ＋ 对象卷 tar（同窗口拍＝元数据与字节同一时刻） ——
cp "$DB" "$DIR/db.bak"
tar czf "$DIR/objects.tgz" -C "$DATA" .
say "备份完成：库 $(wc -c <"$DIR/db.bak") 字节＋对象卷 $(wc -c <"$DIR/objects.tgz") 字节"

# —— 4. 清场＝干净机器：停服务、库删除、桶归零 ——
kill "$SERVE_PID" && wait "$SERVE_PID" 2>/dev/null || true
SERVE_PID=""
"$BIN" storage down "$COMPOSE" >/dev/null
rm -f "$DB"
docker run --rm --entrypoint /bin/sh -v "$DATA:/d" rustfs/rustfs:latest \
  -c 'rm -rf /d/* /d/.rustfs.sys'
say "清场完成（库与桶均归零）"

# —— 5. 成对恢复：库 cp 回 ＋ 对象卷 tar 回（含部署口径的属主修复：
#     rustfs 以 uid 10001 运行，恢复后须 chown 回——非 root 宿主经
#     root 容器执行） ——
cp "$DIR/db.bak" "$DB"
tar xzf "$DIR/objects.tgz" -C "$DATA"
docker run --rm --user 0 --entrypoint /bin/sh -v "$DATA:/d" \
  rustfs/rustfs:latest -c 'chown -R 10001:10001 /d'
"$BIN" storage up "$COMPOSE" >/dev/null
"$BIN" storage health "$S3" --timeout 60 >/dev/null
MEMEX_S3_ENDPOINT="$S3" MEMEX_S3_BUCKET="$BUCKET" \
MEMEX_S3_ACCESS_KEY="$AK" MEMEX_S3_SECRET_KEY="$SK" \
  "$BIN" serve --port "$COLLAB_PORT" --files-port "$FILES_PORT" --db "$DB" \
  >"$DIR/serve2.log" 2>&1 &
SERVE_PID=$!
for i in $(seq 1 40); do
  curl -sf "$FILES_URL/files/health" >/dev/null 2>&1 && break
  [ "$i" = 40 ] && { echo "恢复后服务端未就绪，serve 日志：" >&2; cat "$DIR/serve2.log" >&2; exit 1; }
  sleep 1
done
say "恢复完成，服务端再起"

# —— 6. 一致性核对：files verify 退出码 0 ——
if "$BIN" files verify --db "$DB" --s3-endpoint "$S3" --s3-bucket "$BUCKET" \
     --s3-access-key "$AK" --s3-secret-key "$SK"; then
  say "files verify：一致"
else
  echo "files verify 报不一致（恢复面有缺）" >&2
  exit 1
fi

# —— 7. 权限关系抽查：会话随重启失效须重登（内存令牌口径）；
#     本人可下、他人 403、字节与备份前逐位一致 ——
tok_alice="$(tok_of alice pass-alice)"
tok_bob="$(tok_of bob pass-bob)"
[ -n "$tok_alice" ] && [ -n "$tok_bob" ] || { echo "恢复后重登失败" >&2; exit 1; }
curl -sf -H "Authorization: Bearer $tok_alice" \
  "$FILES_URL/files/download?id=$id1" >"$DIR/r1.bin"
cmp -s "$DIR/f1.bin" "$DIR/r1.bin" || { echo "f1 字节不一致" >&2; exit 1; }
curl -sf -H "Authorization: Bearer $tok_alice" \
  "$FILES_URL/files/download?id=$id2" >"$DIR/r2.bin"
cmp -s "$DIR/f2.bin" "$DIR/r2.bin" || { echo "f2 字节不一致" >&2; exit 1; }
code=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $tok_bob" \
  "$FILES_URL/files/download?id=$id1")
[ "$code" = "403" ] || { echo "他人下载个人文件应 403，实得 $code" >&2; exit 1; }
curl -sf -H "Authorization: Bearer $tok_bob" \
  "$FILES_URL/files/download?id=$id3" >"$DIR/r3.bin"
cmp -s "$DIR/f3.bin" "$DIR/r3.bin" || { echo "f3 字节不一致" >&2; exit 1; }
say "权限关系：alice 本人可下（字节一致）、bob 下载 alice 文件 403、bob 本人可下"

# —— 8. 核对工具自证（压轴）：真删一个对象字节 → verify 必报不一致
#     （退出码 1）——核对工具自己也被演练（rustfs 对象在盘上是目录——
#     纠删码分片，故 rm -rf） ——
docker run --rm --entrypoint /bin/sh -v "$DATA:/d" rustfs/rustfs:latest \
  -c "ls /d/$BUCKET/users/alice/ >/dev/null && rm -rf /d/$BUCKET/users/alice/*"
if "$BIN" files verify --db "$DB" --s3-endpoint "$S3" --s3-bucket "$BUCKET" \
     --s3-access-key "$AK" --s3-secret-key "$SK" >/dev/null 2>&1; then
  echo "核对工具未报不一致（对象已删仍一致＝核对失真）" >&2
  exit 1
fi
say "核对工具自证：缺对象必报不一致"

say "演练通过：干净机器全量恢复 → 一致性核对 → 权限关系正确"
