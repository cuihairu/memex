import Foundation

/// 文件面调用失败（op=操作名、status=HTTP 状态码；status 0＝本地守卫拒）。
/// 对齐桌面 request_failed(op, status, error) 三元组。
public struct FilesApiError: Error, Equatable {
    public let op: String
    public let status: Int
    public let error: String

    public init(op: String, status: Int, error: String) {
        self.op = op
        self.status = status
        self.error = error
    }
}

/// 收件箱混排条目（/files/list?target=inbox 的 items 元素；me 回 files 纯文件数组）
public enum FilesInboxItem: Equatable {
    case memo(id: Int64, content: String, createdMs: Int64, updatedMs: Int64)
    case file(
        id: Int64, fileName: String, fileSize: Int64, fileHash: String,
        pin: Int, status: Int, uploadTs: Int64
    )
}

public struct FilesMemo: Equatable {
    public let id: Int64
    public let content: String
    public let createdMs: Int64
    public let updatedMs: Int64

    public init(id: Int64, content: String, createdMs: Int64, updatedMs: Int64) {
        self.id = id
        self.content = content
        self.createdMs = createdMs
        self.updatedMs = updatedMs
    }
}

public struct FilesUploadResult: Equatable {
    public let id: Int64
    public let secondTransfer: Bool

    public init(id: Int64, secondTransfer: Bool) {
        self.id = id
        self.secondTransfer = secondTransfer
    }
}

/// 文件面客户端（R23-3 块3 iOS 接线）：FileServer HTTP 面的 Swift 封装，
/// 端点/语义对齐桌面 FilesClient（client/engine/collab/files_client.cpp），
/// 与 Android 面（apps/android core/FilesClient.kt）同口径：
///
///  - POST /files/session 换 token（唯一免鉴权路径），token 仅内存持有；
///  - Bearer 全路由；收件箱混排分页时间倒序；target=me 与收件箱隔离；
///  - 上传 octet-stream + X-File-Name（「手机发自己」inbox＝文件传输）；
///  - 下载流式落盘，重名加序号（base-1.ext）不覆盖；
///  - 失败统一抛 FilesApiError，401/403/503 服务端 error 字段明示。
///
/// 阻塞式（与 MemexClient 同口径）：UI 层放后台线程调用，单测直接在
/// 测试线程跑（URLSession 走注入的 URLProtocol 假面）。设计铁律：字节面
/// 一律过 memex server，不直连对象存储。
public final class FilesClient {
    /// 文件面缺省端口（server/src/main.cpp 与桌面 file_assistant 同款 24561）
    public static let defaultFilesPort = 24_561

    public private(set) var token: String?
    public var isLoggedIn: Bool { token != nil }

    private let session: URLSession
    private var host = ""
    private var port = FilesClient.defaultFilesPort

    /// - Parameter session: 注入 URLSession（单测换 URLProtocol 假面；生产
    ///   URLSessionConfiguration.ephemeral 同款配置）
    public init(session: URLSession) {
        self.session = session
    }

    /// host:port → 请求 URL（IPv6 裸地址补方括号；与 Android 面同款组装）
    private func url(_ path: String) -> URL {
        let hostPart = host.contains(":") ? "[\(host)]" : host
        return URL(string: "http://\(hostPart):\(port)\(path)")!
    }

    // —— 会话 ——

    /// 换 token（POST /files/session，服务端与消息面同一账号库）
    public func login(host: String, port: Int, account: String, password: String) throws {
        token = nil
        self.host = host
        self.port = port
        let body: [String: String] = ["account": account, "password": password]
        let json = try request("session.login", "POST", "/files/session", jsonBody: body, auth: false)
        guard let t = json["token"] as? String, !t.isEmpty else {
            throw FilesApiError(op: "session.login", status: 200, error: "响应缺 token")
        }
        token = t
    }

    /// 清 token（退出文件助手；口令本就不落盘）
    public func logout() {
        token = nil
    }

    // —— 收件箱混排 ——

    public func listInbox(limit: Int = 200, offset: Int = 0) throws -> [FilesInboxItem] {
        try list("inbox.list", target: "inbox", limit: limit, offset: offset, key: "items")
    }

