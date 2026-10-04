/**
 * wire 编解码测试：
 * 1) 与 protobufjs（加载 common/proto/memex.proto，单一事实源）双向交叉
 *    验证——我方编出的字节 protobufjs 原样解出，protobufjs 编出的字节
 *    我方原样解出。这是「线格式字节级一致」的独立背书；
 * 2) varint 边界与畸形输入防御；
 * 3) 未知字段/未实现 oneof 分支整体跳过。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as path from 'node:path';
import protobuf from 'protobufjs';
import {
  MsgType,
  NoticeUrgency,
  ProtocolViolation,
  encodeEnvelope,
  decodeEnvelope,
  Envelope,
} from '../core/wire';

// —— protobufjs 参考实现（从 proto 文件加载，绝不写死字段表） ——

const PROTO_PATH = path.resolve(__dirname, '../../../../common/proto/memex.proto');
let EnvelopeType: protobuf.Type;

async function loadProto(): Promise<protobuf.Type> {
  if (EnvelopeType !== undefined) return EnvelopeType;
  // protobufjs 默认即 camelCase JS 属性名（keepCase:false 语义）
  const root = await protobuf.load(PROTO_PATH);
  EnvelopeType = root.lookupType('memex.protocol.v1.Envelope');
  return EnvelopeType;
}

/**
 * 我方 Envelope 形态 → protobufjs 回放对象（同源字段，命名同 camelCase）。
 * canonical proto3 口径：零值标量一律剥除——proto3 隐式存在域不上线，两侧
 * 都省略才字节可比（protobufjs 的 fromObject 对显式空串会照写，非 canonical）。
 */
function toPbJson(env: Envelope): Record<string, unknown> {
  const obj: Record<string, unknown> = {};
  if (env.type !== 0) obj.type = env.type;
  if (env.seq !== 0) obj.seq = env.seq;
  if (env.from !== '') obj.from = env.from;
  if (env.to !== '') obj.to = env.to;
  if (env.tsMs !== 0) obj.tsMs = env.tsMs;
  if (env.msgId !== '') obj.msgId = env.msgId;
  if (env.text !== undefined) {
    const body: Record<string, unknown> = {};
    if (env.text.text !== '') body.text = env.text.text;
    obj.text = body;
  }
  if (env.ack !== undefined) {
    const body: Record<string, unknown> = {};
    if (env.ack.msgId !== '') body.msgId = env.ack.msgId;
    obj.ack = body;
  }
  if (env.login !== undefined) {
    const body: Record<string, unknown> = {};
    if (env.login.account !== '') body.account = env.login.account;
    if (env.login.password !== '') body.password = env.login.password;
    if (env.login.deviceFingerprint !== '') body.deviceFingerprint = env.login.deviceFingerprint;
    if (env.login.deviceKind !== '') body.deviceKind = env.login.deviceKind;
    if (env.login.deviceName !== '') body.deviceName = env.login.deviceName;
    if (env.login.clientVersion !== '') body.clientVersion = env.login.clientVersion;
    obj.login = body;
  }
  if (env.loginResult !== undefined) {
    const body: Record<string, unknown> = {};
    if (env.loginResult.ok) body.ok = true;
    if (env.loginResult.reason !== '') body.reason = env.loginResult.reason;
    if (env.loginResult.displayName !== '') body.displayName = env.loginResult.displayName;
    obj.loginResult = body;
  }
  if (env.kick !== undefined) {
    const body: Record<string, unknown> = {};
    if (env.kick.reason !== '') body.reason = env.kick.reason;
    if (env.kick.replacedBy !== '') body.replacedBy = env.kick.replacedBy;
    obj.kick = body;
  }
  if (env.logout !== undefined) obj.logout = {};
  if (env.notice !== undefined) {
    const body: Record<string, unknown> = {};
    if (env.notice.title !== '') body.title = env.notice.title;
    if (env.notice.content !== '') body.content = env.notice.content;
    if (env.notice.urgency !== 0) body.urgency = env.notice.urgency;
    if (env.notice.jumpUrl !== '') body.jumpUrl = env.notice.jumpUrl;
    obj.notice = body;
  }
  return obj;
}

