/**
 * 传输抽象：ChatSession/MemexClient 只依赖本接口，平台差异隔离在外——
 * - Node 实现（test/node_transport.ts，net 模块）：Node 单测与 CI 用；
 * - 鸿蒙壳实现（App/entry/src/main/ets/common/HarmonyTransport.ets，
 *   @ohos.net.socket）：ArkTS 侧等价实现。
 * 两实现都必须逐字节收发帧（见 frame_codec），不另造线格式。
 */

/** 传输错误：kind 供上层归类（对齐 Android 异常分类）。 */
export class TransportError extends Error {
  readonly kind: 'resolve' | 'refused' | 'network' | 'timeout' | 'eof';

  constructor(kind: 'resolve' | 'refused' | 'network' | 'timeout' | 'eof', message: string) {
    super(message);
    this.name = 'TransportError';
    this.kind = kind;
  }
}

export interface Transport {
  /** 建立连接（超时/被拒/域名解析失败按 TransportError 抛出）。 */
  open(host: string, port: number, timeoutMs: number): Promise<void>;
  /** 写入字节（未连接/已关闭抛 TransportError）。 */
  write(bytes: Uint8Array): void;
  /** 主动关闭。 */
  close(): void;
  /** 收到字节（帧流；由消费方喂 FrameDecoder）。 */
  onData: ((chunk: Uint8Array) => void) | null;
  /** 连接异常断开（含对端关闭）。 */
  onClose: ((cause: string) => void) | null;
}