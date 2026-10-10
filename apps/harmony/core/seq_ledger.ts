/**
 * 本端 seq 持久化台账（BUG-007 §4.1，镜像 apps/android SeqLedger.kt）：
 * seq 每连接重置会让重登后的新消息与旧档撞 msg_id（服务端已按内容比对
 * 消歧，重排为兜底）——本台账对齐桌面平台-9：账号维度单调续位、发号即写，
 * 常规流保持 sha256(from:seq) 派生式。
 */

/** 本端 seq 台账接口（壳层提供 preferences 落盘实现）。 */
export interface SeqLedger {
  /** 读账号已落盘 seq 上界（无记录返回 0）。 */
  load(account: string): number;
  /** 发号即写（发号方串行调用；实现取 max 合并防乱序回写）。 */
  save(account: string, seq: number): void;
}

/** 内存实现（Node 单测与纯逻辑验证用）。 */
export class InMemorySeqLedger implements SeqLedger {
  private seqs = new Map<string, number>();

  load(account: string): number {
    return this.seqs.get(account) ?? 0;
  }

  save(account: string, seq: number): void {
    if (seq > (this.seqs.get(account) ?? 0)) {
      this.seqs.set(account, seq);
    }
  }
}
