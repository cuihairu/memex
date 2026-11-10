#!/usr/bin/env python3
"""E2E-LOCAL-001：本机真实服务器协议走查（Python 最小客户端）。

对照的协议事实源（不许编造字段，全部以此为准）：
  - common/proto/memex.proto      帧载荷 = Envelope；oneof 字段号 = 100 + MsgType
  - common/src/frame.cpp          帧格式 = 4 字节大端长度前缀 + Envelope 序列化字节
  - server/src/session.cpp        LOGIN(30)→LOGIN_RESULT(31)、TEXT(10)→ACK(11)/
                                  DELIVER_NOTICE(54)、ORG_QUERY(34)→ORG_DATA(35)、
                                  PRESENCE_QUERY(43)→PRESENCE_DATA(44)、PING(2)→PONG(3)

用法（默认即巡检单口径，指向本机 E2E 实例而非生产 24360）：
  python3 memex_e2e_client.py [--host 127.0.0.1] [--port 24370]
      [--account-a e2e_a --password-a e2e-pass-a]
      [--account-b e2e_b --password-b e2e-pass-b]

前置：服务器已起且两个账号已建：
  memex_server account add e2e_a <口令> --db <库>
  memex_server account add e2e_b <口令> --db <库>

覆盖面：PING/PONG 健康检查 → 错口令负路径 → 登录成功 → 组织架构（联系人）
→ 在线表 → A→B 文本（ACK + 送达回执）→ B→A 文本（ACK + 送达回执）。
直连文件旁路（FILE_META 等）不经服务端、协作面文件需 S3，不在本走查范围。
"""

import argparse
import hashlib
import socket
import struct
import sys
import time

# ---- MsgType（memex.proto enum，只列本脚本用到者）----
PING = 2
PONG = 3
TEXT = 10
ACK = 11
LOGIN = 30
LOGIN_RESULT = 31
LOGOUT = 33
ORG_QUERY = 34
ORG_DATA = 35
PRESENCE_QUERY = 43
PRESENCE_DATA = 44
DELIVER_NOTICE = 54

TYPE_NAMES = {
    PING: "PING", PONG: "PONG", TEXT: "TEXT", ACK: "ACK", LOGIN: "LOGIN",
    LOGIN_RESULT: "LOGIN_RESULT", LOGOUT: "LOGOUT", ORG_QUERY: "ORG_QUERY",
    ORG_DATA: "ORG_DATA", PRESENCE_QUERY: "PRESENCE_QUERY",
    PRESENCE_DATA: "PRESENCE_DATA", DELIVER_NOTICE: "DELIVER_NOTICE",
}


# ---- protobuf wire format（varint / 64bit / len-delim 三类足够） ----

def enc_varint(v: int) -> bytes:
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def enc_tag(field: int, wire: int) -> bytes:
    return enc_varint((field << 3) | wire)


def enc_str(field: int, s: str) -> bytes:
    b = s.encode("utf-8")
    return enc_tag(field, 2) + enc_varint(len(b)) + b


def enc_uint(field: int, v: int) -> bytes:
    return b"" if v == 0 else enc_tag(field, 0) + enc_varint(v)


def enc_bool(field: int, v: bool) -> bytes:
    return enc_tag(field, 0) + enc_varint(1 if v else 0) if v else b""


def dec_varint(data: bytes, i: int):
    result = shift = 0
    while True:
        b = data[i]
        i += 1
        result |= (b & 0x7F) << shift
        if not b & 0x80:
            return result, i
        shift += 7


def dec_fields(data: bytes) -> dict:
    """解析为 {field_no: [(wire_type, 原始值), ...]，未知字段照 proto3 跳过。"""
    out: dict[int, list] = {}
    i = 0
    while i < len(data):
        tag, i = dec_varint(data, i)
        field, wire = tag >> 3, tag & 7
        if wire == 0:
            val, i = dec_varint(data, i)
        elif wire == 2:
            n, i = dec_varint(data, i)
            val = data[i:i + n]
            i += n
        elif wire == 1:
            val = data[i:i + 8]
            i += 8
        elif wire == 5:
            val = data[i:i + 4]
            i += 4
        else:
            raise ValueError(f"不支持的 wire type {wire}（field {field}）")
        out.setdefault(field, []).append((wire, val))
    return out


def f_str(fields: dict, field: int, default: str = "") -> str:
    if field in fields:
        return fields[field][0][1].decode("utf-8")
    return default


def f_uint(fields: dict, field: int, default: int = 0) -> int:
    return fields[field][0][1] if field in fields else default


def f_bool(fields: dict, field: int) -> bool:
    return bool(fields[field][0][1]) if field in fields else False


# ---- Envelope 组包/解包 ----

