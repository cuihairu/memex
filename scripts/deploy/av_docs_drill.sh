#!/usr/bin/env bash
# 音视频会议＋协同文档底座验证（todo 音视频会议·协同文档行）：
# compose 配置校验 → 拉起（LiveKit 内嵌 TURN＋OnlyOffice）→ 建房/列房/删房
# API 全通 → TURN Allocate 成功（对称 NAT 穿透凭据链）→ OnlyOffice 健康 →
# 回收。exit 0 = 底座可用。
# 用法：scripts/deploy/av_docs_drill.sh
# 需要 docker（compose v2）＋ python3；全程约 2-4 分钟（含镜像拉取）。
set -euo pipefail

DEPLOY="$(cd "$(dirname "$0")" && pwd)"
LK_URL="http://127.0.0.1:7880"
OO_URL="http://127.0.0.1:8800"
ROOM="drill-room"

cleanup() {
  docker compose -f "$DEPLOY/meet-compose.yml" -f "$DEPLOY/docs-compose.yml" \
    down -v >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "== 1/6 compose 配置校验 =="
docker compose -f "$DEPLOY/meet-compose.yml" config -q
docker compose -f "$DEPLOY/docs-compose.yml" config -q
echo "compose 配置合法"

echo "== 2/6 拉起底座 =="
docker compose -f "$DEPLOY/meet-compose.yml" -f "$DEPLOY/docs-compose.yml" \
  up -d --quiet-pull

echo "== 3/6 等待健康（LiveKit 60s／OnlyOffice 180s）=="
for i in $(seq 1 12); do
  LK_OK="$(curl -s -o /dev/null -w '%{http_code}' "$LK_URL/" || true)"
  if [ "$LK_OK" = "404" ] || [ "$LK_OK" = "200" ]; then break; fi
  sleep 5
done
[ "$LK_OK" = "404" ] || [ "$LK_OK" = "200" ] || {
  echo "LiveKit 未就绪（HTTP $LK_OK）" >&2; exit 1; }
echo "LiveKit HTTP 可达"
for i in $(seq 1 36); do
  if curl -sf "$OO_URL/healthcheck" | grep -q true; then break; fi
  sleep 5
done
curl -sf "$OO_URL/healthcheck" | grep -q true || {
  echo "OnlyOffice 未就绪" >&2; exit 1; }
echo "OnlyOffice 健康"

echo "== 4/6 LiveKit 建房／列房／删房 =="
# 根路径 200 早于 Twirp 就绪：先轮询 ListRooms 直到 API 真正可用（60s）
python3 - "$LK_URL" "$ROOM" <<'EOF'
import base64, hashlib, hmac, json, sys, time, urllib.request

url, room = sys.argv[1], sys.argv[2]
key = "devkey"
secret = b"devsecret0123456789devsecret0123456789"

def jwt():
    header = base64.urlsafe_b64encode(b'{"alg":"HS256","typ":"JWT"}').rstrip(b"=")
    now = int(time.time())
    body = base64.urlsafe_b64encode(json.dumps({
        "iss": key, "sub": "drill", "iat": now, "exp": now + 300,
        # roomJoin 看似与建房无关，但缺失时本版本以 404 拒绝建房——一并授予
        "video": {"roomCreate": True, "roomList": True, "roomAdmin": True,
                  "roomJoin": True},
    }).encode()).rstrip(b"=")
    sig = base64.urlsafe_b64encode(
        hmac.new(secret, header + b"." + body, hashlib.sha256).digest()).rstrip(b"=")
    return (header + b"." + body + b"." + sig).decode()

def twirp(service, payload):
    req = urllib.request.Request(
        url + "/twirp/livekit." + service,
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json",
                 "Authorization": "Bearer " + jwt()})
    with urllib.request.urlopen(req, timeout=15) as r:
        return json.load(r)

deadline = time.time() + 60
while True:
    try:
        twirp("RoomService.ListRooms", {})
        break
    except Exception:
        if time.time() > deadline:
            raise
        time.sleep(3)
print("API 就绪")

twirp("RoomService.CreateRoom", {"name": room})
rooms = twirp("RoomService.ListRooms", {})
assert room in [x["name"] for x in rooms.get("rooms", [])], rooms
print("建房＋列房可见：" + room)
twirp("RoomService.DeleteRoom", {"room": room})
rooms = twirp("RoomService.ListRooms", {})
assert room not in [x["name"] for x in rooms.get("rooms", [])], rooms
print("删房已回收")
EOF

echo "== 5/6 TURN Allocate（内嵌 TURN 凭据链）=="
python3 - <<'EOF'
import base64, hashlib, hmac, os, socket, struct, time

USER_EXP = str(int(time.time()) + 300) + ":drill"
SECRET = b"devsecret0123456789devsecret0123456789"
PASSWORD = base64.b64encode(
    hmac.new(SECRET, USER_EXP.encode(), hashlib.sha1).digest()).decode()

def attr(t, v):
    return struct.pack(">HH", t, len(v)) + v + b"\x00" * (-len(v) % 4)

def msg(mtype, txid, *attrs):
    body = b"".join(attrs)
    return struct.pack(">HHI12s", mtype, len(body), 0x2112A442, txid) + body

def parse(data):
    assert struct.unpack(">H", data[:2])[0] == 0x0104, data.hex()[:32]  # Allocate success
    off, out = 20, {}
    while off + 4 <= len(data):
        t, ln = struct.unpack(">HH", data[off:off + 4])
        out[t] = data[off + 4:off + 4 + ln]
        off += 4 + ln + (-ln % 4)
    assert 0x0016 in out, "无 RELAYED-ADDRESS"
    relayed_port = struct.unpack(">H", out[0x0016][2:4])[0]
    print("Allocate 成功，中继端口：%d" % relayed_port)

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(10)
txid = os.urandom(12)
s.sendto(msg(0x0003, txid, attr(0x0006, USER_EXP.encode())), ("127.0.0.1", 3478))
data, _ = s.recvfrom(2048)  # 401（带 NONCE／REALM）
off, attrs = 20, {}
while off + 4 <= len(data):
    t, ln = struct.unpack(">HH", data[off:off + 4])
    attrs[t] = data[off + 4:off + 4 + ln]
    off += 4 + ln + (-ln % 4)
nonce, realm = attrs[0x0015], attrs[0x0014]
user = USER_EXP.encode()
key = hashlib.md5(b":".join([user, realm, PASSWORD.encode()])).digest()
body_attrs = (attr(0x0006, user) + attr(0x0014, realm) + attr(0x0015, nonce) +
              attr(0x0019, struct.pack(">I", 0x11000000)) + attr(0x000D, b""))
integrity = hmac.new(key, struct.pack(">HHI12s", 0x0003, len(body_attrs) + 24,
                                      0x2112A442, txid) + body_attrs,
                     hashlib.sha1).digest()
full = body_attrs + attr(0x0008, integrity)
s.sendto(struct.pack(">HHI12s", 0x0003, len(full), 0x2112A442, txid) + full,
         ("127.0.0.1", 3478))
data, _ = s.recvfrom(2048)
parse(data)
EOF

echo "== 6/6 回收（trap 自动 down -v）=="
echo "音视频＋协同文档底座验证通过"
