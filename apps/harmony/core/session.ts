/**
 * 协作态长连接会话（镜像 apps/android ChatSession.kt，语义对齐桌面
 * collab_engine.cpp）：
 * - 登录后保持单条 TCP 连接；帧流由 WireChannel 分帧分发；
 * - send_text：本端 seq 自增（seqLedger 提供时发号即写、重登续位——BUG-007
 *   §4.1 对齐桌面平台-9），帧 = Envelope{TEXT, seq, from, to, ts_ms, text}；
 * - 收到 TEXT：按 peer 落库（群消息 peer=to 的 "group:N"，单聊 peer=from），
 *   若带 msg_id 回 ACK(msg_id)（服务端按 msg_id 清离线队列，重复投递由
 *   本地 msg_id 去重）；自己发的消息本地立即落库（msg_id 为空，等受理回执）；
 * - ACK(seq)：发送方受理回执 → onSent；ACK(msg_id) 为接收方已收取，仅清理
 *   本地 pending（移动端不维护 inflight 重传，重传随断线重连块）；
 * - NOTICE：归档 compose_notice_text「标题：正文[ 跳转]」、peer 规则同 TEXT、
 *   seq=0 则本地单调 seq、回 ACK(msg_id)、去重后回调 onNotice（分级弹窗
 *   由上层裁决）；
 * - KICK：服务端单点互踢 → onKicked 后断开；
 * - 断开：onDisconnected 回调，由上层决定重连（首块不做自动重连）。
 *
 * 线程模型：Transport 事件线程内分发（Node=事件循环；ArkTS=套接字回调
 * 线程，壳层负责投递 UI）。回调全部走构造传入的 listener（直投）。
 */
import {
  Envelope,
  MsgType,
  ackEnvelope,
  loginEnvelope,
  logoutEnvelope,
  textEnvelope,
} from './wire';
import { Transport } from './transport';
import { WireChannel, classifyFailure } from './wire_channel';
import { ChatStore } from './chat_store';
import { SeqLedger } from './seq_ledger';
import { NoticeGrade, composeNoticeText, fromProtoNumber } from './format';

/** UI 回调（读线程/事件线程进入，壳层负责投递 UI 线程）。 */
export interface ChatSessionListener {
  /** 收到一条新消息（已落库；msgId 空=自己刚发的受理暂存）。 */
  onMessage?(peer: string, msgId: string, mine: boolean): void;
  /** 收到站内通知（NOTICE 三级推送；peer 已落库，分级弹窗由上层裁决）。 */
  onNotice?(
    peer: string,
    grade: NoticeGrade,
    title: string,
    content: string,
    jumpUrl: string,
    msgId: string,
  ): void;
  /** 发送受理回执（服务端 ACK(seq)）。 */
  onSent?(seq: number): void;
  /** 连接断开（未主动 close）——上层决定重连。 */
  onDisconnected?(cause: string): void;
  /** 被服务端互踢（KICK）。 */
  onKicked?(reason: string): void;
}

export type ConnectOutcome =
  | { kind: 'ok'; rttMs: number }
  | { kind: 'rejected'; reason: string }
  | { kind: 'unreachable'; detail: string }
  | { kind: 'timeout'; detail: string }
  | { kind: 'notMemex'; detail: string };

export class ChatSession {
  private channel: WireChannel | null = null;
  private listener: ChatSessionListener | null = null;
  private closed = true;
  /** 发号器：构造期从台账续位（BUG-007 §4.1，对齐桌面平台-9；无台账从 1 起） */
  private seqGen: number;
  private readonly seqLedger: SeqLedger | null;

  private readonly store: ChatStore;
  private readonly account: string;
  /** 展示名：登录成功由服务端 LOGIN_RESULT 回填（壳层显示用）。 */
  private displayName: string;
  private readonly connectTimeoutMs: number;
  private readonly readTimeoutMs: number; // 0=无限：长连接不因空闲断