def envelope(msg_type: int, body_field: int | None = None,
             body: bytes | None = None, **kw) -> bytes:
    """kw: seq, from, to, ts_ms, msg_id（proto3 默认值不落线，同 C++ 端）。"""
    payload = enc_uint(1, msg_type)                       # Envelope.type
    payload += enc_uint(2, kw.get("seq", 0))              # Envelope.seq
    payload += enc_str(3, kw.get("from", ""))             # Envelope.from
    payload += enc_str(4, kw.get("to", ""))               # Envelope.to
    payload += enc_uint(5, kw.get("ts_ms", 0))            # Envelope.ts_ms
    payload += enc_str(6, kw.get("msg_id", ""))           # Envelope.msg_id
    if body_field is not None:                            # oneof：101+MsgType
        payload += enc_tag(body_field, 2) + enc_varint(len(body or b"")) + (body or b"")
    return struct.pack(">I", len(payload)) + payload      # frame.cpp 大端前缀


def decode_envelope(frame: bytes) -> dict:
    fields = dec_fields(frame)
    env = {
        "type": f_uint(fields, 1),
        "seq": f_uint(fields, 2),
        "from": f_str(fields, 3),
        "to": f_str(fields, 4),
        "msg_id": f_str(fields, 6),
        "body": dec_fields(fields[100 + f_uint(fields, 1)][0][1])
        if (100 + f_uint(fields, 1)) in fields else None,
    }
    return env


# ---- 服务器登录/消息载荷（memex.proto 字段号） ----

def login_body(account: str, password: str, fingerprint: str) -> bytes:
    # Login{account=1, password=2, device_fingerprint=3, device_kind=4,
    #        device_name=5, client_version=6}
    return (enc_str(1, account) + enc_str(2, password) +
            enc_str(3, fingerprint) + enc_str(4, "desktop") +
            enc_str(5, "e2e-local") + enc_str(6, "e2e-local-0.0"))


def text_body(text: str) -> bytes:
    return enc_str(1, text)                               # Text{text=1}


def ack_body(msg_id: str) -> bytes:
    return enc_str(1, msg_id)                             # Ack{msg_id=1}


# ---- 会话 ----

