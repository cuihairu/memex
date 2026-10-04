/**
 * 初始化向导（R17，对齐 apps/android InitActivity）：首次启动强制进入的
 * 服务器地址设置页。无跳过路径——「校验并保存」必须拿到 PONG 才落盘并
 * 放行去登录页；已初始化的常规进入一律改道登录页（InitGate），仅登录页
 * 「修改服务器地址」显式重设（reconfigure）时出示表单。
 */
import router from '@ohos.router';
import promptAction from '@ohos.promptAction';
import { InitEntry, InitGate, MemexClient, ProbeResult, ServerAddress } from '@memex/core';
import { appState, newTransport } from '../common/AppState';

/** 地址解析失败原因 → 文案（对齐 Android err_addr_* 字符串）。 */
function addrErrorText(reason: string): string {
  if (reason === 'EMPTY') {
    return '请填写服务器地址';
  }
  if (reason === 'BAD_HOST') {
    return '主机名不合法';
  }
  if (reason === 'BAD_PORT') {
    return '端口不合法（1-65535）';
  }
  if (reason === 'SCHEME') {
    return '仅支持 http:// 前缀（会被剥除）';
  }
  return 'IPv6 请用 [IPv6]:port 形态';
}

@Entry
@Component
struct InitPage {
  @State address: string = '';
  @State error: string = '';
  @State busy: boolean = false;

  aboutToAppear(): void {
    const p = router.getParams() as Record<string, boolean>;
    const reconfigure: boolean = p !== undefined && p !== null && p['reconfigure'] === true;
    const entry = InitGate.entry(appState.initStore, reconfigure);
    if (entry === InitEntry.TO_LOGIN) {
      // 向导不是回头路：已初始化的常规进入改道登录页
      router.replaceUrl({ url: 'pages/LoginPage' });
      return;
    }
    const addr = appState.initStore.serverAddress();
    if (addr !== null) {
      this.address = addr.display(); // 重设场景预填当前地址
    }
  }

  private async probeAndSave(): Promise<void> {
    this.error = '';
    const parsed = ServerAddress.parse(this.address);
    if (!parsed.ok) {
      this.error = addrErrorText(parsed.reason);
      return;
    }
    this.busy = true;
    const client = new MemexClient(() => newTransport(), 5000, 5000);
    let r: ProbeResult;
    try {
      r = await client.probe(parsed.address);
    } finally {
      this.busy = false;
    }
    if (r.kind === 'ok') {
      // 唯一落盘路径：连通性校验通过（R17 口径）
      appState.initStore.markInitialized(parsed.address);
      promptAction.showToast({ message: '服务器校验通过', duration: 1500 });
      router.replaceUrl({ url: 'pages/LoginPage' });
    } else if (r.kind === 'unreachable') {
      this.error = '服务器无法连接，请检查地址与网络';
    } else if (r.kind === 'timeout') {
      this.error = '连接超时，请稍后重试';
    } else {
      this.error = '对端不是 Memex 服务端';
    }
  }

  build() {
    Column() {
      Text('初始化')
        .fontSize(28)
        .fontWeight(FontWeight.Bold)
      Text('首次使用请填写 Memex 服务器地址；连通性校验通过后才能进入。')
        .fontSize(14)
        .opacity(0.7)
        .margin({ top: 8 })

      TextInput({ text: this.address, placeholder: 'host:port 或 [IPv6]:port' })
        .type(InputType.Normal)
        .enabled(!this.busy)
        .margin({ top: 24 })
        .onChange((v: string) => {
          this.address = v;
        })

      if (this.error !== '') {
        Text(this.error)
          .fontSize(13)
          .fontColor('#D0342C')
          .margin({ top: 8 })
      }

      if (this.busy) {
        LoadingProgress()
          .width(36)
          .height(36)
          .margin({ top: 16 })
      }

      Button(this.busy ? '校验中…' : '校验并保存')
        .width('100%')
        .margin({ top: 24 })
        .enabled(!this.busy && this.address.length > 0)
        .onClick(() => {
          this.probeAndSave();
        })
    }
    .padding(24)
    .width('100%')
    .height('100%')
    .alignItems(HorizontalAlign.Start)
  }
}
