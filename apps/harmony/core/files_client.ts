/**
 * 文件面客户端（R23-3 块3 鸿蒙接线，镜像 apps/android core/FilesClient.kt）：
 * - POST /files/session 换 token（唯一免鉴权路径），token 仅内存持有；
 * - Bearer 全路由；收件箱混排分页时间倒序；target=me 与收件箱隔离；
 * - 上传 octet-stream + X-File-Name（「手机发自己」inbox＝文件传输）；
 * - 下载回字节与还原名（X-File-Name 线口径＝原始 UTF-8 字节，宿主按
 *   Latin-1 视角解出的字符串需经 restoreUtf8Name 还原）——落盘归 ArkTS 壳
 *   （core 纯逻辑不碰 fs，对齐 Transport 注入的分界）；
 * - 失败统一抛 FilesApiError（op/status/error 三元组；status 0＝本地守卫拒）。
 *
 * HTTP 由调用方注入（HttpFetch）：Node 测试用 test/node_http_fetch.ts
 * （node:http），鸿蒙壳用 @ohos.net.http 等价实现。X-File-Name 双向都是
 * 原始 UTF-8 字节：宿主 HTTP 栈只收 Latin-1 头值时，发送前 utf8ToLatin1、
 * 接收后 restoreUtf8Name（两 helper 供壳复用）。
 * 设计铁律：字节面一律过 memex server，不直连对象存储。
 */

/** 文件面调用失败（status=HTTP 状态码；0＝本地守卫拒，未发请求）。 */
export class FilesApiError extends Error {
  readonly op: string;
  readonly status: number;

  constructor(op: string, status: number, error: string) {
    super(error);
    this.name = 'FilesApiError';
    this.op = op;
    this.status = status;
  }
}

/** 一次 HTTP 请求（url=完整 URL，含查询串；timeoutMs 宿主栈自选实现）。 */
export interface HttpRequest {
  method: string;
  url: string;
  headers: Record<string, string>;
  body?: Uint8Array;
  timeoutMs?: number;
}

/** 一次 HTTP 应答（headers 键为宿主栈原样；通常小写）。 */
export interface HttpResponse {
  status: number;
  headers: Record<string, string>;
  body: Uint8Array;
}

/** HTTP 传输注入点：Node 测试 / 鸿蒙壳各给一份实现（对齐 Transport 分界）。 */
export type HttpFetch = (req: HttpRequest) => Promise<HttpResponse>;

/** 收件箱混排条目（/files/list?target=inbox 的 items；me 回 files 纯文件数组）。 */
export type FilesInboxItem =
  | { kind: 'memo'; id: number; content: string; createdMs: number; updatedMs: number }
  | {
      kind: 'file';
      id: number;
      fileName: string;
      fileSize: number;
      fileHash: string;
      pin: number;
      status: number;
      uploadTs: number;
    };

export interface FilesMemo {
  id: number;
  content: string;
  createdMs: number;
  updatedMs: number;
}

export interface FilesUploadResult {
  id: number;
  secondTransfer: boolean;
}

/** 下载结果：还原后的文件名 + 字节（落盘/分享由 ArkTS 壳决定）。 */
export interface DownloadedFile {
  fileName: string;
  data: Uint8Array;
}

/** 文件面缺省端口（与桌面 file_assistant、Android 面同款）。 */
export const DEFAULT_FILES_PORT = 24561;

/** UTF-8 字节 → Latin-1 视角字符串（X-File-Name 上线前的宿主栈适配）。 */
export function utf8ToLatin1(s: string): string {
  const bytes = new TextEncoder().encode(s);
  let out = '';
  for (let i = 0; i < bytes.length; i++) out += String.fromCharCode(bytes[i]);
  return out;
}

/** Latin-1 视角字符串 → UTF-8 还原（X-File-Name 收头侧逆变换）。 */
export function restoreUtf8Name(headerValue: string): string {
  const bytes = new Uint8Array(headerValue.length);
  for (let i = 0; i < headerValue.length; i++) bytes[i] = headerValue.charCodeAt(i) & 0xff;
  return new TextDecoder('utf-8').decode(bytes);
}

/** 文件名去控制字符（客户端兜底，服务端头里已滤一层；非 ASCII 原样保留）。 */
export function sanitizeFileName(name: string): string {
  let out = '';
  for (const ch of name) {
    const code = ch.codePointAt(0) ?? 0;
    if (code >= 0x20 && code !== 0x7f) out += ch;
  }
  const trimmed = out.trim();
  return trimmed.length > 0 ? trimmed : 'download.bin';
}

/** 重名加序号：name.ext → name-1.ext（existing=同目录已有名，对齐桌面口径）。 */
export function dedupeFileName(existing: string[], name: string): string {
  if (!existing.includes(name)) return name;
  const dot = name.lastIndexOf('.');
  const base = dot > 0 ? name.slice(0, dot) : name;
  const ext = dot > 0 ? name.slice(dot) : '';
  for (let n = 1; ; n++) {
    const candidate = `${base}-${n}${ext}`;
    if (!existing.includes(candidate)) return candidate;
  }
}