  constructor(
    store: ChatStore,
    account: string,
    displayName: string,
    connectTimeoutMs = 8000,
    readTimeoutMs = 0,
    seqLedger: SeqLedger | null = null,
  ) {
    this.store = store;
    this.account = account;
    this.displayName = displayName;
    this.connectTimeoutMs = connectTimeoutMs;
    this.readTimeoutMs = readTimeoutMs;
    this.seqLedger = seqLedger;
    // 台账存最后已发号，续位从上界＋1 起；无记录从 1 起
    const last = seqLedger !== null ? seqLedger.load(account) : 0;
    this.seqGen = last > 0 ? last + 1 : 1;
  }

  /** 发号＋发号即写（BUG-007 §4.1；事件循环单线程，天然保序）。 */
  private issueSeq(): number {
    const seq = this.seqGen++;
    if (this.seqLedger !== null) this.seqLedger.save(this.account, seq);
    return seq;
  }

  /**
   * 登录并挂起读循环（async：UI 侧放异步任务调用；Node 单测直接 await）。
   * 成功返回 ok；失败返回对应结果且连接已关闭。
   */
  async connect(
    transport: Transport,
    host: string,
    port: number,
    password: string,
    deviceFingerprint: string,
    deviceName: string,
    clientVersion: string,
    listener: ChatSessionListener,
  ): Promise<ConnectOutcome> {
    this.listener = listener;
    const ch = new WireChannel(transport);
    const start = Date.now();
    try {
      await ch.connect(host, port, this.connectTimeoutMs);
      ch.send(
        loginEnvelope(this.account, password, deviceFingerprint, deviceName, clientVersion, Date.now()),
      );
      // 登录广播可能先于 LOGIN_RESULT 到达（PRESENCE_DATA 等），跳过直到目标帧
      const deadline = Date.now() + this.readTimeoutMs;
      for (;;) {
        const reply = await ch.nextEnvelope(this.readTimeoutMs > 0 ? this.readTimeoutMs : 0);
        if (reply === null) {
          this.safeClose(ch);
          return { kind: 'timeout', detail: '等待 LOGIN_RESULT 超时' };
        }
        if (reply.type !== MsgType.LOGIN_RESULT) {
          if (this.readTimeoutMs > 0 && Date.now() > deadline) {
            this.safeClose(ch);
            return { kind: 'timeout', detail: '等待 LOGIN_RESULT 超时' };
          }
          continue;
        }
        const r = reply.loginResult;
        if (r === undefined) {
          this.safeClose(ch);
          return { kind: 'notMemex', detail: 'LOGIN_RESULT 缺载荷' };
        }
        if (!r.ok) {
          this.safeClose(ch);
          return { kind: 'rejected', reason: r.reason.length > 0 ? r.reason : '登录被拒绝' };
        }
        this.displayName = r.displayName.length > 0 ? r.displayName : this.account;
        break;
      }
      this.closed = false;
      this.channel = ch;
      ch.onData = (env) => this.handleEnvelope(env);
      ch.onClose = (cause) => this.notifyDisconnect(cause);
      return { kind: 'ok', rttMs: Date.now() - start };
    } catch (e) {
      this.safeClose(ch);
      const f = classifyFailure(e as Error);
      return { kind: f.kind, detail: f.detail };
    }
  }

  /** 当前展示名（登录成功后为服务端回填值；未登录为构造传入值）。 */
  displayNameForUi(): string {
    return this.displayName;
  }

  /** 发送文本。返回分配的 seq（0=未连接/空目标）。 */
  sendText(to: string, text: string): number {
    const ch = this.channel;
    if (ch === null || this.closed || to.length === 0) return 0;
    const seq = this.issueSeq();
    const ts = Date.now();
    try {
      ch.send(textEnvelope(seq, this.account, to, text, ts));
    } catch (e) {
      this.notifyDisconnect((e as Error).message || '发送失败');
      return 0;
    }
    // 本地立即落库（自己发的消息；msg_id 空，服务端受理后才有——对齐桌面）
    this.store.append({
      id: 0,
      peer: to,
      from: this.account,
      to: to,
      seq,
      tsMs: ts,
      text,
      source: 'collab',
      msgId: '',
      recalled: false,
    });
    this.call((l) => l.onMessage?.(to, '', true));
    return seq;
  }

