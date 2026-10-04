/**
 * 单条 TCP 连接的帧通道（Transport + FrameDecoder + wire 编解码），
 * ChatSession 与 MemexClient 共用。Node/ArkTS 两平台同构——平台差异
 * 全部隔离在 Transport 实现里。
 *
 * 帧流消费模式：
 * - nextEnvelope(timeoutMs)：登录/探测阶段等下一帧（队列＋等待者）；
 * - onData 回调：连接建立后的常态分发（ChatSession 读循环语义）。
 */
import {
  decodeEnvelope,
  encodeEnvelope,
  Envelope,
  ProtocolViolation,
  msgTypeName,
} from './wire';
import { encodeFrame, FrameDecoder, DecodeStatus } from './frame_codec';
import { Transport, TransportError } from './transport';

export class WireChannel {
  private decoder = new FrameDecoder();
  private queue: Envelope[] = [];
  private eof: string | null = null; // 对端关闭原因（关闭后 nextEnvelope 抛 eof）
  private waiter: (() => void) | null = null;
  private closed = false;
  /** 常态分发（未挂 onData 时帧入队，由 nextEnvelope 消费）。 */
  onData: ((env: Envelope) => void) | null = null;
  /** 异常断开（含对端关闭）；常态分发挂接后才有意义。 */
  onClose: ((cause: string) => void) | null = null;

  private readonly t: Transport;

  constructor(t: Transport) {
    this.t = t;
    t.onData = (chunk) => this.handleBytes(chunk);
    t.onClose = (cause) => this.handleClose(cause);
  }

  async connect(host: string, port: number, timeoutMs: number): Promise<void> {
    await this.t.open(host, port, timeoutMs);
  }

  send(env: Envelope): void {
    if (this.closed) throw new TransportError('network', '连接已关闭');
    this.t.write(encodeFrame(encodeEnvelope(env)));
  }

  close(): void {
    this.closed = true;
    this.t.close();
  }

  /**
   * 等下一帧：timeoutMs<=0 无限等；超时返回 null；对端在应答前关闭抛
   * TransportError(kind='eof')；帧/载荷非法抛 ProtocolViolation。
   */
  async nextEnvelope(timeoutMs: number): Promise<Envelope | null> {
    for (;;) {
      if (this.queue.length > 0) {
        const env = this.queue[0];
        this.queue.splice(0, 1);
        return env;
      }
      if (this.eof !== null) throw new TransportError('eof', this.eof);
      if (timeoutMs <= 0) {
        await this.waitWake();
        continue;
      }
      const woke = await this.waitWakeWithTimeout(timeoutMs);
      if (!woke) return null; // 超时
    }
  }

  private waitWake(): Promise<void> {
    return new Promise<void>((resolve) => {
      this.waiter = resolve;
    });
  }

  /** resolve(true)=被帧/关闭唤醒；resolve(false)=超时。 */
  private waitWakeWithTimeout(timeoutMs: number): Promise<boolean> {
    return new Promise<boolean>((resolve) => {
      let done = false;
      const timer = setTimeout(() => {
        if (done) return;
        done = true;
        this.waiter = null;
        resolve(false);
      }, timeoutMs);
      this.waiter = () => {
        if (done) return;
        done = true;
        clearTimeout(timer);
        resolve(true);
      };
    });
  }

  private wake(): void {
    const w = this.waiter;
    this.waiter = null;
    if (w !== null) w();
  }

  private handleBytes(chunk: Uint8Array): void {
    if (this.closed) return;
    let res;
    try {
      res = this.decoder.feed(chunk);
    } catch (e) {
      this.handleClose((e as Error).message || '解码异常');
      return;
    }
    if (res.status === DecodeStatus.ZERO_LENGTH || res.status === DecodeStatus.TOO_LARGE) {
      this.handleClose('非法帧：' + res.status);
      return;
    }
    for (const frame of res.frames) {
      let env: Envelope;
      try {
        env = decodeEnvelope(frame);
      } catch (e) {
        if (e instanceof ProtocolViolation) {
          this.handleClose('载荷不是合法的 Envelope 编码');
          return;
        }
        throw e;
      }
      if (this.onData !== null) {
        this.onData(env);
      } else {
        this.queue.push(env);
      }
      this.wake();
    }
  }

  private handleClose(cause: string): void {
    if (this.closed) return;
    this.closed = true;
    if (this.onClose !== null) {
      this.onClose(cause);
      return;
    }
    this.eof = cause.length > 0 ? cause : '连接被对端关闭';
    this.wake();
  }

  /** 诊断辅助：当前已入队未消费的帧类型（测试断言失败时输出）。 */
  queuedTypes(): string[] {
    return this.queue.map((e) => msgTypeName(e.type));
  }
}

/**
 * 传输/协议失败归类（对齐 Android 异常映射）：resolve/refused/network →
 * unreachable；timeout → timeout；eof（应答前关闭）与 ProtocolViolation →
 * notMemex；其余 → notMemex。
 */
export function classifyFailure(e: Error): { kind: 'unreachable' | 'timeout' | 'notMemex'; detail: string } {
  if (e instanceof TransportError) {
    switch (e.kind) {
      case 'resolve':
        return { kind: 'unreachable', detail: e.message };
      case 'refused':
        return { kind: 'unreachable', detail: e.message || '连接被拒绝' };
      case 'network':
        return { kind: 'unreachable', detail: e.message || 'IO 错误' };
      case 'timeout':
        return { kind: 'timeout', detail: e.message || 'timeout' };
      case 'eof':
        return { kind: 'notMemex', detail: '对端在应答前关闭连接' };
    }
  }
  if (e instanceof ProtocolViolation) {
    return { kind: 'notMemex', detail: e.message || '协议不符' };
  }
  return { kind: 'notMemex', detail: e.message || '异常' };
}