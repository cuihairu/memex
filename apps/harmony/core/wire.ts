/**
 * Memex 协议 wire 编解码（根据 common/proto/memex.proto 手工实现的
 * protobuf wire format 子集——单一事实源仍是 proto 文件，本文件只覆盖
 * 客户端用到的 message 与字段；未知字段一律按 wire 规范跳过，保证
 * 前向解析不崩）。
 *
 * 为什么不用生成器：鸿蒙 ArkTS 生态没有 protoc→ArkTS 官方生成器，且
 * 让纯逻辑层零第三方依赖才能在 Node（CI）与 ArkTS（壳）两侧共用。
 * 字节级一致性由 test/wire.test.ts 用 protobufjs 加载 memex.proto 做
 * 双向交叉验证背书（我方编出的字节 protobufjs 原样解出，反之亦然）。
 *
 * ArkTS 兼容子集：无 any/unknown、对象字面量必须带类型、无解构/rest。
 */

/** 帧类型编号（与 memex.proto MsgType 严格一致，只增不改）。 */
export enum MsgType {
  UNSPECIFIED = 0,   // 未设置（proto MSG_TYPE_UNSPECIFIED）
  HELLO = 1,         // 握手：终端标识与能力
  PING = 2,          // 心跳请求
  PONG = 3,          // 心跳应答
  TEXT = 10,         // 文本消息
  ACK = 11,          // 消息回执
  RECALL = 12,       // 撤回（仅显示层）
  LOGIN = 30,        // 终端 → 服务端：登录
  LOGIN_RESULT = 31, // 服务端 → 终端：登录结果
  KICK = 32,         // 服务端 → 终端：单点互踢
  LOGOUT = 33,       // 终端 → 服务端：主动登出
  NOTICE = 48,       // 通知（webhook 推送，T4.10）
}

/** 通知紧急程度（memex.proto Notice.Urgency 编号）。 */
export const NoticeUrgency = {
  UNSPECIFIED: 0,
  NORMAL: 1,
  IMPORTANT: 2,
  URGENT: 3,
} as const;

/** 帧类型 → 可读名（错误提示用）。 */
export function msgTypeName(t: number): string {
  switch (t) {
    case MsgType.UNSPECIFIED: return 'UNSPECIFIED';
    case MsgType.HELLO: return 'HELLO';
    case MsgType.PING: return 'PING';
    case MsgType.PONG: return 'PONG';
    case MsgType.TEXT: return 'TEXT';
    case MsgType.ACK: return 'ACK';
    case MsgType.RECALL: return 'RECALL';
    case MsgType.LOGIN: return 'LOGIN';
    case MsgType.LOGIN_RESULT: return 'LOGIN_RESULT';
    case MsgType.KICK: return 'KICK';
    case MsgType.LOGOUT: return 'LOGOUT';
    case MsgType.NOTICE: return 'NOTICE';
    default: return 'TYPE_' + t;
  }
}

/** 协议层错误：畸形输入必须报错返回，不得静默吞掉。 */
export class ProtocolViolation extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'ProtocolViolation';
  }
}

// —— 嵌套 message 的字段形态（未设置=undefined） ——

export interface TextMsg {
  text: string;
}

export interface AckMsg {
  msgId: string;
}

export interface LoginMsg {
  account: string;
  password: string;
  deviceFingerprint: string;
  deviceKind: string;
  deviceName: string;
  clientVersion: string;
}

export interface LoginResultMsg {
  ok: boolean;
  reason: string;
  displayName: string;
}

export interface KickMsg {
  reason: string;
  replacedBy: string;
}

export interface NoticeMsg {
  title: string;
  content: string;
  urgency: number;
  jumpUrl: string;
}

/**
 * Envelope 的信封字段 + oneof body（proto3 oneof 有 presence：设置了就
 * 写出，空 message 也照写；未设置=undefined，两者可区分）。
 */
export interface Envelope {
  type: MsgType;
  seq: number;
  from: string;
  to: string;
  tsMs: number;
  msgId: string;
  text?: TextMsg;
  ack?: AckMsg;
  login?: LoginMsg;
  loginResult?: LoginResultMsg;
  kick?: KickMsg;
  logout?: boolean;
  notice?: NoticeMsg;
}