  /** 主动登出并断开（LOGOUT 后由服务端关连接，无需等待）。 */
  logout(): void {
    const ch = this.channel;
    if (ch !== null && !this.closed) {
      try {
        ch.send(logoutEnvelope(this.issueSeq(), this.account, Date.now()));
      } catch (e) {
        // 忽略：即将断开
      }
    }
    this.safeClose(ch);
  }

  close(): void {
    this.safeClose(this.channel);
  }

  private handleEnvelope(env: Envelope): void {
    switch (env.type) {
      case MsgType.TEXT:
        this.onIncomingText(env);
        break;
      case MsgType.NOTICE:
        this.onIncomingNotice(env);
        break;
      case MsgType.ACK:
        this.onAck(env);
        break;
      case MsgType.KICK: {
        const reason = env.kick?.reason ?? '';
        this.call((l) => l.onKicked?.(reason.length > 0 ? reason : '账号已在其他设备登录'));
        this.safeClose(this.channel);
        break;
      }
      case MsgType.PONG:
        break; // 心跳应答
      default:
        break; // 组织/群/已读等后续块处理
    }
  }

  private onIncomingText(env: Envelope): void {
    const t = env.text;
    if (t === undefined) return;
    // 群消息 peer=to 的 "group:N"；单聊 peer=from（对齐桌面 collab_engine）
    const peer = env.to.startsWith('group:') ? env.to : env.from;
    const inserted = this.store.append({
      id: 0,
      peer,
      from: env.from,
      to: env.to,
      seq: env.seq,
      tsMs: env.tsMs,
      text: t.text,
      source: 'collab',
      msgId: env.msgId,
      recalled: false,
    });
    // 已收取回执（msg_id 非空即回；离线补投去重后照回 ACK——服务端按
    // 账号清离线队列，重复 ACK 无害）
    if (env.msgId.length > 0) this.sendAck(env.msgId);
    if (inserted) this.call((l) => l.onMessage?.(peer, env.msgId, false));
  }

  /**
   * 收到站内通知（NOTICE；webhook 接入推送，三级推送数据面）。
   * 语义对齐桌面 collab_engine.handle_notice。
   */
  private onIncomingNotice(env: Envelope): void {
    const n = env.notice;
    if (n === undefined) return;
    const ts = env.tsMs > 0 ? env.tsMs : Date.now();
    const peer = env.to.startsWith('group:') ? env.to : env.from;
    const inserted = this.store.append({
      id: 0,
      peer,
      from: env.from,
      to: env.to,
      seq: env.seq > 0 ? env.seq : this.store.nextLocalSeq(env.from),
      tsMs: ts,
      text: composeNoticeText(n.title, n.content, n.jumpUrl),
      source: 'collab',
      msgId: env.msgId,
      recalled: false,
    });
    if (env.msgId.length > 0) this.sendAck(env.msgId);
    if (inserted) {
      this.call((l) =>
        l.onNotice?.(peer, fromProtoNumber(n.urgency), n.title, n.content, n.jumpUrl, env.msgId),
      );
    }
  }

  private sendAck(msgId: string): void {
    const ch = this.channel;
    if (ch === null) return;
    try {
      ch.send(ackEnvelope(this.issueSeq(), this.account, 'server', msgId, Date.now()));
    } catch (e) {
      // 忽略：断线路径由读侧处理
    }
  }

  private onAck(env: Envelope): void {
    const seq = env.seq;
    if (seq === 0) return;
    this.call((l) => l.onSent?.(seq));
  }

  private notifyDisconnect(cause: string): void {
    this.safeClose(this.channel);
    this.call((l) => l.onDisconnected?.(cause));
  }

  private call(fn: (l: ChatSessionListener) => void): void {
    const l = this.listener;
    if (l !== null) {
      try {
        fn(l);
      } catch (e) {
        // 回调异常不吞读循环（日志由上层决定）
      }
    }
  }

  private safeClose(ch: WireChannel | null): void {
    if (ch === null) return;
    this.closed = true;
    if (this.channel === ch) this.channel = null;
    ch.close();
  }
}