/**
 * 单测公用件：有界等待（确定性 bounded waits，不删断言）与回调记录器。
 */
import { NoticeGrade } from '../core/format';
import { ChatSessionListener } from '../core/session';
import { sleep } from './fake_server';

export async function until(pred: () => boolean, timeoutMs = 3000): Promise<boolean> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (pred()) return true;
    await sleep(10);
  }
  return false;
}

/** 记录回调并支持轮询断言（对齐 Android RecordingListener 口径）。 */
export class RecordingListener implements ChatSessionListener {
  messages: Array<{ peer: string; msgId: string; mine: boolean }> = [];
  notices: Array<{ peer: string; grade: NoticeGrade; title: string; msgId: string }> = [];
  sentSeqs: number[] = [];
  kicked: string[] = [];
  disconnected: string[] = [];

  onMessage(peer: string, msgId: string, mine: boolean): void {
    this.messages.push({ peer, msgId, mine });
  }

  onNotice(
    peer: string,
    grade: NoticeGrade,
    title: string,
    _content: string,
    _jumpUrl: string,
    msgId: string,
  ): void {
    this.notices.push({ peer, grade, title, msgId });
  }

  onSent(seq: number): void {
    this.sentSeqs.push(seq);
  }

  onDisconnected(cause: string): void {
    this.disconnected.push(cause);
  }

  onKicked(reason: string): void {
    this.kicked.push(reason);
  }
}