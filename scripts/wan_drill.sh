#!/usr/bin/env bash
# T5.4 广域网／跨地域断线补传场景验证（A16）：
# 在回环口叠加 netem（时延＋丢包），跑断线补传验收（collab_resend），
# 恢复网络后归档必须无重复、无缺失。用法：wan_drill.sh <test_collab_resend 路径>
# 需要 passwordless sudo（tc qdisc）；exit 0 = 降级网络下 A16 通过。
set -euo pipefail

BIN="$(readlink -f "$1")"
[ -x "$BIN" ] || { echo "test_collab_resend 不可执行：$1" >&2; exit 2; }

cleanup() { sudo -n tc qdisc del dev lo root 2>/dev/null || true; }
trap cleanup EXIT

sudo -n tc qdisc replace dev lo root netem delay 80ms 20ms loss 2%
echo "== netem 生效 =="
sudo -n tc qdisc show dev lo

"$BIN"
echo "== 降级网络下断线补传验收通过（A16）=="
