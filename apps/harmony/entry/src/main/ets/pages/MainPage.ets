/**
 * 主界面＝会话列表（对齐 apps/android MainActivity）：onShow 先过守卫
 * （未初始化/未登录改道）；会话数据来自 AppState 本地库聚合，长连接由
 * 登录页移交，本页只订阅展示。新建聊天：输入对端账号（单聊）或群地址
 * （group:N 可收可发）。紧急通知的确认弹窗在本页挂载（NoticeManager
 * pump 的 UI 接收方）。
 */
import router from '@ohos.router';
import promptAction from '@ohos.promptAction';
import common from '@ohos.app.ability.common';
import { Conversation } from '@memex/core';
import { AppListener, appState } from '../common/AppState';
import { UrgentNotice } from '../common/NoticeManager';

interface ConvRow {
  peer: string;
  title: string;
  preview: string;
  time: string;
  unread: number;
}

function fmtTime(tsMs: number): string {
  const d = new Date(tsMs);
  const mo = ('0' + (d.getMonth() + 1)).slice(-2);
  const da = ('0' + d.getDate()).slice(-2);
  const hh = ('0' + d.getHours()).slice(-2);
  const mi = ('0' + d.getMinutes()).slice(-2);
  return `${mo}-${da} ${hh}:${mi}`;
}

@Entry
@Component
struct MainPage {
  @State rows: ConvRow[] = [];
  @State peerInput: string = '';
  @State who: string = '';
  private listener: AppListener | null = null;

  aboutToAppear(): void {
    // 守卫：登录态被清（被踢后重启到本页等边界）不允许停留
    if (!appState.isLoggedIn) {
      router.replaceUrl({ url: 'pages/LoginPage' });
      return;
    }
    this.who = appState.displayName.length > 0 ? appState.displayName : appState.account;
    appState.ensureNotifications(getContext(this) as common.UIAbilityContext);
    const l: AppListener = {
      onListChanged: (): void => {
        this.render();
      },
      onOffline: (cause: string): void => {
        promptAction.showToast({ message: '连接断开：' + cause, duration: 2000 });
      },
      onKicked: (reason: string): void => {
        this.onKicked(reason);
      },
    };
    this.listener = l;
    appState.addListener(l);
    // 紧急通知确认弹窗挂载（跨页面共享 NoticeManager 队列）
    appState.notice.setUrgentPrompt((n: UrgentNotice) => {
      this.showUrgentConfirm(n);
    });
    this.render();
  }

  aboutToDisappear(): void {
    if (this.listener !== null) {
      appState.removeListener(this.listener);
      this.listener = null;
    }
    appState.notice.setUrgentPrompt(null);
  }

  private onKicked(reason: string): void {
    promptAction.showToast({ message: reason, duration: 3000 });
    router.replaceUrl({ url: 'pages/LoginPage' });
  }

  private render(): void {
    const convs: Conversation[] = appState.store.conversations();
    const arr: ConvRow[] = [];
    for (let i = 0; i < convs.length; i++) {
      const c = convs[i];
      const row: ConvRow = {
        peer: c.peer,
        title: c.peer,
        preview: c.lastText,
        time: fmtTime(c.lastTsMs),
        unread: c.unread,
      };
      arr.push(row);
    }
    this.rows = arr;
  }

  private openChat(peer: string): void {
    router.pushUrl({ url: 'pages/ChatPage', params: { peer: peer } });
  }

  private newChat(): void {
    const peer = this.peerInput.trim();
    if (peer.length === 0) {
      promptAction.showToast({ message: '请填写对端账号', duration: 1500 });
      return;
    }
    this.peerInput = '';
    this.openChat(peer);
  }

  private logout(): void {
    appState.logoutAndClear();
    router.replaceUrl({ url: 'pages/LoginPage' });
  }

  /** 紧急通知确认（对齐桌面「点确认收悉后关闭；未确认的继续排队弹出」）。 */
  private showUrgentConfirm(n: UrgentNotice): void {
    AlertDialog.confirm({
      title: n.title,
      message: n.content,
      autoCancel: false,
      primaryButton: {
        value: '确认收悉',
        action: () => {
          appState.notice.confirmUrgent(n.msgId);
        }
      },
      secondaryButton: {
        value: '稍后',
        action: () => {
          // 不确认：滞留队列，回主界面/下一条确认后再弹
        }
      }
    });
  }

  build() {
    Column() {
      Row() {
        Text(this.who)
          .fontSize(22)
          .fontWeight(FontWeight.Bold)
          .layoutWeight(1)
        Button('登出')
          .buttonStyle(ButtonStyleMode.NORMAL)
          .onClick(() => {
            this.logout();
          })
      }
      .width('100%')

      List({ space: 8 }) {
        ForEach(this.rows, (r: ConvRow) => {
          ListItem() {
            Column() {
              Row() {
                Text(r.title)
                  .fontSize(17)
                  .fontWeight(FontWeight.Medium)
                  .layoutWeight(1)
                if (r.unread > 0) {
                  Text(`${r.unread}`)
                    .fontSize(12)
                    .fontColor('#FFFFFF')
                    .backgroundColor('#D0342C')
                    .borderRadius(10)
                    .padding({ left: 7, right: 7, top: 2, bottom: 2 })
                }
              }
              .width('100%')
              Text(`${r.preview}`)
                .fontSize(14)
                .opacity(0.7)
                .maxLines(1)
                .margin({ top: 4 })
              Text(r.time)
                .fontSize(12)
                .opacity(0.45)
                .margin({ top: 2 })
            }
            .alignItems(HorizontalAlign.Start)
            .width('100%')
            .padding(12)
            .borderRadius(10)
            .backgroundColor('#F5F5F5')
            .onClick(() => {
              this.openChat(r.peer);
            })
          }
        }, (r: ConvRow) => r.peer)
      }
      .layoutWeight(1)
      .width('100%')
      .margin({ top: 12 })
      .visibility(this.rows.length === 0 ? Visibility.None : Visibility.Visible)

      if (this.rows.length === 0) {
        Text('还没有会话。输入对端账号开始聊天。')
          .fontSize(14)
          .opacity(0.5)
          .layoutWeight(1)
      }

      Row() {
        TextInput({ text: this.peerInput, placeholder: '对端账号（如 alice 或 group:9）' })
          .layoutWeight(1)
          .onChange((v: string) => {
            this.peerInput = v;
          })
        Button('聊天')
          .margin({ left: 8 })
          .onClick(() => {
            this.newChat();
          })
      }
      .width('100%')
      .margin({ top: 12 })
    }
    .padding(16)
    .width('100%')
    .height('100%')
  }
}
