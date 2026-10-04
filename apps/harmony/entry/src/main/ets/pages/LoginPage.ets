/**
 * 登录页（R18，对齐 apps/android LoginActivity）：移动端全部为协作态，
 * 必须登录后使用；本页自身也守卫（未初始化一律改道向导）。
 * 登录验证与长连接建立一次完成（ChatSession.connect）：成功即移交
 * AppState，主界面/聊天页直接消费；避免「验证连接＋常连接」双连接互踢。
 */
import router from '@ohos.router';
import { ConnectOutcome } from '@memex/core';
import { appState, CLIENT_VERSION, newTransport } from '../common/AppState';

@Entry
@Component
struct LoginPage {
  @State account: string = '';
  @State password: string = '';
  @State error: string = '';
  @State busy: boolean = false;

  aboutToAppear(): void {
    // 防御守卫：初始化态被清（清数据/重装）时不允许停在登录页
    if (!appState.initStore.isInitialized()) {
      router.replaceUrl({ url: 'pages/InitPage' });
    }
  }

  private async login(): Promise<void> {
    this.error = '';
    const account = this.account.trim();
    if (account.length === 0) {
      this.error = '请填写账号';
      return;
    }
    const addr = appState.initStore.serverAddress();
    if (addr === null) {
      router.replaceUrl({ url: 'pages/InitPage' });
      return;
    }
    this.busy = true;
    const session = appState.establish(account);
    let outcome: ConnectOutcome;
    try {
      outcome = await session.connect(
        newTransport(),
        addr.host,
        addr.port,
        this.password,
        appState.deviceFingerprint(),
        appState.deviceName(),
        CLIENT_VERSION,
        appState.bridge,
      );
    } finally {
      this.busy = false;
    }
    if (outcome.kind === 'ok') {
      // 展示名以服务端 LOGIN_RESULT 回填为准（未回填用账号兜底）
      appState.signIn(account, session.displayNameForUi());
      router.replaceUrl({ url: 'pages/MainPage' });
    } else if (outcome.kind === 'rejected') {
      this.error = outcome.reason.length > 0 ? outcome.reason : '登录被拒绝';
    } else if (outcome.kind === 'unreachable') {
      this.error = '服务器无法连接，请检查地址与网络';
    } else if (outcome.kind === 'timeout') {
      this.error = '登录超时，请稍后重试';
    } else {
      this.error = '对端不是 Memex 服务端';
    }
  }

  build() {
    Column() {
      Text('登录')
        .fontSize(28)
        .fontWeight(FontWeight.Bold)
      Text('Memex 全部为协作态：登录后才能收发。')
        .fontSize(14)
        .opacity(0.7)
        .margin({ top: 8 })

      TextInput({ text: this.account, placeholder: '账号' })
        .type(InputType.Normal)
        .enabled(!this.busy)
        .margin({ top: 24 })
        .onChange((v: string) => {
          this.account = v;
        })

      TextInput({ text: this.password, placeholder: '密码' })
        .type(InputType.Password)
        .enabled(!this.busy)
        .margin({ top: 12 })
        .onChange((v: string) => {
          this.password = v;
        })

      if (this.error !== '') {
        Text(this.error)
          .fontSize(13)
          .fontColor('#D0342C')
          .margin({ top: 8 })
      }

      Button(this.busy ? '登录中…' : '登录')
        .width('100%')
        .margin({ top: 24 })
        .enabled(!this.busy)
        .onClick(() => {
          this.login();
        })

      Text('修改服务器地址')
        .fontSize(14)
        .fontColor('#0A59F7')
        .margin({ top: 20 })
        .onClick(() => {
          router.pushUrl({ url: 'pages/InitPage', params: { reconfigure: true } });
        })
    }
    .padding(24)
    .width('100%')
    .height('100%')
    .alignItems(HorizontalAlign.Start)
  }
}
