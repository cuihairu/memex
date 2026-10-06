/**
 * Node 平台 HttpFetch 实现（node:http）：files_client 单测/CI 专用。
 * 鸿蒙壳用 @ohos.net.http 等价实现。头值先 utf8ToLatin1 适配（node:http
 * 只收 Latin-1 头值；注意 node 客户端把该串再按 UTF-8 写线——不是字节
 * 级原始上线，见 files_client.test.ts 上传用例注记；Android HttpURLConnection
 * 与 ArkTS 壳的头字节行为以各自实测为准）。
 */
import * as http from 'http';
import { HttpRequest, HttpResponse, utf8ToLatin1 } from '../core/files_client';

export class NodeHttpFetch {
  /** 最近一次请求的超时（便于测试断言；0=未设）。 */
  lastTimeoutMs = 0;

  fetch(req: HttpRequest): Promise<HttpResponse> {
    this.lastTimeoutMs = req.timeoutMs ?? 0;
    return new Promise((resolve, reject) => {
      const u = new URL(req.url);
      const request = http.request(
        {
          host: u.hostname,
          port: u.port,
          path: u.pathname + u.search,
          method: req.method,
        },
        (res) => {
          const chunks: Buffer[] = [];
          res.on('data', (c: Buffer) => chunks.push(c));
          res.on('end', () => {
            const body = Buffer.concat(chunks);
            const headers: Record<string, string> = {};
            for (const [k, v] of Object.entries(res.headers)) {
              if (typeof v === 'string') headers[k] = v;
            }
            resolve({
              status: res.statusCode ?? 0,
              headers,
              body: new Uint8Array(body.buffer, body.byteOffset, body.byteLength),
            });
          });
        },
      );
      request.setTimeout(req.timeoutMs ?? 30000, () => {
        request.destroy(new Error('请求超时'));
      });
      request.on('error', (err: Error) => reject(err));
      for (const [k, v] of Object.entries(req.headers)) {
        request.setHeader(k, utf8ToLatin1(v)); // ASCII 值原样，中文值转字节
      }
      if (req.body !== undefined && req.body.length > 0) {
        request.end(Buffer.from(req.body.buffer, req.body.byteOffset, req.body.byteLength));
      } else {
        request.end();
      }
    });
  }
}