    /// 个人空间列表（target=me）：只列本人文件，与收件箱相互隔离
    /// （服务端非 inbox 回 files 纯文件数组，不混排备忘录）
    public func listPersonal(limit: Int = 200, offset: Int = 0) throws -> [FilesInboxItem] {
        try list("me.list", target: "me", limit: limit, offset: offset, key: "files")
    }

    private func list(
        _ op: String, target: String, limit: Int, offset: Int, key: String
    ) throws -> [FilesInboxItem] {
        let json = try request(
            op, "GET", "/files/list?target=\(target)&limit=\(limit)&offset=\(offset)"
        )
        guard let arr = json[key] as? [[String: Any]] else { return [] }
        return arr.compactMap { obj in
            let id = (obj["id"] as? NSNumber)?.int64Value ?? 0
            if target == "inbox", (obj["type"] as? String) == "memo" {
                return .memo(
                    id: id,
                    content: obj["content"] as? String ?? "",
                    createdMs: (obj["created_ms"] as? NSNumber)?.int64Value ?? 0,
                    updatedMs: (obj["updated_ms"] as? NSNumber)?.int64Value ?? 0
                )
            }
            return .file(
                id: id,
                fileName: obj["file_name"] as? String ?? "",
                fileSize: (obj["file_size"] as? NSNumber)?.int64Value ?? 0,
                fileHash: obj["file_hash"] as? String ?? "",
                pin: (obj["pin"] as? NSNumber)?.intValue ?? 0,
                status: (obj["status"] as? NSNumber)?.intValue ?? 0,
                uploadTs: (obj["upload_ts"] as? NSNumber)?.int64Value ?? 0
            )
        }
    }

    // —— 备忘录 CRUD ——

    @discardableResult
    public func createMemo(_ content: String) throws -> Int64 {
        try memoWrite(nil, content)
    }

    /// 有 id=更新（服务端同一 POST /files/memo 路由）
    @discardableResult
    public func updateMemo(id: Int64, content: String) throws -> Int64 {
        try memoWrite(id, content)
    }

    private func memoWrite(_ id: Int64?, _ content: String) throws -> Int64 {
        var body: [String: Any] = ["content": content]
        if let id = id { body["id"] = id }
        let json = try request("memo.write", "POST", "/files/memo", anyBody: body)
        return (json["id"] as? NSNumber)?.int64Value ?? 0
    }

    public func deleteMemo(id: Int64) throws {
        try requestVoid("memo.delete", "DELETE", "/files/memo?id=\(id)")
    }

    public func listMemos(limit: Int = 100, offset: Int = 0) throws -> [FilesMemo] {
        let json = try request("memo.list", "GET", "/files/memo?limit=\(limit)&offset=\(offset)")
        guard let arr = json["memos"] as? [[String: Any]] else { return [] }
        return arr.map { obj in
            FilesMemo(
                id: (obj["id"] as? NSNumber)?.int64Value ?? 0,
                content: obj["content"] as? String ?? "",
                createdMs: (obj["created_ms"] as? NSNumber)?.int64Value ?? 0,
                updatedMs: (obj["updated_ms"] as? NSNumber)?.int64Value ?? 0
            )
        }
    }

    public func fetchMemo(id: Int64) throws -> FilesMemo {
        let json = try request("memo.fetch", "GET", "/files/memo?id=\(id)")
        guard let obj = json["memo"] as? [String: Any] else {
            throw FilesApiError(op: "memo.fetch", status: 200, error: "响应缺 memo")
        }
        return FilesMemo(
            id: (obj["id"] as? NSNumber)?.int64Value ?? 0,
            content: obj["content"] as? String ?? "",
            createdMs: (obj["created_ms"] as? NSNumber)?.int64Value ?? 0,
            updatedMs: (obj["updated_ms"] as? NSNumber)?.int64Value ?? 0
        )
    }

    // —— 文件上传/下载/删除 ——