export class FilesClient {
  private readonly http: HttpFetch;
  private token: string | null = null;
  private base = '';

  constructor(http: HttpFetch) {
    this.http = http;
  }

  get isLoggedIn(): boolean {
    return this.token !== null;
  }

  /** host:port → baseUrl（IPv6 裸地址补方括号；与 Android 面同款组装）。 */
  static baseUrlOf(host: string, port: number): string {
    const hostPart = host.includes(':') ? `[${host}]` : host;
    return `http://${hostPart}:${port}`;
  }

  /** 换 token（POST /files/session，服务端与消息面同一账号库）。 */
  async login(host: string, port: number, account: string, password: string): Promise<void> {
    this.token = null;
    this.base = FilesClient.baseUrlOf(host, port);
    const json = (await this.request(
      'session.login',
      'POST',
      '/files/session',
      { 'Content-Type': 'application/json' },
      new TextEncoder().encode(JSON.stringify({ account, password })),
      false,
    )) as Record<string, unknown>;
    const t = json.token;
    if (typeof t !== 'string' || t.length === 0) {
      throw new FilesApiError('session.login', 200, '响应缺 token');
    }
    this.token = t;
  }

  /** 清 token（退出文件助手；口令本就不落盘）。 */
  logout(): void {
    this.token = null;
  }

  /** 收件箱混排（memo+file 按 updated_ms/upload_ts 倒序，分页透传）。 */
  async listInbox(limit = 200, offset = 0): Promise<FilesInboxItem[]> {
    return this.list('inbox.list', 'inbox', limit, offset);
  }

  /** 个人空间列表（target=me）：只列本人文件，与收件箱相互隔离。 */
  async listPersonal(limit = 200, offset = 0): Promise<FilesInboxItem[]> {
    return this.list('me.list', 'me', limit, offset);
  }

  private async list(
    op: string,
    target: string,
    limit: number,
    offset: number,
  ): Promise<FilesInboxItem[]> {
    const key = target === 'inbox' ? 'items' : 'files';
    const json = (await this.request(
      op,
      'GET',
      `/files/list?target=${target}&limit=${limit}&offset=${offset}`,
    )) as Record<string, unknown>;
    const arr = json[key];
    if (!Array.isArray(arr)) return [];
    return arr.map((raw) => {
      const obj = raw as Record<string, unknown>;
      const id = num(obj.id);
      if (target === 'inbox' && obj.type === 'memo') {
        return {
          kind: 'memo' as const,
          id,
          content: str(obj.content),
          createdMs: num(obj.created_ms),
          updatedMs: num(obj.updated_ms),
        };
      }
      return {
        kind: 'file' as const,
        id,
        fileName: str(obj.file_name),
        fileSize: num(obj.file_size),
        fileHash: str(obj.file_hash),
        pin: num(obj.pin),
        status: num(obj.status),
        uploadTs: num(obj.upload_ts),
      };
    });
  }

  // —— 备忘录 CRUD ——

  /** 建备忘录（POST /files/memo，回 id）。 */
  async createMemo(content: string): Promise<number> {
    return this.memoWrite(null, content);
  }

  /** 改备忘录（同路由，带 id）。 */
  async updateMemo(id: number, content: string): Promise<number> {
    return this.memoWrite(id, content);
  }

  private async memoWrite(id: number | null, content: string): Promise<number> {
    const body: Record<string, unknown> = { content };
    if (id !== null) body.id = id;
    const json = (await this.request(
      'memo.write',
      'POST',
      '/files/memo',
      { 'Content-Type': 'application/json' },
      new TextEncoder().encode(JSON.stringify(body)),
    )) as Record<string, unknown>;
    return num(json.id);
  }

  /** 删备忘录（DELETE /files/memo?id=N）。 */
  async deleteMemo(id: number): Promise<void> {
    await this.request('memo.delete', 'DELETE', `/files/memo?id=${id}`);
  }

  /** 备忘录列表（GET /files/memo?limit&offset，按更新时间倒序）。 */
  async listMemos(limit = 100, offset = 0): Promise<FilesMemo[]> {
    const json = (await this.request(
      'memo.list',
      'GET',
      `/files/memo?limit=${limit}&offset=${offset}`,
    )) as Record<string, unknown>;
    const arr = json.memos;
    if (!Array.isArray(arr)) return [];
    return arr.map((raw) => {
      const obj = raw as Record<string, unknown>;
      return {
        id: num(obj.id),
        content: str(obj.content),
        createdMs: num(obj.created_ms),
        updatedMs: num(obj.updated_ms),
      };
    });
  }

