/**
 * 长连接会话全链路测试（镜像 apps/android ChatSessionTest：真 socket＋
 * 假服务端，线格式/分发/去重/ACK/通知分级全口径对齐）。断言前一律
 * 有界轮询（until），读侧竞态与 iOS 收口口径一致。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ChatSession, ConnectOutcome } from '../core/session';
import { MemexClient } from '../core/client';
import { InMemoryChatStore } from '../core/chat_store';
import { ServerAddress } from '../core/address';
import { MsgType } from '../core/wire';
import { NoticeGrade } from '../core/format';
import { FakeMemexServer, textFrame, noticeFrame, sleep } from './fake_server';
import { NodeTransport } from './node_transport';
import { RecordingListener, until } from './helpers';

async function newServer(): Promise<FakeMemexServer> {
  const s = new FakeMemexServer();
  await s.start();
  return s;
}

async function connectSession(
  s: FakeMemexServer,
  store: InMemoryChatStore = new InMemoryChatStore(),
  listener: RecordingListener = new RecordingListener(),
): Promise<{ session: ChatSession; outcome: ConnectOutcome; listener: RecordingListener }> {
  const session = new ChatSession(store, 'alice', 'Alice');
  const outcome = await session.connect(
    new NodeTransport(),
    '127.0.0.1',
    s.port,
    'pw',
    'fp',
    'Pixel',
    '0.1.0',
    listener,
  );
  return { session, outcome, listener };
}

test('登录成功且 LOGIN 帧字段完整', async () => {
  const s = await newServer();
  let loginEnvelope: unknown = null;
  s.handler = (env, conn) => {
    if (env.type === MsgType.LOGIN) {
      loginEnvelope = env;
      s.send(conn, {
        type: MsgType.LOGIN_RESULT,
        seq: 1,
        from: 'server',
        to: env.from,
        tsMs: Date.now(),
        msgId: '',
        loginResult: { ok: true, reason: '', displayName: '张三' },
      });
    }
  };
  const { session, outcome } = await connectSession(s);
  assert.equal(outcome.kind, 'ok', JSON.stringify(outcome));
  const e = loginEnvelope as { type: number; login: { account: string; deviceKind: string } };
  assert.equal(e.type, MsgType.LOGIN);
  assert.equal(e.login.account, 'alice');
  assert.equal(e.login.deviceKind, 'mobile');
  session.close();
  await s.close();
});

test('登录被拒带原因且连接关闭', async () => {
  const s = await newServer();
  s.handler = (env, conn) => {
    if (env.type === MsgType.LOGIN) {
      s.send(conn, {
        type: MsgType.LOGIN_RESULT,
        seq: 1,
        from: 'server',
        to: env.from,
        tsMs: Date.now(),
        msgId: '',
        loginResult: { ok: false, reason: '账号不存在', displayName: '' },
      });
    }
  };
  const { session, outcome } = await connectSession(s);
  assert.deepEqual(outcome, { kind: 'rejected', reason: '账号不存在' });
  session.close();
  await s.close();
});

test('发送文本帧线格式对齐桌面端且受理回执 onSent', async () => {
  const s = await newServer();
  let textEnvelope: unknown = null;
  s.handler = (env, conn) => {
    if (env.type === MsgType.LOGIN) {
      s.send(conn, {
        type: MsgType.LOGIN_RESULT,
        seq: 1,
        from: 'server',
        to: env.from,
        tsMs: Date.now(),
        msgId: '',
        loginResult: { ok: true, reason: '', displayName: '张三' },
      });
    } else if (env.type === MsgType.TEXT) {
      textEnvelope = env;
      // 服务端受理回执（对齐 session.cpp：ack 带原 seq、to=发送方）
      s.send(conn, {
        type: MsgType.ACK,
        seq: env.seq,
        from: 'server',
        to: env.from,
        tsMs: Date.now(),
        msgId: '',
      });
    }
  };
  const { session, outcome, listener } = await connectSession(s);
  assert.equal(outcome.kind, 'ok');

  const seq = session.sendText('bob', '你好');
  assert.ok(seq > 0, 'seq 应为正，实得 ' + seq);
  // 发送是异步落网：有界轮询等服务端收到（读侧竞态消除）
  assert.equal(await until(() => textEnvelope !== null), true, '服务端应收到 TEXT 帧');
  const e = textEnvelope as { type: number; seq: number; from: string; to: string; text: { text: string } };
  assert.equal(e.type, MsgType.TEXT);
  assert.equal(e.seq, seq);
  assert.equal(e.from, 'alice');
  assert.equal(e.to, 'bob');
  assert.equal(e.text.text, '你好');
  assert.equal(await until(() => listener.sentSeqs.length === 1), true);
  assert.deepEqual(listener.sentSeqs, [seq]);
  session.close();
  await s.close();
});

test('收到 TEXT 回 ACK(msg_id) 落库并回调 重复补投去重', async () => {
  const s = await newServer();
  const store = new InMemoryChatStore();
  const listener = new RecordingListener();
  const { session, outcome } = await connectSession(s, store, listener);
  assert.equal(outcome.kind, 'ok');

  const conn = s.lastConnection()!;
  const frame = textFrame({ seq: 7, from: 'bob', to: 'alice', msgId: 'sha256:bob:7', text: '在吗' });
  s.send(conn, frame);
  assert.equal(await until(() => listener.messages.length === 1), true);

  // 已回 ACK(msg_id)——有界轮询读侧（对齐 iOS awaitReceived 口径）
  assert.equal(await until(() => s.received.some((e) => e.type === MsgType.ACK)), true);
  const ack = s.received.find((e) => e.type === MsgType.ACK)!;
  assert.equal(ack.ack?.msgId, 'sha256:bob:7');
  assert.equal(ack.to, 'server');

  // 落库且已回调
  assert.deepEqual(listener.messages.map((m) => `${m.peer}:${m.msgId}`), ['bob:sha256:bob:7']);
  const hist = store.history('bob');
  assert.equal(hist.length, 1);
  assert.equal(hist[0].text, '在吗');
  assert.equal(hist[0].msgId, 'sha256:bob:7');

  // 重复补投：去重不重复落库不重复回调，但照回 ACK
  s.send(conn, frame);
  await sleep(120);
  assert.equal(store.history('bob').length, 1);
  assert.equal(listener.messages.length, 1);
  assert.equal(s.received.filter((e) => e.type === MsgType.ACK).length, 2);

  session.close();
  await s.close();
});

test('群消息按 to 归到群会话并回 ACK', async () => {
  const s = await newServer();
  const store = new InMemoryChatStore();
  const listener = new RecordingListener();
  const { session, outcome } = await connectSession(s, store, listener);
  assert.equal(outcome.kind, 'ok');

  const conn = s.lastConnection()!;
  s.send(conn, textFrame({ seq: 3, from: 'bob', to: 'group:9', msgId: 'g1', text: '群消息' }));
  assert.equal(await until(() => listener.messages.length === 1), true);
  const hist = store.history('group:9');
  assert.deepEqual(hist.map((m) => m.peer), ['group:9']);
  assert.deepEqual(hist.map((m) => m.text), ['群消息']);
  assert.equal(await until(() => s.received.some((e) => e.type === MsgType.ACK)), true);
  assert.equal(s.received.find((e) => e.type === MsgType.ACK)!.ack?.msgId, 'g1');

  session.close();
  await s.close();
});

test('KICK 踢下线回调并关闭 关闭后发送不受理', async () => {
  const s = await newServer();
  const listener = new RecordingListener();
  const { session, outcome } = await connectSession(s, new InMemoryChatStore(), listener);
  assert.equal(outcome.kind, 'ok');

  const conn = s.lastConnection()!;
  s.send(conn, {
    type: MsgType.KICK,
    seq: 2,
    from: 'server',
    to: 'alice',
    tsMs: Date.now(),
    msgId: '',
    kick: { reason: '账号已在其他设备登录', replacedBy: 'Pixel 9' },
  });
  assert.equal(await until(() => listener.kicked.length === 1), true);
  assert.deepEqual(listener.kicked, ['账号已在其他设备登录']);
  assert.equal(session.sendText('bob', 'x'), 0);

  session.close();
  await s.close();
});

test('自己发的消息本地立即落库且未读计 0', async () => {
  const s = await newServer();
  const store = new InMemoryChatStore();
  const { session, outcome } = await connectSession(s, store);
  assert.equal(outcome.kind, 'ok');

  session.sendText('bob', '我发的');
  const convs = store.conversations();
  assert.deepEqual(convs.map((c) => c.peer), ['bob']);
  assert.equal(convs[0].lastText, '我发的');
  assert.equal(convs[0].unread, 0); // 自己发的不计未读

  session.close();
  await s.close();
});

test('断线回调 onDisconnected（服务端主动关闭连接）', async () => {
  const s = await newServer();
  const listener = new RecordingListener();
  const { session, outcome } = await connectSession(s, new InMemoryChatStore(), listener);
  assert.equal(outcome.kind, 'ok');
  s.lastConnection()!.destroy();
  assert.equal(await until(() => listener.disconnected.length === 1), true);
  assert.ok(listener.disconnected[0].length > 0);
  // 断开后发送不受理
  assert.equal(session.sendText('bob', 'x'), 0);
  session.close();
  await s.close();
});

test('个人通知落库归档态回 ACK 并回调分级', async () => {
  const s = await newServer();
  const store = new InMemoryChatStore();
  const listener = new RecordingListener();
  const { session, outcome } = await connectSession(s, store, listener);
  assert.equal(outcome.kind, 'ok');

  s.send(s.lastConnection()!, noticeFrame({ to: 'alice', msgId: 'n1', title: '发布', content: '新版本上线' }));
  assert.equal(await until(() => listener.notices.length === 1), true);

  // 归档形态「标题：正文」（compose_notice_text 同源）；peer=from「通知」
  const hist = store.history('通知');
  assert.equal(hist.length, 1);
  assert.equal(hist[0].text, '发布：新版本上线');
  assert.equal(hist[0].msgId, 'n1');
  assert.equal(listener.notices[0].grade, NoticeGrade.IMPORTANT);
  // 已回 ACK(msg_id) 清服务端离线队列；通知不走 onMessage（列表刷新由上层桥接）
  assert.equal(await until(() => s.received.some((e) => e.type === MsgType.ACK)), true);
  assert.equal(s.received.find((e) => e.type === MsgType.ACK)!.ack?.msgId, 'n1');
  assert.equal(listener.messages.length, 0);

  session.close();
  await s.close();
});

test('群通知按 to 归会话 跳转随文留痕 未指定紧急度按普通', async () => {
  const s = await newServer();
  const store = new InMemoryChatStore();
  const listener = new RecordingListener();
  const { session, outcome } = await connectSession(s, store, listener);
  assert.equal(outcome.kind, 'ok');

  s.send(
    s.lastConnection()!,
    noticeFrame({
      to: 'group:9',
      msgId: 'g9',
      title: '会议',
      content: '十点开始',
      urgency: 0,
      jumpUrl: 'https://example.com/a',
    }),
  );
  assert.equal(await until(() => listener.notices.length === 1), true);
  assert.deepEqual(store.history('group:9').map((m) => m.peer), ['group:9']);
  assert.equal(store.history('group:9')[0].text, '会议：十点开始 https://example.com/a');
  assert.equal(listener.notices[0].grade, NoticeGrade.NORMAL);

  session.close();
  await s.close();
});

test('紧急通知分级映射与去重不重复回调但照回 ACK', async () => {
  const s = await newServer();
  const store = new InMemoryChatStore();
  const listener = new RecordingListener();
  const { session, outcome } = await connectSession(s, store, listener);
  assert.equal(outcome.kind, 'ok');

  const frame = noticeFrame({ to: 'alice', msgId: 'u1', title: '安全', content: '异常登录', urgency: 3 });
  s.send(s.lastConnection()!, frame);
  assert.equal(await until(() => listener.notices.length === 1), true);
  s.send(s.lastConnection()!, frame); // 重复补投
  await sleep(120);

  assert.equal(listener.notices.length, 1);
  assert.equal(listener.notices[0].grade, NoticeGrade.URGENT);
  assert.equal(store.history('通知').length, 1);
  // 去重后照回 ACK（服务端按账号清离线队列，重复 ACK 无害）
  assert.equal(await until(() => s.received.filter((e) => e.type === MsgType.ACK).length >= 2), true);

  session.close();
  await s.close();
});

test('probe：PING→PONG 连通校验通过（R17 口径）', async () => {
  const s = await newServer();
  s.handler = (env, conn) => {
    if (env.type === MsgType.PING) {
      s.send(conn, {
        type: MsgType.PONG,
        seq: env.seq,
        from: 'server',
        to: env.from,
        tsMs: Date.now(),
        msgId: '',
      });
    }
  };
  const client = new MemexClient(() => new NodeTransport());
  const r = await client.probe(new ServerAddress('127.0.0.1', s.port));
  assert.equal(r.kind, 'ok');
  assert.ok(r.rttMs >= 0);
  await s.close();
});

test('probe：有应答但不是 PONG → notMemex', async () => {
  const s = await newServer();
  s.handler = (env, conn) => {
    if (env.type === MsgType.PING) {
      s.send(conn, textFrame({ seq: 1, from: 'server', to: 'mobile-setup', text: '你是谁' }));
    }
  };
  const client = new MemexClient(() => new NodeTransport());
  const r = await client.probe(new ServerAddress('127.0.0.1', s.port));
  assert.equal(r.kind, 'notMemex');
  assert.match(r.detail, /PONG/);
  await s.close();
});

test('probe：连接被拒绝 → unreachable', async () => {
  const s = await newServer();
  const port = s.port;
  await s.close(); // 端口已释放，连接必然被拒
  const client = new MemexClient(() => new NodeTransport(), 1000, 500);
  const r = await client.probe(new ServerAddress('127.0.0.1', port));
  assert.equal(r.kind, 'unreachable');
});

test('probe：连上但无应答 → timeout', async () => {
  const s = await newServer();
  s.handler = () => {}; // 不应答
  const client = new MemexClient(() => new NodeTransport(), 1000, 300);
  const r = await client.probe(new ServerAddress('127.0.0.1', s.port));
  assert.equal(r.kind, 'timeout');
  await s.close();
});

test('client.login：成功回填展示名', async () => {
  const s = await newServer();
  const client = new MemexClient(() => new NodeTransport());
  const r = await client.login(new ServerAddress('127.0.0.1', s.port), 'zhangsan', 'pw', 'fp', 'Pixel', '0.1.0');
  assert.equal(r.kind, 'success');
  assert.equal((r as { displayName: string }).displayName, '张三');
  await s.close();
});

test('client.login：被拒带原因', async () => {
  const s = await newServer();
  s.handler = (env, conn) => {
    if (env.type === MsgType.LOGIN) {
      s.send(conn, {
        type: MsgType.LOGIN_RESULT,
        seq: 1,
        from: 'server',
        to: env.from,
        tsMs: Date.now(),
        msgId: '',
        loginResult: {
          ok: false,
          reason: '密码错误',
          displayName: '',
        },
      });
    }
  };
  const client = new MemexClient(() => new NodeTransport());
  const r = await client.login(new ServerAddress('127.0.0.1', s.port), 'zhangsan', 'bad', 'fp', 'Pixel', '0.1.0');
  assert.equal(r.kind, 'rejected');
  assert.equal((r as { reason: string }).reason, '密码错误');
  await s.close();
});