// —— varint ——

const MAX_SAFE = 9007199254740992; // 2^53，JS 精确整数上限

function toVarint(n: number, out: number[]): void {
  if (!Number.isSafeInteger(n)) {
    throw new ProtocolViolation('整数超出 JS 精确表示范围：' + n);
  }
  let v: number = n;
  while (v > 0x7f) {
    out.push((v & 0x7f) | 0x80);
    v = Math.floor(v / 128);
  }
  out.push(v);
}

/** 读 varint：返回值（BigInt 中间态防 64 位溢出）与占用字节数。 */
function readVarint(bytes: Uint8Array, at: number): { value: number; used: number } {
  let acc = BigInt(0);
  let shift = 0;
  for (let i = 0; i < 10; i++) {
    const pos = at + i;
    if (pos >= bytes.length) throw new ProtocolViolation('varint 截断');
    const b = bytes[pos];
    acc |= BigInt(b & 0x7f) << BigInt(shift);
    if ((b & 0x80) === 0) {
      if (i === 9 && (b & 0xfe) !== 0) throw new ProtocolViolation('varint 超过 64 位');
      // 64 位补码还原（int64 负数在 wire 上是 10 字节补码；BigInt 无符号）
      let v: number;
      if (i === 9 && (acc >> 63n) !== 0n) {
        v = Number(acc - (1n << 64n));
      } else {
        v = Number(acc);
      }
      if (v > MAX_SAFE || v < -MAX_SAFE) {
        throw new ProtocolViolation('整数超出 JS 精确表示范围');
      }
      return { value: v, used: i + 1 };
    }
    shift += 7;
  }
  throw new ProtocolViolation('varint 超过 10 字节');
}

function utf8Encode(s: string): Uint8Array {
  if (typeof TextEncoder !== 'undefined') return new TextEncoder().encode(s);
  const out: number[] = [];
  for (const ch of s) {
    const cp = ch.codePointAt(0)!;
    if (cp < 0x80) out.push(cp);
    else if (cp < 0x800) out.push(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f));
    else if (cp < 0x10000) {
      out.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
    } else {
      out.push(
        0xf0 | (cp >> 18),
        0x80 | ((cp >> 12) & 0x3f),
        0x80 | ((cp >> 6) & 0x3f),
        0x80 | (cp & 0x3f),
      );
    }
  }
  return new Uint8Array(out);
}

function utf8Decode(bytes: Uint8Array, at: number, len: number): string {
  if (typeof TextDecoder !== 'undefined') {
    return new TextDecoder().decode(bytes.subarray(at, at + len));
  }
  let s = '';
  const end = at + len;
  let i = at;
  while (i < end) {
    const b = bytes[i];
    if (b < 0x80) {
      s += String.fromCharCode(b);
      i += 1;
    } else if ((b & 0xe0) === 0xc0) {
      s += String.fromCharCode(((b & 0x1f) << 6) | (bytes[i + 1] & 0x3f));
      i += 2;
    } else if ((b & 0xf0) === 0xe0) {
      s += String.fromCharCode(
        ((b & 0x0f) << 12) | ((bytes[i + 1] & 0x3f) << 6) | (bytes[i + 2] & 0x3f),
      );
      i += 3;
    } else if ((b & 0xf8) === 0xf0) {
      const cp =
        ((b & 0x07) << 18) |
        ((bytes[i + 1] & 0x3f) << 12) |
        ((bytes[i + 2] & 0x3f) << 6) |
        (bytes[i + 3] & 0x3f);
      s += String.fromCodePoint(cp);
      i += 4;
    } else {
      throw new ProtocolViolation('非法 UTF-8 字节');
    }
  }
  return s;
}

// —— 信封编码 ——

function writeTag(field: number, wireType: number, out: number[]): void {
  toVarint(field * 8 + wireType, out);
}

function writeVarintField(field: number, v: number, out: number[]): void {
  writeTag(field, 0, out);
  toVarint(v, out);
}

function writeBoolField(field: number, v: boolean, out: number[]): void {
  writeTag(field, 0, out);
  out.push(v ? 1 : 0);
}

function writeStringField(field: number, v: string, out: number[]): void {
  writeTag(field, 2, out);
  const enc = utf8Encode(v);
  toVarint(enc.length, out);
  for (let i = 0; i < enc.length; i++) out.push(enc[i]);
}

