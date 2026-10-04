/**
 * 会话消息模型（镜像 apps/android ChatStore.kt，语义对齐桌面 local_store）：
 * - peer 为会话维度（单聊=对端账号，群聊="group:<群号>"），与消息 frame 的
 *   from/to 无关（自己是发件人时 to 才是 peer）；
 * - source="collab"（移动端全部为协作态，R18）；
 * - msg_id 服务端生成（sha256(from:seq)）用于收方去重与 ACK；
 * - recalled 仅展示层，原文保留（桌面端同语义）。
 */

export interface StoredMessage {
  id: number;
  peer: string;
  from: string;
  to: string;
  seq: number;
  tsMs: number;
  text: string;
  source: string;
  msgId: string;
  recalled: boolean;
}

/** 会话列表条目（按 peer 聚合：最后一条消息预览＋未读数）。 */
export interface Conversation {
  peer: string;
  lastTsMs: number;
  lastText: string;
  unread: number;
}

/** 本地消息存储接口（UI 与收发逻辑只依赖本接口；鸿蒙壳后续可换实现）。 */
export interface ChatStore {
  /** 幂等追加（msg_id 重复忽略）。返回是否真正插入（false=重复消息）。 */
  append(msg: StoredMessage): boolean;
  /** 某会话的本地历史：最近 limit 条按时间正序（聊天窗渲染方向）。 */
  history(peer: string, limit?: number): StoredMessage[];
  /** 会话列表：有历史的对端按最近消息时间倒序（含未读数）。 */
  conversations(): Conversation[];
  /** 置已读（清零未读，仅本端展示用）。 */
  markRead(peer: string): void;
  /** 按 msg_id 置撤回标记（RECALL 事件到达后调用）。 */
  markRecalled(msgId: string): boolean;
  /** 本端单调 seq（服务端起源消息用的本地序号分配，对齐桌面 next_local_seq）。 */
  nextLocalSeq(from: string): number;
}

/**
 * 内存实现（Node 单测与纯逻辑验证用；与未来持久化实现共用全部聚合/
 * 去重逻辑口径）。消息列表按 (peer, tsMs, id) 保序。
 */
export class InMemoryChatStore implements ChatStore {
  private store = new Map<string, StoredMessage[]>();
  private unread = new Map<string, number>();
  private msgIds = new Set<string>();
  private nextId = 1;
  private localSeqs = new Map<string, number>();

  append(msg: StoredMessage): boolean {
    if (msg.msgId.length > 0 && this.msgIds.has(msg.msgId)) return false;
    if (msg.msgId.length > 0) this.msgIds.add(msg.msgId);
    // id 由存储分配，调用方传 0 无意义（ArkTS 不支持对象展开，显式复制）
    const stored: StoredMessage = {
      id: this.nextId++,
      peer: msg.peer,
      from: msg.from,
      to: msg.to,
      seq: msg.seq,
      tsMs: msg.tsMs,
      text: msg.text,
      source: msg.source,
      msgId: msg.msgId,
      recalled: msg.recalled,
    };
    let list = this.store.get(msg.peer);
    if (list === undefined) {
      list = [];
      this.store.set(msg.peer, list);
    }
    list.push(stored);
    // 未读只对「收到的消息」计（带服务端 msg_id；自己刚发的 msg_id 为空）
    if (msg.msgId.length > 0) this.unread.set(msg.peer, (this.unread.get(msg.peer) ?? 0) + 1);
    return true;
  }

  history(peer: string, limit = 200): StoredMessage[] {
    const list = this.store.get(peer);
    if (list === undefined) return [];
    // 先按 (tsMs,id) 全序（容忍乱序到达），取最近 limit 条并保持正序
    const sorted = [...list].sort((a, b) => (a.tsMs - b.tsMs) || (a.id - b.id));
    return sorted.slice(-limit);
  }

  conversations(): Conversation[] {
    const out: Conversation[] = [];
    this.store.forEach((list, peer) => {
      let last: StoredMessage | null = null;
      for (const m of list) {
        if (last === null || m.tsMs > last.tsMs || (m.tsMs === last.tsMs && m.id > last.id)) {
          last = m;
        }
      }
      if (last !== null) {
        out.push({ peer, lastTsMs: last.tsMs, lastText: last.text, unread: this.unread.get(peer) ?? 0 });
      }
    });
    out.sort((a, b) => b.lastTsMs - a.lastTsMs);
    return out;
  }

  markRead(peer: string): void {
    this.unread.set(peer, 0);
  }

  markRecalled(msgId: string): boolean {
    if (msgId.length === 0) return false;
    let found = false;
    this.store.forEach((list) => {
      for (const m of list) {
        if (m.msgId === msgId && !m.recalled) {
          m.recalled = true;
          found = true;
        }
      }
    });
    return found;
  }

  nextLocalSeq(from: string): number {
    const cur = this.localSeqs.get(from) ?? 0;
    this.localSeqs.set(from, cur + 1);
    return cur + 1;
  }
}