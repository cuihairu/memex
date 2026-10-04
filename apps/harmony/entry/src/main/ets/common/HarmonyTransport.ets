/**
 * 鸿蒙平台 Transport 实现（@ohos.net.socket TCPSocket）：
 * 与 Node 实现（core 的 NodeTransport，仅测试用）完全同构——
 * open/write/close + onData/onClose，逐字节收发帧，不另造线格式。
 * 连接超时由本类自管计时器；数据/关闭事件直投 @memex/core 的 WireChannel。
 */
import socket from '@ohos.net.socket';
import { BusinessError } from '@ohos.base';
import { Transport, TransportError } from '@memex/core';

export class HarmonyTransport implements Transport {
  onData: ((chunk: Uint8Array) => void) | null = null;
  onClose: ((cause: string) => void) | null = null;
  private tcp: socket.TCPSocket | null = null;
  private closed = true;

  open(host: string, port: number, timeoutMs: number): Promise<void> {
    if (this.tcp !== null) {
      return Promise.reject(new TransportError('network', '重复建立连接'));
    }
    this.closed = false;
    return new Promise<void>((resolve, reject) => {
      const t: socket.TCPSocket = socket.constructTCPSocketInstance();
      this.tcp = t;
      let settled = false;

      const fail = (kind: 'network' | 'timeout', message: string): void => {
        if (settled) {
          return;
        }
        settled = true;
        clearTimeout(timer);
        this.teardown();
        reject(new TransportError(kind, message));
      };

      const timer = setTimeout(() => {
        fail('timeout', '连接超时');
      }, timeoutMs);

      t.on('connect', () => {
        if (settled) {
          return;
        }
        settled = true;
        clearTimeout(timer);
        resolve();
      });
      t.on('message', (value: socket.SocketMessageInfo) => {
        if (this.onData === null) {
          return;
        }
        const chunk = new Uint8Array(value.message);
        if (chunk.length > 0) {
          this.onData(chunk);
        }
      });
      t.on('close', () => {
        if (this.closed) {
          return; // 主动 close 不算异常断开
        }
        this.closed = true;
        if (this.onClose !== null) {
          this.onClose('连接被对端关闭');
        }
      });
      t.on('error', (err: BusinessError) => {
        fail('network', err.message !== undefined ? err.message : 'socket 错误');
      });

      // 鸿蒙 TCPSocket：connect 前须先 bind 本地地址（port=0 随机）
      const local: socket.NetAddress = { address: '0.0.0.0', port: 0, family: 1 };
      t.bind(local, (bindErr: BusinessError) => {
        if (bindErr !== undefined && bindErr !== null && bindErr.code !== 0) {
          fail('network', 'bind 失败：' + (bindErr.message !== undefined ? bindErr.message : ''));
          return;
        }
        const remote: socket.SocketRemoteInfo = { address: host, port: port };
        t.connect(remote, (connErr: BusinessError) => {
          if (connErr !== undefined && connErr !== null && connErr.code !== 0) {
            fail('network', '连接失败：' + (connErr.message !== undefined ? connErr.message : ''));
          }
          // 成功路径由 'connect' 事件 resolve
        });
      });
    });
  }

  write(bytes: Uint8Array): void {
    const t = this.tcp;
    if (t === null || this.closed) {
      throw new TransportError('network', '连接未建立或已关闭');
    }
    // Uint8Array 的 buffer 即帧字节（视图零拷贝）；send 只接受 ArrayBuffer
    t.send({ data: bytes.buffer });
  }

  close(): void {
    this.closed = true;
    this.teardown();
  }

  private teardown(): void {
    const t = this.tcp;
    this.tcp = null;
    if (t !== null) {
      t.close((err: BusinessError) => {
        // 主动关闭失败无补救路径：socket 随引用释放
      });
    }
  }
}