function writeBytesField(field: number, payload: number[], out: number[]): void {
  writeTag(field, 2, out);
  toVarint(payload.length, out);
  for (let i = 0; i < payload.length; i++) out.push(payload[i]);
}

// —— 嵌套 message 编码（oneof 分支逐一声明，字段号来自 memex.proto） ——
// canonical proto3：标量零值不上线（对齐 protoc 生成代码的字节形态）。

function encodeTextBody(m: TextMsg, out: number[]): void {
  if (m.text !== '') writeStringField(1, m.text, out);
}

function encodeAckBody(m: AckMsg, out: number[]): void {
  if (m.msgId !== '') writeStringField(1, m.msgId, out);
}

function encodeLoginBody(m: LoginMsg, out: number[]): void {
  if (m.account !== '') writeStringField(1, m.account, out);
  if (m.password !== '') writeStringField(2, m.password, out);
  if (m.deviceFingerprint !== '') writeStringField(3, m.deviceFingerprint, out);
  if (m.deviceKind !== '') writeStringField(4, m.deviceKind, out);
  if (m.deviceName !== '') writeStringField(5, m.deviceName, out);
  if (m.clientVersion !== '') writeStringField(6, m.clientVersion, out);
}

function encodeLoginResultBody(m: LoginResultMsg, out: number[]): void {
  if (m.ok) writeBoolField(1, true, out);
  if (m.reason !== '') writeStringField(2, m.reason, out);
  if (m.displayName !== '') writeStringField(3, m.displayName, out);
}

function encodeKickBody(m: KickMsg, out: number[]): void {
  if (m.reason !== '') writeStringField(1, m.reason, out);
  if (m.replacedBy !== '') writeStringField(2, m.replacedBy, out);
}

function encodeNoticeBody(m: NoticeMsg, out: number[]): void {
  if (m.title !== '') writeStringField(1, m.title, out);
  if (m.content !== '') writeStringField(2, m.content, out);
  if (m.urgency !== 0) writeVarintField(3, m.urgency, out);
  if (m.jumpUrl !== '') writeStringField(4, m.jumpUrl, out);
}

/**
 * 序列化 Envelope（帧载荷）。oneof body 恰好至多一个（互斥，调用方
 * 保证）；proto3 oneof 有 presence——设置了就写，空 message 也照写。
 */
export function encodeEnvelope(env: Envelope): Uint8Array {
  const out: number[] = [];
  if (env.type !== 0) writeVarintField(1, env.type, out);
  if (env.seq !== 0) writeVarintField(2, env.seq, out);
  if (env.from !== '') writeStringField(3, env.from, out);
  if (env.to !== '') writeStringField(4, env.to, out);
  if (env.tsMs !== 0) writeVarintField(5, env.tsMs, out);
  if (env.msgId !== '') writeStringField(6, env.msgId, out);
  if (env.text !== undefined) {
    const inner: number[] = [];
    encodeTextBody(env.text, inner);
    writeBytesField(110, inner, out);
  } else if (env.ack !== undefined) {
    const inner: number[] = [];
    encodeAckBody(env.ack, inner);
    writeBytesField(111, inner, out);
  } else if (env.login !== undefined) {
    const inner: number[] = [];
    encodeLoginBody(env.login, inner);
    writeBytesField(130, inner, out);
  } else if (env.loginResult !== undefined) {
    const inner: number[] = [];
    encodeLoginResultBody(env.loginResult, inner);
    writeBytesField(131, inner, out);
  } else if (env.kick !== undefined) {
    const inner: number[] = [];
    encodeKickBody(env.kick, inner);
    writeBytesField(132, inner, out);
  } else if (env.logout !== undefined) {
    writeBytesField(133, [], out);
  } else if (env.notice !== undefined) {
    const inner: number[] = [];
    encodeNoticeBody(env.notice, inner);
    writeBytesField(148, inner, out);
  }
  return new Uint8Array(out);
}

// —— 信封解码：通用 walker（未知字段按 wire 规范跳过） ——

const WIRE_VARINT = 0;
const WIRE_FIXED64 = 1;
const WIRE_LENGTH_DELIMITED = 2;
const WIRE_FIXED32 = 5;