  /** 单条备忘录（GET /files/memo?id=N）。 */
  async fetchMemo(id: number): Promise<FilesMemo> {
    const json = (await this.request(
      'memo.fetch',
      'GET',
      `/files/memo?id=${id}`,
    )) as Record<string, unknown>;
    const obj = json.memo as Record<string, unknown> | undefined;
    if (obj === undefined) throw new FilesApiError('memo.fetch', 200, '响应缺 memo');
    return {
      id: num(obj.id),
      content: str(obj.content),
      createdMs: num(obj.created_ms),
      updatedMs: num(obj.updated_ms),
    };
  }

  // —— 文件上传/下载/删除 ——

  /** 上传原始字节（target：inbox=收件箱（手机发自己）／me=个人空间／group:<id>=群）。 */
  async upload(target: string, fileName: string, data: Uint8Array): Promise<FilesUploadResult> {
    const json = (await this.request(
      'inbox.upload',
      'POST',
      `/files/upload?target=${target}`,
      {
        'Content-Type': 'application/octet-stream',
        // 线口径：原始 UTF-8 字节；宿主栈只收 Latin-1 头值时调用方先用
        // utf8ToLatin1 适配（本 helper 已按字节语义设计，原样传入亦可）
        'X-File-Name': utf8ToLatin1(fileName),
      },
      data,
    )) as Record<string, unknown>;
    return { id: num(json.id), secondTransfer: json.second_transfer === true };
  }

  /**
   * 下载（GET /files/download?id=N）：回还原名 + 字节。重名加序号的落盘
   * 策略（base-1.ext）由壳用 dedupeFileName 处理（core 不碰 fs）。
   */
  async download(fileId: number): Promise<DownloadedFile> {
    const res = await this.send('file.download', {
      method: 'GET',
      url: this.url(`/files/download?id=${fileId}`),
      headers: { Authorization: `Bearer ${this.token ?? ''}` },
    });
    if (res.status < 200 || res.status >= 300) {
      throw new FilesApiError('file.download', res.status, errorText(res));
    }
    const headerName = headerValue(res.headers, 'x-file-name') ?? 'download.bin';
    return {
      fileName: sanitizeFileName(restoreUtf8Name(headerName)),
      data: res.body,
    };
  }

  /** 删文件（POST /files/manage/delete?id=N）。 */
  async deleteFile(id: number): Promise<void> {
    await this.request('file.delete', 'POST', `/files/manage/delete?id=${id}`, {
      'Content-Type': 'application/json',
    }, new Uint8Array(0));
  }

  // —— HTTP 底面 ——

  private url(path: string): string {
    return this.base + path;
  }

  private async request(
    op: string,
    method: string,
    path: string,
    headers: Record<string, string> = {},
    body?: Uint8Array,
    auth = true,
  ): Promise<unknown> {
    const all: Record<string, string> = { ...headers };
    if (auth) {
      if (this.token === null) throw new FilesApiError(op, 0, '未登录（先 login）');
      all.Authorization = `Bearer ${this.token}`;
    }
    const res = await this.send(op, {
      method,
      url: this.url(path),
      headers: all,
      body,
    });
    if (res.status < 200 || res.status >= 300) {
      throw new FilesApiError(op, res.status, errorText(res));
    }
    let obj: unknown;
    try {
      obj = JSON.parse(new TextDecoder().decode(res.body));
    } catch {
      throw new FilesApiError(op, res.status, '回包非 JSON');
    }
    const record = obj as Record<string, unknown>;
    if (record.ok === false) {
      throw new FilesApiError(op, res.status, typeof record.error === 'string' ? record.error : '服务端拒绝');
    }
    return obj;
  }

  private async send(op: string, req: HttpRequest): Promise<HttpResponse> {
    try {
      return await this.http(req);
    } catch (e) {
      if (e instanceof FilesApiError) throw e;
      throw new FilesApiError(op, 0, `网络不可达：${(e as Error).message}`);
    }
  }
}

function num(v: unknown): number {
  return typeof v === 'number' ? v : 0;
}

function str(v: unknown): string {
  return typeof v === 'string' ? v : '';
}

/** headers 取值（键大小写不敏感）。 */
function headerValue(headers: Record<string, string>, name: string): string | null {
  const lower = name.toLowerCase();
  for (const key of Object.keys(headers)) {
    if (key.toLowerCase() === lower) return headers[key];
  }
  return null;
}

/** 错误体抽 error 字段（无 JSON 则按状态码给惯用文案，401/403/503 明示）。 */
function errorText(res: HttpResponse): string {
  try {
    const obj = JSON.parse(new TextDecoder().decode(res.body)) as Record<string, unknown>;
    if (typeof obj.error === 'string' && obj.error.length > 0) return obj.error;
  } catch {
    // 落到状态码文案
  }
  switch (res.status) {
    case 401:
      return '会话无效或过期（先 POST /files/session）';
    case 403:
      return '无权操作';
    case 503:
      return '对象存储未就绪（上传暂不可用）';
    default:
      return `HTTP ${res.status}`;
  }
}