async function crossCheck(env: Envelope): Promise<void> {
  const T = await loadProto();
  // 我方 → protobufjs
  const mine = encodeEnvelope(env);
  const theirs = T.encode(T.fromObject(toPbJson(env))).finish();
  assert.deepEqual([...mine], [...theirs], '我方编字节应与 protobufjs 全同：' + env.type);
  // protobufjs → 我方
  const back = decodeEnvelope(new Uint8Array(theirs));
  assert.equal(back.type, env.type);
  assert.equal(back.seq, env.seq);
  assert.equal(back.from, env.from);
  assert.equal(back.to, env.to);
  assert.equal(back.tsMs, env.tsMs);
  assert.equal(back.msgId, env.msgId);
}

test('LOGIN 全字段：我方编字节与 protobufjs 全同', async () => {
  await crossCheck({
    type: MsgType.LOGIN,
    seq: 1,
    from: 'alice',
    to: 'server',
    tsMs: 1728000000000,
    msgId: '',
    login: {
      account: 'alice',
      password: 'secret',
      deviceFingerprint: 'sha256fp',
      deviceKind: 'mobile',
      deviceName: 'Pixel 9',
      clientVersion: '0.1.0',
    },
  });
});

test('TEXT 全字段一致性（含中文与 msg_id）', async () => {
  await crossCheck({
    type: MsgType.TEXT,
    seq: 7,
    from: 'bob',
    to: 'group:9',
    tsMs: 1728000123000,
    msgId: 'sha256:bob:7',
    text: { text: '在吗？hello 你好' },
  });
});

test('ACK / LOGIN_RESULT / KICK 一致性', async () => {
  await crossCheck({ type: MsgType.ACK, seq: 8, from: 'alice', to: 'server', tsMs: 1, msgId: '', ack: { msgId: 'sha256:bob:7' } });
  await crossCheck({
    type: MsgType.LOGIN_RESULT,
    seq: 1,
    from: 'server',
    to: 'alice',
    tsMs: 2,
    msgId: '',
    loginResult: { ok: true, reason: '', displayName: '张三' },
  });
  await crossCheck({
    type: MsgType.KICK,
    seq: 2,
    from: 'server',
    to: 'alice',
    tsMs: 3,
    msgId: '',
    kick: { reason: '账号已在其他设备登录', replacedBy: 'Pixel 9' },
  });
});

test('NOTICE 三级 urgency 一致性', async () => {
  await crossCheck({
    type: MsgType.NOTICE,
    seq: 0,
    from: '通知',
    to: 'alice',
    tsMs: 4,
    msgId: 'n1',
    notice: { title: '发布', content: '新版本上线', urgency: NoticeUrgency.IMPORTANT, jumpUrl: '' },
  });
  await crossCheck({
    type: MsgType.NOTICE,
    seq: 0,
    from: '通知',
    to: 'group:9',
    tsMs: 5,
    msgId: 'n2',
    notice: { title: '安全', content: '异常登录', urgency: NoticeUrgency.URGENT, jumpUrl: 'https://e.cn/a' },
  });
});

test('LOGOUT 空 message 照写（oneof presence）', async () => {
  await crossCheck({ type: MsgType.LOGOUT, seq: 9, from: 'alice', to: 'server', tsMs: 6, msgId: '', logout: true });
});

test('空字段不编（proto3 零值省略）', async () => {
  const env = { type: MsgType.PING, seq: 1, from: 'mobile-setup', to: 'server', tsMs: 7, msgId: '' };
  const bytes = encodeEnvelope(env);
  // seq=1 必编、from/to 必编；msgId='' 省略——canonical 对照（剥零值）字节全同
  const T = await loadProto();
  const theirs = T.encode(T.fromObject(toPbJson(env))).finish();
  assert.deepEqual([...bytes], [...theirs]);
  const back = decodeEnvelope(bytes);
  assert.equal(back.tsMs, 7);
  assert.equal(back.msgId, '');
});

