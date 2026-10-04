/**
 * 假 Memex 服务端（Node net，loopback TCP，线格式与 server 端一致）：
 * 登录应答、记录收发帧、支持测试主动注入帧。对齐 Android ChatSessionTest
 * 的 FakeMemexServer 模式。
 */
import * as net from 'net';
import { FrameDecoder, encodeFrame } from '../core/frame_codec';
import { decodeEnvelope, encodeEnvelope, Envelope, MsgType } from '../core/wire';

export class FakeMemexServer {
  port = 0;
  /** 客户端发来的全部帧（按到达序）。 */
  received: Envelope[] = [];
  /** 服务端主动发送的帧（按序）。 */
  sent: Envelope[] = [];
  /** 存活连接（测试主动注入帧用）。 */
  connections: net.Socket[] = [];

  /** 每帧应答钩子（null=默认：LOGIN→LOGIN_RESULT ok）。 */
  handler: ((env: Envelope, conn: net.Socket) => void) | null = null;

  private server: net.Server | null = null;
  private decoders = new Map<net.Socket, FrameDecoder>();

  start(): Promise<void> {
    return new Promise((resolve) => {
      const server = net.createServer((conn) => {
        this.connections.push(conn);
        const decoder = new FrameDecoder();
        this.decoders.set(conn, decoder);
        conn.on('data', (chunk: Buffer) => this.handleBytes(conn, decoder, chunk));
        conn.on('error', () => this.dropConn(conn));
        conn.on('close', () => this.dropConn(conn));
      });
      this.server = server;
      server.listen(0, '127.0.0.1', () => {
        const addr = server.address();
        this.port = typeof addr === 'object' && addr !== null ? addr.port : 0;
        resolve();
      });
    });
  }

  private handleBytes(conn: net.Socket, decoder: FrameDecoder, chunk: Buffer): void {
    const res = decoder.feed(new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength));
    for (const frame of res.frames) {
      const env = decodeEnvelope(frame);
      this.received.push(env);
      const h = this.handler;
      if (h !== null) {
        h(env, conn);
      } else {
        this.defaultHandler(env, conn);
      }
    }
  }

  private defaultHandler(env: Envelope, conn: net.Socket): void {
    if (env.type === MsgType.LOGIN) {
      this.send(conn, {
        type: MsgType.LOGIN_RESULT,
        seq: 1,
        from: 'server',
        to: env.from,
        tsMs: Date.now(),
        msgId: '',
        loginResult: { ok: true, reason: '', displayName: '张三' },
      });
    }
  }

  /** 服务端发帧（记账后写入）。 */
  send(conn: net.Socket, env: Envelope): void {
    this.sent.push(env);
    conn.write(frameOf(env));
  }

  /** 当前存活连接（测试注入用；未接受过连接为 null）。 */
  lastConnection(): net.Socket | null {
    for (let i = this.connections.length - 1; i >= 0; i--) {
      if (this.connections[i].readyState === 'open') return this.connections[i];
    }
    return null;
  }

  /** 等待条件成立（有界；测试侧读侧竞态消除——对齐 iOS awaitReceived 口径）。 */
  async until(pred: () => boolean, timeoutMs = 3000): Promise<boolean> {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      if (pred()) return true;
      await sleep(10);
    }
    return false;
  }

  close(): Promise<void> {
    for (const c of this.connections) c.destroy();
    this.connections = [];
    const s = this.server;
    this.server = null;
    if (s === null) return Promise.resolve();
    return new Promise((resolve) => s.close(() => resolve()));
  }

  private dropConn(conn: net.Socket): void {
    this.decoders.delete(conn);
    const idx = this.connections.indexOf(conn);
    if (idx >= 0) this.connections.splice(idx, 1);
  }
}

export function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

export function frameOf(env: Envelope): Buffer {
  return Buffer.from(encodeFrame(encodeEnvelope(env)));
}

/** 组一帧服务端推送（对齐 Android ChatSessionTest 的 textFrame/noticeFrame）。 */
export function textFrame(
  opts: { seq: number; from: string; to: string; msgId?: string; text?: string },
): Envelope {
  return {
    type: MsgType.TEXT,
    seq: opts.seq,
    from: opts.from,
    to: opts.to,
    tsMs: Date.now(),
    msgId: opts.msgId ?? '',
    text: { text: opts.text ?? '' },
  };
}

/** 组一帧服务端通知（对齐 webhook deliver_notice：from=「通知」、msg_id 必带、seq=0）。 */
export function noticeFrame(
  opts: { to: string; msgId: string; title: string; content: string; urgency?: number; jumpUrl?: string },
): Envelope {
  return {
    type: MsgType.NOTICE,
    seq: 0,
    from: '通知',
    to: opts.to,
    tsMs: Date.now(),
    msgId: opts.msgId,
    notice: {
      title: opts.title,
      content: opts.content,
      urgency: opts.urgency ?? 2,
      jumpUrl: opts.jumpUrl ?? '',
    },
  };
}