    /// 上传原始字节（target：inbox=收件箱（手机发自己）／me=个人空间／group:<id>=群）
    public func upload(target: String, fileName: String, data: Data) throws -> FilesUploadResult {
        let json = try requestRaw(
            "inbox.upload", "POST", "/files/upload?target=\(target)",
            body: data, contentType: "application/octet-stream",
            extraHeaders: ["X-File-Name": fileName]
        )
        return FilesUploadResult(
            id: (json["id"] as? NSNumber)?.int64Value ?? 0,
            secondTransfer: (json["second_transfer"] as? NSNumber)?.boolValue ?? false
        )
    }

    /// 下载到目录（不存在则创建），文件名取响应头 X-File-Name、重名加序号不覆盖
    @discardableResult
    public func download(fileId: Int64, to directory: URL) throws -> URL {
        let op = "file.download"
        guard token != nil else { throw FilesApiError(op: op, status: 0, error: "未登录（先 login）") }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let (data, response) = try rawGet(op, "/files/download?id=\(fileId)")
        guard let http = response as? HTTPURLResponse else {
            throw FilesApiError(op: op, status: 0, error: "非 HTTP 响应")
        }
        guard (200..<300).contains(http.statusCode) else {
            throw FilesApiError(op: op, status: http.statusCode, error: errorText(of: http, data: data))
        }
        // 线口径：X-File-Name 原始 UTF-8 字节；URLSession 已按 ISO-Latin-1 解出
        // 字符串，逐字符还原字节再走 UTF-8（与 Android 面同一逆变换）
        let headerName = (http.allHeaderFields["X-File-Name"] as? String) ?? "download.bin"
        let rawName = String(
            data: Data(headerName.map { $0.latin1Byte }), encoding: .utf8
        ) ?? headerName
        let name = Self.sanitizeName(rawName)
        let target = Self.dedupeURL(for: name, in: directory)
        try data.write(to: target, options: .atomic)
        return target
    }

    public func deleteFile(id: Int64) throws {
        try requestVoid("file.delete", "POST", "/files/manage/delete?id=\(id)", emptyBody: true)
    }

    // —— HTTP 底面 ——

    private func rawGet(_ op: String, _ path: String) throws -> (Data, URLResponse) {
        guard token != nil else { throw FilesApiError(op: op, status: 0, error: "未登录（先 login）") }
        var req = URLRequest(url: url(path))
        req.httpMethod = "GET"
        req.setValue("Bearer \(token!)", forHTTPHeaderField: "Authorization")
        return try syncSend(op, req)
    }

    private func request(
        _ op: String, _ method: String, _ path: String,
        jsonBody: [String: String]? = nil, auth: Bool = true
    ) throws -> [String: Any] {
        try requestAny(op, method, path, body: jsonBody, auth: auth)
    }

    private func request(
        _ op: String, _ method: String, _ path: String,
        anyBody: [String: Any]? = nil, auth: Bool = true
    ) throws -> [String: Any] {
        try requestAny(op, method, path, body: anyBody, auth: auth)
    }

    private func requestAny(
        _ op: String, _ method: String, _ path: String,
        body: Any?, auth: Bool
    ) throws -> [String: Any] {
        var req = URLRequest(url: url(path))
        req.httpMethod = method
        if auth {
            guard token != nil else { throw FilesApiError(op: op, status: 0, error: "未登录（先 login）") }
            req.setValue("Bearer \(token!)", forHTTPHeaderField: "Authorization")
        }
        if let body = body {
            req.httpBody = try JSONSerialization.data(withJSONObject: body)
            req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        }
        let (data, response) = try syncSend(op, req)
        guard let http = response as? HTTPURLResponse else {
            throw FilesApiError(op: op, status: 0, error: "非 HTTP 响应")
        }
        guard (200..<300).contains(http.statusCode) else {
            throw FilesApiError(op: op, status: http.statusCode, error: errorText(of: http, data: data))
        }
        let obj = try JSONSerialization.jsonObject(with: data)
        guard let dict = obj as? [String: Any] else {
            throw FilesApiError(op: op, status: http.statusCode, error: "回包非 JSON 对象")
        }
        if let ok = dict["ok"] as? Bool, !ok {
            throw FilesApiError(op: op, status: http.statusCode, error: dict["error"] as? String ?? "服务端拒绝")
        }
        return dict
    }

