/**
 * 聊天窗（对齐 apps/android ChatActivity）：单聊收发为主，群地址
 * （group:N）可收可发。消息来自 AppState 本地库（收到即落库），发送走
 * 长连接；列表按时间正序、进入即清未读；本页不拥有连接（只订阅）。
 * 移动端适配：气泡左右分侧、进入自动滚底、长按复制消息原文。
 */
import router from '@ohos.router';
import promptAction from '@ohos.promptAction';
import pasteboard from '@ohos.pasteboard';
import { StoredMessage } from '@memex/core';
import { AppListener, appState } from '../common/AppState';

interface MsgRow {
  who: string;
  text: string;
  mine: boolean;
  time: string;
  recalled: boolean;
}

function fmtClock(tsMs: number): string {
  const d = new Date(tsMs);
  const hh = ('0' + d.getHours()).slice(-2);
  const mi = ('0' + d.getMinutes()).slice(-2);
  return `${hh}:${mi}`;
}

@Entry
@Component
struct ChatPage {
  @State rows: MsgRow[] = [];
  @State input: string = '';
  @State peer: string = '';
  private scroller: Scroller = new Scroller();
  private listener: AppListener | null = null;

  aboutToAppear(): void {
    const p = router.getParams() as Record<string, string>;
    this.peer = p !== undefined && p !== null && p['peer'] !== undefined ? p['peer'] : '';
    if (this.peer.length === 0) {
      router.back();
      return;
    }
    appState.openPeer = this.peer; // 前台推送裁决：正看该会话不弹横幅
    appState.store.markRead(this.peer); // 进会话即清未读（列表角标同步）
    const l: AppListener = {
      onListChanged: (): void => {
        this.render();
      },
      onOffline: (cause: string): void => {
        promptAction.showToast({ message: '连接断开：' + cause, duration: 2000 });
      },
      onKicked: (reason: string): void => {
        promptAction.showToast({ message: reason, duration: 3000 });
        router.replaceUrl({ url: 'pages/LoginPage' });
      },
    };
    this.listener = l;
    appState.addListener(l);
    this.render();
  }

  aboutToDisappear(): void {
    appState.openPeer = '';
    if (this.listener !== null) {
      appState.removeListener(this.listener);
      this.listener = null;
    }
  }

  private render(): void {
    const hist: StoredMessage[] = appState.store.history(this.peer);
    const arr: MsgRow[] = [];
    for (let i = 0; i < hist.length; i++) {
      const m = hist[i];
      const mine = m.from === appState.account;
      const row: MsgRow = {
        who: mine ? '我' : m.from,
        text: m.text,
        mine: mine,
        time: fmtClock(m.tsMs),
        recalled: m.recalled,
      };
      arr.push(row);
    }
    this.rows = arr;
    this.scroller.scrollEdge(Edge.Bottom); // 滚到最新
  }

  private send(): void {
    const text = this.input.trim();
    if (text.length === 0) {
      return;
    }
    const seq = appState.sendText(this.peer, text);
    if (seq === 0) {
      promptAction.showToast({ message: '未连接，发送失败', duration: 1500 });
      return;
    }
    this.input = '';
    this.render(); // 自己发的消息本地立即落库
  }

  /** 移动端适配：长按复制消息原文。 */
  private copyText(text: string): void {
    const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
    pasteboard.getSystemPasteboard().setData(data, (_err: Error) => {
      promptAction.showToast({ message: '已复制', duration: 1200 });
    });
  }

  build() {
    Column() {
      Text(this.peer)
        .fontSize(20)
        .fontWeight(FontWeight.Medium)
        .width('100%')
        .padding({ top: 8, bottom: 8 })

      List({ scroller: this.scroller, space: 8 }) {
        ForEach(this.rows, (m: MsgRow) => {
          ListItem() {
            Row() {
              Column() {
                Text(`${m.who} ${m.time}`)
                  .fontSize(12)
                  .opacity(0.55)
                Text(m.recalled ? '（已撤回）' : m.text)
                  .fontSize(16)
                  .opacity(m.recalled ? 0.4 : 1)
                  .backgroundColor(m.mine ? '#D7E8FF' : '#F1F1F1')
                  .borderRadius(8)
                  .padding(10)
                  .margin({ top: 3 })
                  .constraintSize({ maxWidth: '78%' })
              }
              .alignItems(m.mine ? HorizontalAlign.End : HorizontalAlign.Start)
            }
            .width('100%')
            .justifyContent(m.mine ? FlexAlign.End : FlexAlign.Start)
            .gesture(
              LongPressGesture()
                .onAction(() => {
                  this.copyText(m.text);
                })
            )
          }
        }, (m: MsgRow) => `${m.who}:${m.time}:${m.text}`)
      }
      .layoutWeight(1)
      .width('100%')

      Row() {
        TextInput({ text: this.input, placeholder: '输入消息' })
          .layoutWeight(1)
          .onChange((v: string) => {
            this.input = v;
          })
          .onSubmit(() => {
            this.send();
          })
        Button('发送')
          .margin({ left: 8 })
          .onClick(() => {
            this.send();
          })
      }
      .width('100%')
      .margin({ top: 8 })
    }
    .padding(16)
    .width('100%')
    .height('100%')
  }
}