/**
 * 遍历 message 字节：回调收到「字段号、wire type、varint 值（wt=0）、
 * 载荷起止（wt=2 的 payload 区间 / wt=1、5 的定长区间）」。未知字段
 * 不需要回调消费——walk 统一跳过并推进游标。
 */
function walk(
  body: Uint8Array,
  visit: (field: number, wt: number, varintValue: number, at: number, len: number) => void,
): void {
  let at = 0;
  while (at < body.length) {
    const t = readVarint(body, at);
    at += t.used;
    const field = Math.floor(t.value / 8);
    const wt = t.value % 8;
    switch (wt) {
      case WIRE_VARINT: {
        const v = readVarint(body, at);
        at += v.used;
        visit(field, wt, v.value, 0, 0);
        break;
      }
      case WIRE_LENGTH_DELIMITED: {
        const len = readVarint(body, at);
        at += len.used;
        visit(field, wt, 0, at, len.value);
        at += len.value;
        break;
      }
      case WIRE_FIXED64:
        visit(field, wt, 0, at, 8);
        at += 8;
        break;
      case WIRE_FIXED32:
        visit(field, wt, 0, at, 4);
        at += 4;
        break;
      default:
        throw new ProtocolViolation('非法 wire type：' + wt);
    }
    if (at > body.length) throw new ProtocolViolation('载荷越界');
  }
}

function strAt(body: Uint8Array, at: number, len: number): string {
  return utf8Decode(body, at, len);
}

/** 解析 Envelope（未知字段与未实现的 oneof 分支整体跳过）。 */
export function decodeEnvelope(bytes: Uint8Array): Envelope {
  const env: Envelope = { type: MsgType.UNSPECIFIED, seq: 0, from: '', to: '', tsMs: 0, msgId: '' };
  walk(bytes, (field, wt, _v, at, len) => {
    switch (field) {
      case 1:
        if (wt === WIRE_VARINT) env.type = _v;
        break;
      case 2:
        if (wt === WIRE_VARINT) env.seq = _v;
        break;
      case 5:
        if (wt === WIRE_VARINT) env.tsMs = _v;
        break;
      case 3:
        if (wt === WIRE_LENGTH_DELIMITED) env.from = strAt(bytes, at, len);
        break;
      case 4:
        if (wt === WIRE_LENGTH_DELIMITED) env.to = strAt(bytes, at, len);
        break;
      case 6:
        if (wt === WIRE_LENGTH_DELIMITED) env.msgId = strAt(bytes, at, len);
        break;
      case 110:
        if (wt === WIRE_LENGTH_DELIMITED) env.text = decodeText(bytes.subarray(at, at + len));
        break;
      case 111:
        if (wt === WIRE_LENGTH_DELIMITED) env.ack = decodeAck(bytes.subarray(at, at + len));
        break;
      case 130:
        if (wt === WIRE_LENGTH_DELIMITED) env.login = decodeLogin(bytes.subarray(at, at + len));
        break;
      case 131:
        if (wt === WIRE_LENGTH_DELIMITED) {
          env.loginResult = decodeLoginResult(bytes.subarray(at, at + len));
        }
        break;
      case 132:
        if (wt === WIRE_LENGTH_DELIMITED) env.kick = decodeKick(bytes.subarray(at, at + len));
        break;
      case 133:
        if (wt === WIRE_LENGTH_DELIMITED) env.logout = true;
        break;
      case 148:
        if (wt === WIRE_LENGTH_DELIMITED) {
          env.notice = decodeNotice(bytes.subarray(at, at + len));
        }
        break;
      default:
        break; // 未知 / 未实现的 oneof 分支整体跳过
    }
  });
  return env;
}

function decodeText(body: Uint8Array): TextMsg {
  const m: TextMsg = { text: '' };
  walk(body, (field, wt, _v, at, len) => {
    if (field === 1 && wt === WIRE_LENGTH_DELIMITED) m.text = strAt(body, at, len);
  });
  return m;
}

function decodeAck(body: Uint8Array): AckMsg {
  const m: AckMsg = { msgId: '' };
  walk(body, (field, wt, _v, at, len) => {
    if (field === 1 && wt === WIRE_LENGTH_DELIMITED) m.msgId = strAt(body, at, len);
  });
  return m;
}