    private func requestRaw(
        _ op: String, _ method: String, _ path: String,
        body: Data, contentType: String, extraHeaders: [String: String]
    ) throws -> [String: Any] {
        guard token != nil else { throw FilesApiError(op: op, status: 0, error: "未登录（先 login）") }
        var req = URLRequest(url: url(path))
        req.httpMethod = method
        req.setValue("Bearer \(token!)", forHTTPHeaderField: "Authorization")
        req.httpBody = body
        req.setValue(contentType, forHTTPHeaderField: "Content-Type")
        for (k, v) in extraHeaders { req.setValue(v, forHTTPHeaderField: k) }
        let (data, response) = try syncSend(op, req)
        guard let http = response as? HTTPURLResponse else {
            throw FilesApiError(op: op, status: 0, error: "非 HTTP 响应")
        }
        guard (200..<300).contains(http.statusCode) else {
            throw FilesApiError(op: op, status: http.statusCode, error: errorText(of: http, data: data))
        }
        let obj = try JSONSerialization.jsonObject(with: data)
        return (obj as? [String: Any]) ?? [:]
    }

    private func requestVoid(
        _ op: String, _ method: String, _ path: String, emptyBody: Bool = false
    ) throws {
        _ = try request(
            op, method, path, anyBody: emptyBody ? [String: Any]() : nil
        )
    }

    /// 阻塞收尾（信号量等完成；错误面透传 URL 错误给上层 classify 兜底）
    private func syncSend(_ op: String, _ req: URLRequest) throws -> (Data, URLResponse) {
        var result: Result<(Data, URLResponse), Error>?
        let done = DispatchSemaphore(value: 0)
        session.dataTask(with: req) { data, response, error in
            if let error = error {
                result = .failure(error)
            } else {
                result = .success((data ?? Data(), response ?? URLResponse()))
            }
            done.signal()
        }.resume()
        _ = done.wait(timeout: .now() + 60)
        guard let r = result else {
            throw FilesApiError(op: op, status: 0, error: "请求超时")
        }
        return try r.get()
    }

    private func errorText(of http: HTTPURLResponse, data: Data) -> String {
        if let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           let err = obj["error"] as? String, !err.isEmpty {
            return err
        }
        switch http.statusCode {
        case 401: return "会话无效或过期（先 POST /files/session）"
        case 403: return "无权操作"
        case 503: return "对象存储未就绪（上传暂不可用）"
        default: return "HTTP \(http.statusCode)"
        }
    }

    /// 文件名去控制字符（客户端兜底，服务端头里已滤一层；非 ASCII 原样保留）
    static func sanitizeName(_ name: String) -> String {
        let cleaned = name.filter { ch in
            guard let ascii = ch.asciiValue else { return true }
            return ascii >= 32 && ascii != 127
        }.trimmingCharacters(in: .whitespaces)
        return cleaned.isEmpty ? "download.bin" : cleaned
    }

    /// 重名加序号：name.ext → name-1.ext → name-2.ext（对齐桌面 QSaveFile 口径）
    static func dedupeURL(for name: String, in directory: URL) -> URL {
        let candidate = directory.appendingPathComponent(name)
        if !FileManager.default.fileExists(atPath: candidate.path) { return candidate }
        let dot = name.lastIndex(of: ".") ?? name.endIndex
        let base = String(name[..<dot])
        let ext = dot == name.endIndex ? "" : String(name[dot...])
        var n = 1
        while FileManager.default.fileExists(
            atPath: directory.appendingPathComponent("\(base)-\(n)\(ext)").path
        ) { n += 1 }
        return directory.appendingPathComponent("\(base)-\(n)\(ext)")
    }
}

private extension Character {
    /// ISO-Latin-1 视角的字节（URLSession 头解码还原用）
    var latin1Byte: UInt8 {
        unicodeScalars.first.map { UInt8(clamping: $0.value) } ?? 0
    }
}