class Client:
    def __init__(self, host: str, port: int, name: str):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(5)
        self.buf = b""
        self.name = name
        self.seq = 0
        self.inbox: dict[int, list[dict]] = {}            # type → 未认领帧

    def send(self, msg_type: int, body_field=None, body=None, to="", **kw) -> int:
        self.seq += 1
        self.sock.sendall(envelope(
            msg_type, body_field, body, seq=self.seq, to=to,
            ts_ms=int(time.time() * 1000), **kw))
        return self.seq

    def recv_frame(self) -> dict:
        """按 frame.cpp FrameDecoder 口径切帧（4B 大端长度前缀）。"""
        while len(self.buf) < 4 or len(self.buf) < 4 + struct.unpack(">I", self.buf[:4])[0]:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError(f"[{self.name}] 服务器断开")
            self.buf += chunk
        n = struct.unpack(">I", self.buf[:4])[0]
        payload, self.buf = self.buf[4:4 + n], self.buf[4 + n:]
        return decode_envelope(payload)

    def expect(self, *types: int, timeout: float = 5.0) -> dict:
        """取指定类型帧；无关推送（PRESENCE/FAV 等）入收件箱跳过。"""
        for t in types:
            if self.inbox.get(t):
                return self.inbox[t].pop(0)
        deadline = time.time() + timeout
        self.sock.settimeout(timeout)
        while time.time() < deadline:
            env = self.recv_frame()
            if env["type"] in types:
                return env
            self.inbox.setdefault(env["type"], []).append(env)
        raise TimeoutError(
            f"[{self.name}] 等待 {TYPE_NAMES.get(types[0])} 超时；"
            f"期间收到 {[TYPE_NAMES.get(t, t) for t in self.inbox]}")

    def login(self, account: str, password: str) -> dict:
        fingerprint = hashlib.sha256(f"e2e-local-{account}".encode()).hexdigest()
        self.send(LOGIN, 100 + LOGIN, login_body(account, password, fingerprint))
        env = self.expect(LOGIN_RESULT)
        lr = env["body"]
        return {"ok": f_bool(lr, 1), "reason": f_str(lr, 2),
                "display_name": f_str(lr, 3)}

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# ---- 走查场景 ----

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=24370)
    ap.add_argument("--account-a", default="e2e_a")
    ap.add_argument("--password-a", default="e2e-pass-a")
    ap.add_argument("--account-b", default="e2e_b")
    ap.add_argument("--password-b", default="e2e-pass-b")
    args = ap.parse_args()

    ok_steps: list[str] = []

    a = Client(args.host, args.port, "A")
    print(f"[1] TCP 已连 {args.host}:{args.port}")

    # 健康检查：登录前 PING→PONG（session.cpp 对 PING 不设登录闸）
    a.send(PING)
    pong = a.expect(PONG)
    assert pong["seq"] == a.seq, f"PONG seq 回带不符：{pong}"
    ok_steps.append(f"PING/PONG seq={pong['seq']}")

    # 负路径：错口令须拒（LOGIN_RESULT.ok=false）
    bad = a.login(args.account_a, "definitely-wrong")
    assert not bad["ok"], f"错口令竟登录成功：{bad}"
    print(f"[2] 错口令被拒：{bad['reason']}")
    ok_steps.append(f"错口令拒绝（{bad['reason']}）")

    # 登录成功
    la = a.login(args.account_a, args.password_a)
    assert la["ok"], f"登录失败：{la}"
    print(f"[3] {args.account_a} 登录成功，展示名={la['display_name']}")
    ok_steps.append(f"登录成功 display_name={la['display_name']}")

    # 联系人/会话面：组织架构成员表
    a.send(ORG_QUERY)
    org = a.expect(ORG_DATA)
    members = [f_str(dec_fields(v[1]), 1) for v in org["body"].get(2, [])]
    assert args.account_a in members and args.account_b in members, \
        f"组织架构缺测试账号：{members}"
    print(f"[4] ORG_DATA 成员：{members}")
    ok_steps.append(f"组织架构成员 {members}")

    # B 上线
    b = Client(args.host, args.port, "B")
    lb = b.login(args.account_b, args.password_b)
    assert lb["ok"], f"登录失败：{lb}"
    print(f"[5] {args.account_b} 登录成功，展示名={lb['display_name']}")

    b.send(PRESENCE_QUERY)
    presence = b.expect(PRESENCE_DATA)
    online = [v[1].decode() for v in presence["body"].get(1, [])]
    assert args.account_a in online and args.account_b in online, \
        f"在线表缺账号：{online}"
    print(f"[6] PRESENCE_DATA 在线表：{online}")
    ok_steps.append(f"在线表 {online}")

    # 消息往返 A→B：A 收 ACK(msg_id)，B 收 TEXT 同 msg_id
    text = f"e2e 往返验证 {time.strftime('%H:%M:%S')}"
    seq = a.send(TEXT, 100 + TEXT, text_body(text), to=args.account_b,
                 **{"from": args.account_a})
    ack = a.expect(ACK)
    assert ack["seq"] == seq and ack["body"], f"ACK 异常：{ack}"
    msg_id = f_str(ack["body"], 1)
    print(f"[7] A 发出「{text}」→ ACK msg_id={msg_id}")

    got = b.expect(TEXT)
    got_text = f_str(got["body"], 1)
    assert got["msg_id"] == msg_id and got_text == text, \
        f"B 收到内容不符：msg_id={got['msg_id']} text={got_text!r}"
    print(f"[8] B 收到同文同 id（msg_id={got['msg_id']}）")
    ok_steps.append(f"A→B 文本往返 msg_id={msg_id}")

    # B ACK → A 收 DELIVER_NOTICE（送达级回执）
    b.send(ACK, 100 + ACK, ack_body(msg_id), **{"from": args.account_b})
    notice = a.expect(DELIVER_NOTICE)
    assert f_str(notice["body"], 1) == msg_id and \
        f_str(notice["body"], 2) == args.account_b, f"送达回执异常：{notice}"
    print(f"[9] A 收 DELIVER_NOTICE（{args.account_b} 已送达）")
    ok_steps.append("DELIVER_NOTICE 送达回执")

    # 反向 B→A
    text2 = f"e2e 反向验证 {time.strftime('%H:%M:%S')}"
    seq2 = b.send(TEXT, 100 + TEXT, text_body(text2), to=args.account_a,
                  **{"from": args.account_b})
    ack2 = b.expect(ACK)
    msg_id2 = f_str(ack2["body"], 1)
    got2 = a.expect(TEXT)
    assert got2["msg_id"] == msg_id2 and f_str(got2["body"], 1) == text2
    a.send(ACK, 100 + ACK, ack_body(msg_id2), **{"from": args.account_a})
    notice2 = b.expect(DELIVER_NOTICE)
    assert f_str(notice2["body"], 1) == msg_id2
    print("[10] B→A 反向往返＋送达回执通过")
    ok_steps.append("B→A 反向往返")

    a.close()
    b.close()
    print("\n=== E2E 全部通过 ===")
    for s in ok_steps:
        print(f"  ✓ {s}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, TimeoutError, ConnectionError, OSError) as e:
        print(f"E2E 失败：{e}", file=sys.stderr)
        sys.exit(1)
