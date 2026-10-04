/**
 * Memex 帧编解码（镜像 common/src/frame.cpp 的语义，线格式字节一致）：
 * 帧 = 4 字节大端长度前缀 + 载荷（此处载荷为 Envelope 序列化字节）。
 * 长度前缀只描述载荷长度，不含自身 4 字节。
 */
import { ProtocolViolation } from './wire';

export const LENGTH_PREFIX_SIZE = 4;
export const MAX_FRAME_SIZE = 4 * 1024 * 1024;

/** 编码一帧：空载荷或超限抛 ProtocolViolation。 */
export function encodeFrame(payload: Uint8Array): Uint8Array {
  if (payload.length === 0) throw new ProtocolViolation('空载荷：帧长度必须大于 0');
  if (payload.length > MAX_FRAME_SIZE) throw new ProtocolViolation('载荷超限：超过 MAX_FRAME_SIZE');
  const frame = new Uint8Array(LENGTH_PREFIX_SIZE + payload.length);
  const len = payload.length;
  frame[0] = (len >>> 24) & 0xff;
  frame[1] = (len >>> 16) & 0xff;
  frame[2] = (len >>> 8) & 0xff;
  frame[3] = len & 0xff;
  frame.set(payload, LENGTH_PREFIX_SIZE);
  return frame;
}

export enum DecodeStatus {
  OK = 'ok',
  NEED_MORE_DATA = 'need_more_data',
  ZERO_LENGTH = 'zero_length',
  TOO_LARGE = 'too_large',
}

export interface FeedResult {
  status: DecodeStatus;
  frames: Uint8Array[];
}

/** 流式解码器：容忍字节流任意切分（TCP 粘包／半包）。 */
export class FrameDecoder {
  private buffer = new Uint8Array(0);

  /** 追加字节；解出的完整帧随结果返回。 */
  feed(bytes: Uint8Array): FeedResult {
    if (bytes.length > 0) {
      const merged = new Uint8Array(this.buffer.length + bytes.length);
      merged.set(this.buffer, 0);
      merged.set(bytes, this.buffer.length);
      this.buffer = merged;
    }

    const out: Uint8Array[] = [];
    let offset = 0;
    while (true) {
      if (this.buffer.length - offset < LENGTH_PREFIX_SIZE) break;
      const len = readU32Be(this.buffer, offset);
      if (len === 0) return { status: DecodeStatus.ZERO_LENGTH, frames: out };
      if (len > MAX_FRAME_SIZE) return { status: DecodeStatus.TOO_LARGE, frames: out };
      if (this.buffer.length - offset - LENGTH_PREFIX_SIZE < len) break; // 载荷不完整
      out.push(this.buffer.slice(offset + LENGTH_PREFIX_SIZE, offset + LENGTH_PREFIX_SIZE + len));
      offset += LENGTH_PREFIX_SIZE + len;
    }
    if (offset > 0) this.buffer = this.buffer.slice(offset);
    return { status: out.length > 0 ? DecodeStatus.OK : DecodeStatus.NEED_MORE_DATA, frames: out };
  }

  reset(): void {
    this.buffer = new Uint8Array(0);
  }
}

function readU32Be(b: Uint8Array, at: number): number {
  return (
    ((b[at] << 24) >>> 0) +
    ((b[at + 1] << 16) >>> 0) +
    ((b[at + 2] << 8) >>> 0) +
    b[at + 3]
  );
}