/**
 * 协作态客户端（镜像 apps/android MemexClient.kt）：
 * - probe：连通性校验（PING→PONG，R17 向导前置门）；
 * - login：登录验证（LOGIN→LOGIN_RESULT，device_kind=mobile）。
 * 纯协议语义与平台无关，Transport 由调用方注入（Node 测试 / 鸿蒙壳）。
 */
import { MsgType, msgTypeName, pingEnvelope, loginEnvelope } from './wire';
import { Transport } from './transport';
import { WireChannel, classifyFailure } from './wire_channel';
import { ServerAddress } from './address';

/** 连通性校验结果（R17：校验通过才允许保存并放行）。 */
export type ProbeResult =
  | { kind: 'ok'; rttMs: number }
  | { kind: 'unreachable'; detail: string }
  | { kind: 'timeout'; detail: string }
  | { kind: 'notMemex'; detail: string };

/** 登录结果。 */
export type LoginOutcome =
  | { kind: 'success'; displayName: string }
  | { kind: 'rejected'; reason: string }
  | { kind: 'unreachable'; detail: string }
  | { kind: 'timeout'; detail: string }
  | { kind: 'notMemex'; detail: string };

export class MemexClient {
  private readonly transportFactory: () => Transport;
  private readonly connectTimeoutMs: number;
  private readonly readTimeoutMs: number;

  constructor(
    transportFactory: () => Transport,
    connectTimeoutMs = 5000,
    readTimeoutMs = 5000,
  ) {
    this.transportFactory = transportFactory;
    this.connectTimeoutMs = connectTimeoutMs;
    this.readTimeoutMs = readTimeoutMs;
  }

  /** 连通性校验：PING→PONG 成功算通过；其余归类为不可达/超时/非 Memex。 */
  async probe(address: ServerAddress): Promise<ProbeResult> {
    const ch = new WireChannel(this.transportFactory());
    const start = Date.now();
    try {
      await ch.connect(address.host, address.port, this.connectTimeoutMs);
      ch.send(pingEnvelope(Date.now()));
      const reply = await ch.nextEnvelope(this.readTimeoutMs);
      if (reply === null) return { kind: 'timeout', detail: '等待 PONG 超时' };
      if (reply.type === MsgType.PONG) return { kind: 'ok', rttMs: Date.now() - start };
      return { kind: 'notMemex', detail: '期望 PONG，收到 ' + msgTypeName(reply.type) };
    } catch (e) {
      const f = classifyFailure(e as Error);
      return { kind: f.kind, detail: f.detail };
    } finally {
      ch.close();
    }
  }

  /**
   * 登录验证：LOGIN→ 等 LOGIN_RESULT（登录广播可能先到，跳过直到目标帧）。
   */
  async login(
    address: ServerAddress,
    account: string,
    password: string,
    deviceFingerprint: string,
    deviceName: string,
    clientVersion: string,
  ): Promise<LoginOutcome> {
    const ch = new WireChannel(this.transportFactory());
    try {
      await ch.connect(address.host, address.port, this.connectTimeoutMs);
      ch.send(loginEnvelope(account, password, deviceFingerprint, deviceName, clientVersion, Date.now()));
      const deadline = Date.now() + this.readTimeoutMs;
      for (;;) {
        const reply = await ch.nextEnvelope(this.readTimeoutMs);
        if (reply === null) return { kind: 'timeout', detail: '等待 LOGIN_RESULT 超时' };
        if (reply.type !== MsgType.LOGIN_RESULT) {
          if (Date.now() > deadline) return { kind: 'timeout', detail: '等待 LOGIN_RESULT 超时' };
          continue;
        }
        const r = reply.loginResult;
        if (r === undefined) return { kind: 'notMemex', detail: 'LOGIN_RESULT 缺载荷' };
        if (r.ok) return { kind: 'success', displayName: r.displayName.length > 0 ? r.displayName : account };
        return { kind: 'rejected', reason: r.reason.length > 0 ? r.reason : '登录被拒绝' };
      }
    } catch (e) {
      const f = classifyFailure(e as Error);
      return { kind: f.kind, detail: f.detail };
    } finally {
      ch.close();
    }
  }
}