// —— varint 边界 ——

test('varint 边界：127/128/300/2^31-1/2^32-1 与 2^53 安全域往返', async () => {
  const vals = [127, 128, 300, 16383, 16384, 2147483647, 4294967295, 9007199254740991];
  for (const ts of vals) {
    const env = { type: MsgType.TEXT, seq: 1, from: 'a', to: 'b', tsMs: ts, msgId: '', text: { text: 'x' } };
    const bytes = encodeEnvelope(env);
    assert.equal(decodeEnvelope(bytes).tsMs, ts);
  }
});

test('编码：非安全整数（超 2^53）抛 ProtocolViolation', () => {
  assert.throws(
    () => encodeEnvelope({ type: MsgType.TEXT, seq: 9007199254740992, from: 'a', to: 'b', tsMs: 0, msgId: '', text: { text: 'x' } }),
    ProtocolViolation,
  );
});

test('解码：64 位负值（int64 补码）可解且回负数', () => {
  // protobufjs 编 seq=-1（int64）：10 字节补码
  const bytes = new Uint8Array([0x10, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01]);
  const env = decodeEnvelope(bytes);
  assert.equal(env.seq, -1);
});

// —— 未知字段跳过 ——

test('未知字段与未实现 oneof 分支整体跳过不崩', () => {
  // Envelope：type=TEXT + 未知 varint 字段 99 + 未实现 oneof hello(101)——
  // 穿插在已知字段间不得干扰解析
  const prefix = new Uint8Array([
    0x08, 0x0a, // type=10（f1 varint）
    0x98, 0x06, 0x01, // f99 varint（未知字段，跳过）
    0xaa, 0x06, 0x02, 0x08, 0x01, // f101 hello len2（未实现 oneof，整体跳过）
    0xf2, 0x06, 0x0e, 0x0a, 0x0c, // f110 text len=14＝子消息头 [0x0a,0x0c] + 12 字节
  ]);
  const textBody = new TextEncoder().encode('你好世界');
  const bytes = new Uint8Array(prefix.length + textBody.length);
  bytes.set(prefix);
  bytes.set(textBody, prefix.length);
  const env = decodeEnvelope(bytes);
  assert.equal(env.type, MsgType.TEXT);
  assert.equal(env.seq, 0);
  assert.equal(env.text?.text, '你好世界');
});

test('未知字段跳过不影响纯未知信封', () => {
  // 只有未知字段（f99 varint / f101 len）——全部返回默认值与未设置 body
  const bytes = new Uint8Array([0x98, 0x06, 0x01, 0xaa, 0x06, 0x02, 0x08, 0x01]);
  const env = decodeEnvelope(bytes);
  assert.equal(env.type, 0);
  assert.equal(env.seq, 0);
  assert.strictEqual(env.text, undefined);
  assert.strictEqual(env.login, undefined);
});

test('畸形：截断的 varint 抛 ProtocolViolation', () => {
  assert.throws(() => decodeEnvelope(new Uint8Array([0x08])), ProtocolViolation);
  assert.throws(() => decodeEnvelope(new Uint8Array([0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, 0x01])), ProtocolViolation);
});

test('畸形：非法 wire type 抛 ProtocolViolation', () => {
  // tag 0x0f：field1 wire type 7（非法）
  assert.throws(() => decodeEnvelope(new Uint8Array([0x0f, 0x01])), ProtocolViolation);
});

test('utf-8 全范围：emoji 与四点码', () => {
  const env = { type: MsgType.TEXT, seq: 1, from: 'a', to: 'b', tsMs: 0, msgId: '', text: { text: '🚀🎉 你' } };
  const bytes = encodeEnvelope(env);
  assert.equal(decodeEnvelope(bytes).text!.text, '🚀🎉 你');
});