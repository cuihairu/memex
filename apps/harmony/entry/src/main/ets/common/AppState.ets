/**
 * 应用级状态枢纽（对齐 apps/android 的 MemexApp + Session + ChatHolder +
 * ChatManager 合集，鸿蒙单事件循环模型下合并为一个桥）：
 * - 初始化态：@ohos.data.preferences 落盘 + 内存快照（core InitStore 的
 *   同步实现；RouteGuard/InitGate 直接消费）；
 * - 登录态：进程内存（重启后须重新登录，初始化态持久化不受影响，R18）；
 * - 会话持有：登录页建立连接后移交到此，主界面/聊天页消费（ChatHolder）；
 * - 事件桥：ChatSessionListener 多路分发——常驻通知裁决（NoticeManager，
 *   跨重新登录保持）＋ UI 订阅者（AppListener，页面挂摘）。
 *
 * 线程模型：鸿蒙 socket 回调在事件循环线程直投（与 Node 同构），UI 刷新
 * 由页面订阅回调内完成（@State 赋值触发渲染）。
 */
import preferences from '@ohos.data.preferences';
import deviceInfo from '@ohos.deviceInfo';
import common from '@ohos.app.ability.common';
import {
  ChatSession,
  ChatSessionListener,
  ChatStore,
  InMemoryChatStore,
  InitStore,
  NoticeGrade,
  ServerAddress,
  DEFAULT_PORT,
} from '@memex/core';
import { HarmonyTransport } from './HarmonyTransport';
import { DeviceIdentity } from './DeviceIdentity';
import { NoticeManager } from './NoticeManager';

/** 与 app.json5 versionName 对齐（LOGIN client_version 字段）。 */
export const CLIENT_VERSION = '0.1.0';

/** UI 订阅回调（页面挂摘；ArkTS 无多继承，桥接为单一窄接口）。 */
export interface AppListener {
  /** 列表/聊天数据有变（新消息、通知落库、发送受理）。 */
  onListChanged(): void;
  /** 连接断开（未主动登出）。 */
  onOffline(cause: string): void;
  /** 被服务端互踢（登录态已被本桥清空）。 */
  onKicked(reason: string): void;
}

/**
 * 初始化态存取（core InitStore 的鸿蒙实现）：
 * preferences 异步载入后快照同步读（EntryAbility 启动即预载，页面守卫
 * await appState.ready() 后使用）；markInitialized 同步更新快照＋异步落盘。
 */
class PrefsInitStore implements InitStore {
  private done = false;
  private host = '';
  private port = DEFAULT_PORT;
  private prefs: preferences.Preferences | null = null;

  async load(context: common.UIAbilityContext): Promise<void> {
    const p = await preferences.getPreferencesAsync(context, 'memex_init');
    this.prefs = p;
    this.done = (await p.get('initialized', false)) === true;
    this.host = (await p.get('server_host', '')) as string;
    this.port = (await p.get('server_port', DEFAULT_PORT)) as number;
  }

  isInitialized(): boolean {
    return this.done;
  }

  serverAddress(): ServerAddress | null {
    if (!this.done) {
      return null;
    }
    return new ServerAddress(this.host, this.port);
  }

  markInitialized(address: ServerAddress): void {
    this.done = true;
    this.host = address.host;
    this.port = address.port;
    const p = this.prefs;
    if (p === null) {
      return; // ready 前不可达（守卫先行 ready()）
    }
    p.put('initialized', true);
    p.put('server_host', address.host);
    p.put('server_port', address.port);
    p.flush();
  }
}

class AppState {
  /** 初始化态（R17；preferences 落盘）。 */
  readonly initStore: PrefsInitStore = new PrefsInitStore();
  /** 本地会话存储（内存实现；持久化随后续块，对齐 Android 本块口径）。 */
  readonly store: ChatStore = new InMemoryChatStore();
  /** 常驻通知裁决（三级推送；跨重新登录保持，对齐 ChatHolder.attachNotifier）。 */
  readonly notice: NoticeManager = new NoticeManager(this.store);

  /** 登录态（进程内存，R18：重启后须重新登录）。 */
  account = '';
  displayName = '';
  isLoggedIn = false;

  /** 前后台与当前打开会话（推送裁决依据；UIAbility 生命周期/页面维护）。 */
  foreground = false;
  openPeer = '';

  private context: common.UIAbilityContext | null = null;
  private readyPromise: Promise<void> | null = null;
  private session: ChatSession | null = null;
  private listeners: AppListener[] = [];

  /** UIAbility onCreate 时绑定上下文（preferences 载入用）。 */
  bindContext(ctx: common.UIAbilityContext): void {
    this.context = ctx;
  }

