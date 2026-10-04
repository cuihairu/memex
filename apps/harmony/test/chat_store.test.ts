/**
 * 会话存储测试（镜像 Android ChatStoreTest 语义：聚合/去重/未读口径）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { InMemoryChatStore, StoredMessage } from '../core/chat_store';

function msg(peer: string, seq: number, tsMs: number, msgId = '', text = 'hi'): StoredMessage {
  return {
    id: 0,
    peer,
    from: msgId.length > 0 ? 'bob' : 'alice',
    to: peer,
    seq,
    tsMs,
    text,
    source: 'collab',
    msgId,
    recalled: false,
  };
}

test('追加与历史正序（容忍乱序到达）', () => {
  const s = new InMemoryChatStore();
  s.append(msg('bob', 1, 100, 'a', '早'));
  s.append(msg('bob', 2, 300, 'b', '好'));
  s.append(msg('bob', 3, 200, '', '乱序'));
  const hist = s.history('bob');
  assert.deepEqual(hist.map((m) => m.text), ['早', '乱序', '好']);
  assert.deepEqual(hist.map((m) => m.tsMs), [100, 200, 300]);
});

test('msg_id 去重：重复消息幂等忽略', () => {
  const s = new InMemoryChatStore();
  const first = msg('bob', 1, 100, 'dup', 'x');
  assert.equal(s.append(first), true);
  assert.equal(s.append(first), false);
  assert.equal(s.append({ ...first, seq: 99, tsMs: 999 }), false);
  assert.equal(s.history('bob').length, 1);
});

test('未读只计接收（带 msg_id），自己发的不计', () => {
  const s = new InMemoryChatStore();
  s.append(msg('bob', 1, 100, 'm1'));
  s.append(msg('bob', 2, 200, '', '我发的'));
  const convs = s.conversations();
  assert.equal(convs.length, 1);
  assert.equal(convs[0].unread, 1);
  assert.equal(convs[0].lastText, '我发的');
});

test('markRead 清零未读且不删会话', () => {
  const s = new InMemoryChatStore();
  s.append(msg('bob', 1, 100, 'm1'));
  s.markRead('bob');
  assert.equal(s.conversations()[0].unread, 0);
});

test('会话按最近消息倒序', () => {
  const s = new InMemoryChatStore();
  s.append(msg('bob', 1, 100, 'b1'));
  s.append(msg('group:1', 1, 300, 'g1'));
  s.append(msg('carol', 1, 200, 'c1'));
  assert.deepEqual(s.conversations().map((c) => c.peer), ['group:1', 'carol', 'bob']);
});

test('history limit 只取最近 N 条仍正序', () => {
  const s = new InMemoryChatStore();
  for (let i = 0; i < 5; i++) s.append(msg('bob', i + 1, i * 10, 'm' + i));
  const hist = s.history('bob', 2);
  assert.deepEqual(hist.map((m) => m.msgId), ['m3', 'm4']);
});

test('markRecalled 置撤回标记（展示层）', () => {
  const s = new InMemoryChatStore();
  s.append(msg('bob', 1, 100, 'r1'));
  assert.equal(s.markRecalled('r1'), true);
  assert.equal(s.history('bob')[0].recalled, true);
  assert.equal(s.markRecalled('r1'), false); // 已标记过
  assert.equal(s.markRecalled(''), false);
});

test('nextLocalSeq 按 from 单调分配', () => {
  const s = new InMemoryChatStore();
  assert.equal(s.nextLocalSeq('通知'), 1);
  assert.equal(s.nextLocalSeq('通知'), 2);
  assert.equal(s.nextLocalSeq('other'), 1);
});