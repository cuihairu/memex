#!/usr/bin/env bash
# echo bot：收到什么打印什么（机器人平台最小示例——几行代码跑通收发）。
#
# 用法：
#   MEMEX_SERVER=127.0.0.1:24361 MEMEX_TOKEN=bot_xxx… ./examples/echo_bot.sh
#
# 前置（管理台）：
#   memex_server bot add echo --by admin1      # 得 token（仅此一次显示）
#   memex_server bot join echo <群号>          # 可选：加群收群消息
#
# 行为：每秒拉一次 /bot/updates，打印收到的文本，ack 后原样回发。
# 依赖：curl（jq 可选——有则输出更整齐）。
set -u

SERVER="${MEMEX_SERVER:-127.0.0.1:24361}"
TOKEN="${MEMEX_TOKEN:?缺 MEMEX_TOKEN（memex_server bot add 输出的 token）}"
POLL="${MEMEX_POLL:-1}"

api() { # api <method> <path> [json-body]
  local method="$1" path="$2" body="${3-}"
  if [ -n "$body" ]; then
    curl -fsS -X "$method" -H "Authorization: Bearer $TOKEN" \
      -H 'Content-Type: application/json' -d "$body" "$SERVER$path"
  else
    curl -fsS -X "$method" -H "Authorization: Bearer $TOKEN" "$SERVER$path"
  fi
}

echo "echo bot 已启动（$SERVER，每 ${POLL}s 拉一次，Ctrl-C 退出）"
while true; do
  # jq 缺席时用 python3 兜底解析（再缺席则打印原始 JSON 由人眼读）
  if command -v jq >/dev/null 2>&1; then
    UPDATES=$(api GET /bot/updates | jq -r '.updates[] |
      "\(.msg_id)\t\(.from)\t\(.text)"' 2>/dev/null) || UPDATES=""
  elif command -v python3 >/dev/null 2>&1; then
    UPDATES=$(api GET /bot/updates | python3 -c '
import json,sys
for u in json.load(sys.stdin).get("updates",[]):
    print(f"{u[\"msg_id\"]}\t{u[\"from\"]}\t{u[\"text\"]}")' 2>/dev/null) || UPDATES=""
  else
    UPDATES=$(api GET /bot/updates)
  fi

  [ -z "$UPDATES" ] && { sleep "$POLL"; continue; }
  while IFS="$(printf '\t')" read -r msg_id from text; do
    [ -n "$msg_id" ] || continue
    echo "收到 [$from] $text"
    api POST /bot/ack "{\"msg_id\":\"$msg_id\"}" >/dev/null
    # 原样回发（群消息回群、单聊回本人；from 是 bot: 前缀的环回不回发防打转）
    case "$from" in
      bot:*) ;;
      group:*) api POST /bot/send "{\"target\":\"$from\",\"text\":\"$text\"}" >/dev/null ;;
      *) api POST /bot/send "{\"target\":\"$from\",\"text\":\"$text\"}" >/dev/null ;;
    esac
  done <<< "$UPDATES"
  sleep "$POLL"
done