  /** 初始化态预载（幂等；页面守卫 await）。 */
  ready(): Promise<void> {
    if (this.readyPromise === null) {
      const ctx = this.context;
      if (ctx === null) {
        return Promise.reject(new Error('AppState 未绑定 UIAbility 上下文'));
      }
      this.readyPromise = this.initStore.load(ctx);
    }
    return this.readyPromise;
  }

  signIn(account: string, displayName: string): void {
    this.account = account;
    this.displayName = displayName;
    this.isLoggedIn = true;
  }

  signOut(): void {
    this.account = '';
    this.displayName = '';
    this.isLoggedIn = false;
  }

  addListener(l: AppListener): void {
    for (let i = 0; i < this.listeners.length; i++) {
      if (this.listeners[i] === l) {
        return;
      }
    }
    this.listeners.push(l);
  }

  removeListener(l: AppListener): void {
    const idx = this.listeners.indexOf(l);
    if (idx >= 0) {
      this.listeners.splice(idx, 1);
    }
  }

  /**
   * 建立会话（登录页调用；重复登录先清理旧连接——单点互踢语义下服务端
   * 也会踢旧连接，本地照旧清干净）。连接由页面 await connect 完成。
   * account 在构造期传入（LOGIN 帧 from/发送 from 共用）；展示名登录成功
   * 由服务端 LOGIN_RESULT 回填。
   */
  establish(account: string): ChatSession {
    const prev = this.session;
    if (prev !== null) {
      prev.logout();
      this.session = null;
    }
    return new ChatSession(this.store, account, account, 8000, 0);
  }

  /** 发送文本。返回 seq（0=未连接/发送失败）。 */
  sendText(to: string, text: string): number {
    const s = this.session;
    if (s === null) {
      return 0;
    }
    return s.sendText(to, text);
  }

  /** 通知授权引导＋slot 声明（主界面首次进入时调用）。 */
  ensureNotifications(ctx: common.UIAbilityContext): void {
    this.notice.ensureChannels();
    this.notice.requestEnable(ctx);
  }

  /** 登出并断开（LOGOUT 帧后关连接，清 UI 听众）。 */
  logoutAndClear(): void {
    const s = this.session;
    this.session = null;
    if (s !== null) {
      s.logout();
    }
    this.signOut();
    this.listeners.splice(0, this.listeners.length);
  }

  /**
   * 常驻事件桥（connect 的 listener 参数）：通知裁决常驻，UI 订阅转发。
   * 对齐 Android ChatHolder.attachNotifier 的「跨重新登录保持」语义。
   */
  readonly bridge: ChatSessionListener = {
    onMessage: (peer: string, msgId: string, mine: boolean): void => {
      this.notice.onNewMessage(peer, this.foreground, this.openPeer);
      for (let i = 0; i < this.listeners.length; i++) {
        this.listeners[i].onListChanged();
      }
    },
    onNotice: (
      peer: string,
      grade: NoticeGrade,
      title: string,
      content: string,
      jumpUrl: string,
      msgId: string,
    ): void => {
      this.notice.onNotice(peer, grade, title, content, jumpUrl, msgId, this.foreground);
      for (let i = 0; i < this.listeners.length; i++) {
        this.listeners[i].onListChanged();
      }
    },
    onSent: (seq: number): void => {},
    onDisconnected: (cause: string): void => {
      for (let i = 0; i < this.listeners.length; i++) {
        this.listeners[i].onOffline(cause);
      }
    },
    onKicked: (reason: string): void => {
      // 单点互踢：连接已被 core 关闭，清登录态后由页面跳登录页
      this.session = null;
      this.signOut();
      for (let i = 0; i < this.listeners.length; i++) {
        this.listeners[i].onKicked(reason);
      }
    },
  };

  /** 登录附加参数：设备指纹（稳定种子，见 DeviceIdentity）。 */
  deviceFingerprint(): string {
    return DeviceIdentity.fingerprint(
      'harmony:' + deviceInfo.productModel + ':' + deviceInfo.marketName,
    );
  }

  /** 登录附加参数：设备名（互踢 replacedBy 展示用）。 */
  deviceName(): string {
    const n = deviceInfo.marketName;
    return n !== undefined && n.length > 0 ? n : deviceInfo.productModel;
  }

  /** 当前连接（ChatPage 判定离线提示用）。 */
  hasSession(): boolean {
    return this.session !== null;
  }
}

/** 应用级单例（对齐 Android Session.instance / ChatHolder 双单例合并）。 */
export const appState = new AppState();

/** 组装登录用 transport 工厂（页面直接 new 亦可；集中便于替换 mock）。 */
export function newTransport(): HarmonyTransport {
  return new HarmonyTransport();
}