function decodeLogin(body: Uint8Array): LoginMsg {
  const m: LoginMsg = {
    account: '', password: '', deviceFingerprint: '',
    deviceKind: '', deviceName: '', clientVersion: '',
  };
  walk(body, (field, wt, _v, at, len) => {
    if (wt !== WIRE_LENGTH_DELIMITED) return;
    switch (field) {
      case 1: m.account = strAt(body, at, len); break;
      case 2: m.password = strAt(body, at, len); break;
      case 3: m.deviceFingerprint = strAt(body, at, len); break;
      case 4: m.deviceKind = strAt(body, at, len); break;
      case 5: m.deviceName = strAt(body, at, len); break;
      case 6: m.clientVersion = strAt(body, at, len); break;
      default: break;
    }
  });
  return m;
}

function decodeLoginResult(body: Uint8Array): LoginResultMsg {
  const m: LoginResultMsg = { ok: false, reason: '', displayName: '' };
  walk(body, (field, wt, _v, at, len) => {
    if (wt === WIRE_VARINT) {
      if (field === 1) m.ok = _v !== 0;
    } else if (wt === WIRE_LENGTH_DELIMITED) {
      if (field === 2) m.reason = strAt(body, at, len);
      if (field === 3) m.displayName = strAt(body, at, len);
    }
  });
  return m;
}

function decodeKick(body: Uint8Array): KickMsg {
  const m: KickMsg = { reason: '', replacedBy: '' };
  walk(body, (field, wt, _v, at, len) => {
    if (wt !== WIRE_LENGTH_DELIMITED) return;
    if (field === 1) m.reason = strAt(body, at, len);
    if (field === 2) m.replacedBy = strAt(body, at, len);
  });
  return m;
}

function decodeNotice(body: Uint8Array): NoticeMsg {
  const m: NoticeMsg = { title: '', content: '', urgency: 0, jumpUrl: '' };
  walk(body, (field, wt, _v, at, len) => {
    if (wt === WIRE_VARINT) {
      if (field === 3) m.urgency = _v;
    } else if (wt === WIRE_LENGTH_DELIMITED) {
      if (field === 1) m.title = strAt(body, at, len);
      if (field === 2) m.content = strAt(body, at, len);
      if (field === 4) m.jumpUrl = strAt(body, at, len);
    }
  });
  return m;
}

/** 便捷构造：一帧文本（发送路径，字段与桌面/移动端一致）。 */
export function textEnvelope(
  seq: number,
  from: string,
  to: string,
  text: string,
  tsMs: number,
  msgId: string = '',
): Envelope {
  return { type: MsgType.TEXT, seq, from, to, tsMs, msgId, text: { text } };
}

/** 便捷构造：ACK 回执（msg_id 收取确认）。 */
export function ackEnvelope(
  seq: number,
  from: string,
  to: string,
  msgId: string,
  tsMs: number,
): Envelope {
  return { type: MsgType.ACK, seq, from, to, tsMs, msgId: '', ack: { msgId } };
}

/** 便捷构造：LOGIN 帧（device_kind 固定 mobile，与 Android/iOS 一致）。 */
export function loginEnvelope(
  account: string,
  password: string,
  deviceFingerprint: string,
  deviceName: string,
  clientVersion: string,
  tsMs: number,
): Envelope {
  return {
    type: MsgType.LOGIN,
    seq: 1,
    from: account,
    to: 'server',
    tsMs,
    msgId: '',
    login: {
      account,
      password,
      deviceFingerprint,
      deviceKind: 'mobile',
      deviceName,
      clientVersion,
    },
  };
}

/** 便捷构造：LOGOUT 帧。 */
export function logoutEnvelope(seq: number, from: string, tsMs: number): Envelope {
  return { type: MsgType.LOGOUT, seq, from, to: 'server', tsMs, msgId: '', logout: true };
}

/** 便捷构造：PING（连通性校验，对齐 MemexClient.probe：from=mobile-setup）。 */
export function pingEnvelope(tsMs: number): Envelope {
  return { type: MsgType.PING, seq: 1, from: 'mobile-setup', to: 'server', tsMs, msgId: '' };
}