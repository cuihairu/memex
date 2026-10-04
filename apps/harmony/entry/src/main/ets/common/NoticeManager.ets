/**
 * 消息推送横幅＋震动（对齐 apps/android NotificationHelper.kt，桌面
 * T4.10 三级通知语义）：
 * - TEXT 消息：仅应用在后台（或非当前打开会话）时横幅——前台站内渲染
 *   已覆盖，不重复打扰（移动端适配）；
 * - NOTICE 普通（NORMAL）：仅站内会话消息（已落库渲染），不弹——对齐
 *   桌面「普通通知弹窗默认关＝仅站内会话消息」；
 * - NOTICE 重要（IMPORTANT）：横幅＋震动强提醒，不看应用前后台——
 *   对齐桌面「重要通知桌面提醒（强提醒，不看窗口激活态）」；
 * - NOTICE 紧急（URGENT）：前台→应用内置顶确认弹窗（未确认的继续排队
 *   弹出）；后台→常驻横幅（isOngoing 不可滑除）＋震动；确认收悉后横幅
 *   才消失。后台动作按钮经 wantAgent 拉起的路径待 OHOS runner 后补。
 *
 * 通知授权：requestEnableNotification 由主界面引导（用户拒绝→静默降级
 * 为站内，不阻塞主流程）。
 */
import notificationManager from '@ohos.notificationManager';
import vibrator from '@ohos.vibrator';
import { BusinessError } from '@ohos.base';
import common from '@ohos.app.ability.common';
import { ChatStore, NoticeGrade, StoredMessage } from '@memex/core';

/** 紧急通知待确认条目（应用内弹窗与后台常驻横幅共用）。 */
export interface UrgentNotice {
  msgId: string;
  title: string;
  content: string;
  peer: string;
}

export class NoticeManager {
  private readonly store: ChatStore;
  private seqId = 1;
  /** 紧急通知待确认队列（未确认继续排队弹出——对齐桌面 T4.10）。 */
  private urgentQueue: UrgentNotice[] = [];
  /** 前台紧急确认弹窗回调（主界面注册；未注册时滞留队列待回前台再弹）。 */
  private urgentPrompt: ((n: UrgentNotice) => void) | null = null;

  constructor(store: ChatStore) {
    this.store = store;
  }

  setUrgentPrompt(fn: ((n: UrgentNotice) => void) | null): void {
    this.urgentPrompt = fn;
    if (fn !== null) {
      this.pumpUrgent();
    }
  }

  /** 建通知 slot（幂等；应用启动/登录成功后调一次）。 */
  ensureChannels(): void {
    notificationManager.addSlot(
      notificationManager.SlotType.SOCIAL_COMMUNICATION,
      (err: BusinessError) => {
        // 已存在/失败均不阻塞（幂等声明式 slot）
      },
    );
  }

  /** 通知授权引导；拒绝则静默降级为站内（横幅不弹，消息照常落库）。 */
  requestEnable(context: common.UIAbilityContext): void {
    notificationManager
      .requestEnableNotification(context)
      .then(() => {})
      .catch(() => {});
  }

  /** TEXT 消息横幅裁决（bridge 在事件线程直投；调用方传入前后台状态）。 */
  onNewMessage(peer: string, foreground: boolean, openPeer: string): void {
    if (foreground && peer === openPeer) {
      return; // 正看该会话，站内渲染即可
    }
    const hist: StoredMessage[] = this.store.history(peer, 1);
    if (hist.length === 0) {
      return;
    }
    const last = hist[hist.length - 1];
    if (last.msgId.length === 0) {
      return; // 自己刚发的受理暂存不推
    }
    this.publishBasic(this.notifId(last.msgId), peerDisplayName(peer), last.text, false);
  }

  /** NOTICE 三级裁决（grade 已由 core 按 proto urgency 映射）。 */
  onNotice(
    peer: string,
    grade: NoticeGrade,
    title: string,
    content: string,
    jumpUrl: string,
    msgId: string,
    foreground: boolean,
  ): void {
    if (grade === NoticeGrade.NORMAL) {
      return; // 普通＝仅站内会话消息，不弹（桌面默认偏好同）
    }
    if (grade === NoticeGrade.IMPORTANT) {
      this.publishBasic(this.notifId(msgId), title, content, false);
      this.vibrate();
      return;
    }
    // URGENT：前台→应用内确认弹窗（排队）；后台→常驻横幅＋震动
    const info: UrgentNotice = { msgId: msgId, title: title, content: content, peer: peer };
    if (foreground) {
      this.urgentQueue.push(info);
      this.pumpUrgent();
    } else {
      this.urgentQueue.push(info);
      this.publishBasic(this.notifId(msgId), title, content, true);
      this.vibrate();
    }
  }

  /** 确认收悉：撤销常驻横幅并弹队列中的下一条（对齐桌面「确认后关闭」）。 */
  confirmUrgent(msgId: string): void {
    notificationManager.cancel(this.notifId(msgId), (err: BusinessError) => {
      // 无对应横幅（前台路径未发系统通知）也照常弹下一条
    });
    this.dropQueued(msgId);
    this.pumpUrgent();
  }

  // —— 内部 ——

  private pumpUrgent(): void {
    const prompt = this.urgentPrompt;
    if (prompt === null) {
      return; // 无 UI 接收方：滞留队列（回主界面 setUrgentPrompt 时再弹）
    }
    if (this.urgentQueue.length === 0) {
      return;
    }
    const next = this.urgentQueue[0];
    this.urgentQueue.splice(0, 1);
    prompt(next);
  }

  private dropQueued(msgId: string): void {
    for (let i = this.urgentQueue.length - 1; i >= 0; i--) {
      if (this.urgentQueue[i].msgId === msgId) {
        this.urgentQueue.splice(i, 1);
      }
    }
  }

  /** 基础文本横幅。urgent=true 时 isOngoing（确认前不可滑除）。 */
  private publishBasic(id: number, title: string, text: string, urgent: boolean): void {
    const normal: notificationManager.NotificationContent = {
      notificationContentType: notificationManager.ContentType.NOTIFICATION_CONTENT_BASIC_TEXT,
      normal: {
        title: title,
        text: text,
        additionalText: ''
      }
    };
    if (urgent) {
      const request: notificationManager.NotificationRequest = {
        id: id,
        isOngoing: true,
        content: normal
      };
      notificationManager.publish(request, (err: BusinessError) => {
        // 未授权/失败：静默降级为站内（消息已落库）
      });
      return;
    }
    const request: notificationManager.NotificationRequest = {
      id: id,
      content: normal
    };
    notificationManager.publish(request, (err: BusinessError) => {
      // 同上
    });
  }

  private vibrate(): void {
    try {
      vibrator.startVibration(
        { type: 'preset', count: 2 },
        { id: 0, usage: 'notification' },
        (err: BusinessError) => {
          // 震动失败（无马达/不支持）静默
        },
      );
    } catch (e) {
      // 设备不支持时 API 可能同步抛错——通知链路不因此中断
    }
  }

  /** 通知 id：同一 msg_id 稳定映射（重复投递更新原横幅而非叠条）。 */
  private notifId(msgId: string): number {
    if (msgId.length === 0) {
      const v = this.seqId;
      this.seqId++;
      return v;
    }
    let h = 5381;
    for (let i = 0; i < msgId.length; i++) {
      h = (((h << 5) - h + msgId.charCodeAt(i)) | 0) >>> 0;
    }
    return h % 2147483647;
  }
}

/** 会话标题（单聊=对端账号；群聊=group:N 原样展示，群名随后续块）。 */
export function peerDisplayName(peer: string): string {
  return peer;
}
