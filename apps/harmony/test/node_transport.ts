/**
 * Node 平台 Transport 实现（net 模块）：Node 单测/CI 专用。
 * 鸿蒙壳用 App/entry/src/main/ets/common/HarmonyTransport.ets（@ohos.net.socket）。
 * 用途完全一样：open/write/close + onData/onClose，逐字节收发帧。
 */
import * as net from 'net';
import { Transport, TransportError } from '../core/transport';

export class NodeTransport implements Transport {
  private socket: net.Socket | null = null;
  private closed = false;
  onData: ((chunk: Uint8Array) => void) | null = null;
  onClose: ((cause: string) => void) | null = null;

  open(host: string, port: number, timeoutMs: number): Promise<void> {
    if (this.socket !== null) throw new TransportError('network', '重复建立连接');
    return new Promise((resolve, reject) => {
      const s = new net.Socket();
      s.setNoDelay(true);
      this.socket = s;
      const timer = setTimeout(() => {
        s.destroy();
        reject(new TransportError('timeout', '连接超时'));
      }, timeoutMs);
      s.once('connect', () => {
        clearTimeout(timer);
        resolve();
      });
      s.once('error', (err: NodeJS.ErrnoException) => {
        clearTimeout(timer);
        const code = err.code ?? '';
        const kind =
          code === 'ENOTFOUND' || code === 'EAI_AGAIN'
            ? 'resolve'
            : code === 'ECONNREFUSED'
              ? 'refused'
              : 'network';
        reject(new TransportError(kind, kind === 'resolve' ? '域名无法解析：' + err.message : err.message));
      });
      s.on('data', (buf: Buffer) => {
        if (this.onData !== null) {
          this.onData(new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength));
        }
      });
      s.on('close', () => {
        if (this.closed) return; // 主动 close 不算异常断开
        if (this.onClose !== null) this.onClose('连接被对端关闭');
      });
      s.connect(port, host);
    });
  }

  write(bytes: Uint8Array): void {
    const s = this.socket;
    if (s === null || s.destroyed) throw new TransportError('network', '连接未建立或已关闭');
    s.write(Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength));
  }

  close(): void {
    this.closed = true;
    const s = this.socket;
    if (s !== null) {
      s.destroy();
      this.socket = null;
    }
  }
}