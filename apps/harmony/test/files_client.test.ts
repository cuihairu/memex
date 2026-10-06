/**
 * FilesClient 单测（镜像 Android FilesClientTest 16 用例语义）：
 * node http.createServer 假面记请求形态、回罐头响应；X-File-Name 走
 * 「原始 UTF-8 字节」线口径（服务端发 Latin-1 视角串，客户端还原）。
 */
import { test, beforeEach, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import * as http from 'http';
import {
  FilesClient,
  FilesApiError,
  DEFAULT_FILES_PORT,
  dedupeFileName,
  sanitizeFileName,
  FilesInboxItem,
} from '../core/files_client';
import { NodeHttpFetch } from './node_http_fetch';

/** 假文件服务端：记录请求（含体），按 handler 回包（缺省=登录 ok）。
 *  handler 内断言抛错不炸进程：记入 errors 并回 500（测试侧以 500 失败暴露）。 */
class FakeFilesServer {
  port = 0;
  received: Array<{ method: string; url: string; headers: http.IncomingHttpHeaders; body: Buffer }> = [];
  /** handler 内抛出的断言错误（测试侧可查）。 */
  errors: unknown[] = [];
  /** (req, body) → [status, headers, body]；缺省登录应答。 */
  handler:
    | ((
        req: http.IncomingMessage,
        body: Buffer,
      ) => [number, Record<string, string>, Buffer])
    | null = null;

  private server: http.Server | null = null;

  start(): Promise<void> {
    return new Promise((resolve) => {
      const s = http.createServer((req, res) => {
        const chunks: Buffer[] = [];
        req.on('data', (c: Buffer) => chunks.push(c));
        req.on('end', () => {
          const body = Buffer.concat(chunks);
          this.received.push({ method: req.method ?? '', url: req.url ?? '', headers: req.headers, body });
          const h =
            this.handler ??
            (() => [200, { 'content-type': 'application/json' }, Buffer.from('{"ok":true,"token":"tok-1"}')]);
          let status = 200;
          let headers: Record<string, string> = { 'content-type': 'application/json' };
          let out: Buffer = Buffer.from('{}');
          try {
            [status, headers, out] = h(req, body);
          } catch (e) {
            this.errors.push(e); // 断言失败回 500：客户端侧以非预期状态暴露
            status = 500;
            out = Buffer.from('{"error":"fake handler threw"}');
          }
          res.writeHead(status, headers);
          res.end(out);
        });
      });
      this.server = s;
      s.listen(0, '127.0.0.1', () => {
        const addr = s.address();
        this.port = typeof addr === 'object' && addr !== null ? addr.port : 0;
        resolve();
      });
    });
  }

  stop(): Promise<void> {
    return new Promise((resolve) => {
      this.server?.closeAllConnections?.();
      this.server?.close(() => resolve());
    });
  }

  last() {
    return this.received[this.received.length - 1];
  }

  /** Latin-1 视角串 ← 原始名（服务端发 X-File-Name 的线口径）。 */
  static headerNameOf(name: string): string {
    return Buffer.from(name, 'utf8').toString('latin1');
  }

  /** 服务端收头还原（Latin-1 视角串 → 原始名）。 */
  static restoreHeaderName(value: string): string {
    return Buffer.from(value, 'latin1').toString('utf8');
  }

  /** node:http 客户端专用还原：客户端把 latin1 头值再按 UTF-8 写线
   *  （二次编码，发不出原始字节），这里按其实际行为双重撤销。字节级
   *  「原始 UTF-8 上线」证明归 Android JVM 原生 socket 用例；本用例证明
   *  的是「经会写坏头的宿主栈往返后客户端仍能算出正确文件名/服务端仍
   *  能解码」。ArkTS 壳（@ohos.net.http）实测行为待真机验证。 */
  static restoreAfterNodeClient(value: string): string {
    const once = Buffer.from(value, 'latin1').toString('utf8'); // 撤 UTF-8 再编码
    return Buffer.from(once, 'latin1').toString('utf8'); // 撤 latin1 视角
  }
}

function json(status: number, obj: unknown): [number, Record<string, string>, Buffer] {
  return [status, { 'content-type': 'application/json' }, Buffer.from(JSON.stringify(obj))];
}

let fake: FakeFilesServer;
let client: FilesClient;

beforeEach(async () => {
  fake = new FakeFilesServer();
  await fake.start();
  client = new FilesClient((req) => new NodeHttpFetch().fetch(req));
});

/** 登录到假面（tok-1）。 */
async function loginFirst(): Promise<void> {
  await client.login('127.0.0.1', fake.port, 'alice', 'pw');
  assert.equal(client.isLoggedIn, true);
}

/** 每测收尾：关 server（句柄不吊事件循环）+ 假面断言未静默炸。 */
afterEach(async () => {
  await fake.stop();
  assert.deepEqual(fake.errors, []);
});

test('登录：POST /files/session 免鉴权、后继请求带 Bearer', async () => {
  await loginFirst();
  const first = fake.received[0];
  assert.equal(first.url, '/files/session');
  assert.equal(first.method, 'POST');
  assert.equal(first.headers.authorization, undefined); // 唯一免鉴权路径
  assert.deepEqual(JSON.parse(first.body.toString('utf8')), { account: 'alice', password: 'pw' });

  fake.handler = () => json(200, { ok: true, items: [] });
  await client.listInbox();
  assert.equal(fake.last().headers.authorization, 'Bearer tok-1');
});

test('登录失败 401：状态与服务端文案透出', async () => {
  fake.handler = () => json(401, { error: '账号或口令不正确' });
  await assert.rejects(
    client.login('127.0.0.1', fake.port, 'alice', 'bad'),
    (e: FilesApiError) => e.op === 'session.login' && e.status === 401 && e.message === '账号或口令不正确',
  );
  assert.equal(client.isLoggedIn, false);
});

test('未登录守卫：本地拒（status 0）、请求未发', async () => {
  await assert.rejects(client.listInbox(), (e: FilesApiError) => e.status === 0);
  assert.equal(fake.received.length, 0);
});

test('登出：清 token，后继本地拒', async () => {
  await loginFirst();
  client.logout();
  assert.equal(client.isLoggedIn, false);
  await assert.rejects(client.listInbox(), (e: FilesApiError) => e.status === 0);
});

test('收件箱混排：items 两型解析 + 分页透传', async () => {
  await loginFirst();
  fake.handler = (req) => {
    assert.equal(new URL('http://x' + req.url).searchParams.get('target'), 'inbox');
    return json(200, {
      ok: true,
      items: [
        { type: 'memo', id: 7, content: '备忘', created_ms: 111, updated_ms: 222 },
        { type: 'file', id: 9, file_name: 'a.txt', file_size: 3, file_hash: 'h1', pin: 1, status: 2, upload_ts: 333 },
      ],
    });
  };
  const items = await client.listInbox(50, 10);
  const want: FilesInboxItem[] = [
    { kind: 'memo', id: 7, content: '备忘', createdMs: 111, updatedMs: 222 },
    { kind: 'file', id: 9, fileName: 'a.txt', fileSize: 3, fileHash: 'h1', pin: 1, status: 2, uploadTs: 333 },
  ];
  assert.deepEqual(items, want);
  const q = new URL('http://x' + fake.last().url).searchParams;
  assert.equal(q.get('limit'), '50');
  assert.equal(q.get('offset'), '10');
});

test('个人空间：me 回 files 纯文件数组', async () => {
  await loginFirst();
  fake.handler = () =>
    json(200, {
      ok: true,
      files: [{ id: 5, file_name: 'b.bin', file_size: 9, file_hash: 'h2', pin: 0, status: 0, upload_ts: 444 }],
    });
  const items = await client.listPersonal();
  assert.deepEqual(items, [
    { kind: 'file', id: 5, fileName: 'b.bin', fileSize: 9, fileHash: 'h2', pin: 0, status: 0, uploadTs: 444 },
  ]);
  assert.equal(new URL('http://x' + fake.last().url).searchParams.get('target'), 'me');
});

test('备忘录建改同路由：建不带 id、改带 id', async () => {
  await loginFirst();
  fake.handler = () => json(200, { ok: true, id: 11 });
  assert.equal(await client.createMemo('内容一'), 11);
  assert.equal(fake.last().url, '/files/memo');
  assert.equal(fake.last().method, 'POST');
  let body = JSON.parse(fake.last().body.toString('utf8'));
  assert.equal(body.content, '内容一');
  assert.equal(body.id, undefined);

  assert.equal(await client.updateMemo(11, '内容二'), 11);
  assert.equal(fake.last().url, '/files/memo'); // 同路由
  body = JSON.parse(fake.last().body.toString('utf8'));
  assert.equal(body.id, 11);
  assert.equal(body.content, '内容二');
});

test('备忘录删/列/单条', async () => {
  await loginFirst();
  fake.handler = (req) => {
    const url = req.url ?? '';
    if (req.method === 'DELETE') return json(200, { ok: true });
    if (url.startsWith('/files/memo?limit=')) {
      return json(200, { ok: true, memos: [{ id: 3, content: 'c', created_ms: 1, updated_ms: 2 }] });
    }
    return json(200, { ok: true, memo: { id: 3, content: 'c', created_ms: 1, updated_ms: 2 } });
  };
  await client.deleteMemo(3);
  assert.equal(fake.last().method, 'DELETE');
  assert.equal(fake.last().url, '/files/memo?id=3');

  assert.deepEqual(await client.listMemos(20, 0), [
    { id: 3, content: 'c', createdMs: 1, updatedMs: 2 },
  ]);
  assert.deepEqual(await client.fetchMemo(3), { id: 3, content: 'c', createdMs: 1, updatedMs: 2 });
});

test('上传：octet-stream + X-File-Name 语义往返 + 体原样 + 秒传', async () => {
  await loginFirst();
  fake.handler = (req) => {
    assert.equal(req.headers['content-type'], 'application/octet-stream');
    assert.equal(
      FakeFilesServer.restoreAfterNodeClient(String(req.headers['x-file-name'] ?? '')),
      '报告 终版.txt',
    );
    return json(200, { id: 21, second_transfer: true });
  };
  const payload = new Uint8Array([0x00, 0xff, 0x10, 0x7f]);
  const r = await client.upload('inbox', '报告 终版.txt', payload);
  assert.deepEqual(r, { id: 21, secondTransfer: true });
  assert.equal(fake.last().url, '/files/upload?target=inbox');
  assert.deepEqual([...fake.last().body], [...payload]);
});

test('下载：X-File-Name Latin-1 → UTF-8 还原 + 字节', async () => {
  await loginFirst();
  fake.handler = () => [
    200,
    { 'x-file-name': FakeFilesServer.headerNameOf('报告.txt'), 'content-type': 'application/octet-stream' },
    Buffer.from('hello'),
  ];
  const dl = await client.download(9);
  assert.equal(dl.fileName, '报告.txt');
  assert.equal(Buffer.from(dl.data).toString('utf8'), 'hello');
});

test('下载 404：状态与服务端文案透出', async () => {
  await loginFirst();
  fake.handler = () => json(404, { error: '文件不存在' });
  await assert.rejects(
    client.download(404),
    (e: FilesApiError) => e.op === 'file.download' && e.status === 404 && e.message === '文件不存在',
  );
});

test('删文件：POST /files/manage/delete?id=N', async () => {
  await loginFirst();
  fake.handler = () => json(200, { ok: true });
  await client.deleteFile(33);
  assert.equal(fake.last().method, 'POST');
  assert.equal(fake.last().url, '/files/manage/delete?id=33');
});

test('403：服务端文案透出', async () => {
  await loginFirst();
  fake.handler = () => json(403, { error: '无权删除他人文件' });
  await assert.rejects(
    client.deleteFile(1),
    (e: FilesApiError) => e.status === 403 && e.message === '无权删除他人文件',
  );
});

test('503：对象存储未就绪透出', async () => {
  await loginFirst();
  fake.handler = () => json(503, { error: '对象存储未就绪' });
  await assert.rejects(
    client.upload('inbox', 'x', new Uint8Array([1])),
    (e: FilesApiError) => e.status === 503,
  );
});

test('文件名兜底：去控制字符 + 重名加序号 + IPv6 方括号', () => {
  assert.equal(sanitizeFileName('a\u0000b\u007f.txt '), 'ab.txt');
  assert.equal(sanitizeFileName('\u0001\u0002'), 'download.bin');
  assert.equal(dedupeFileName(['a.txt'], 'a.txt'), 'a-1.txt');
  assert.equal(dedupeFileName(['a.txt', 'a-1.txt'], 'a.txt'), 'a-2.txt');
  assert.equal(dedupeFileName(['b'], 'b'), 'b-1'); // 无扩展名
  assert.equal(FilesClient.baseUrlOf('2001:db8::1', DEFAULT_FILES_PORT), 'http://[2001:db8::1]:24561');
  assert.equal(FilesClient.baseUrlOf('192.168.1.5', 24561), 'http://192.168.1.5:24561');
});
