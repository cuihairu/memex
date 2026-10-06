import XCTest
@testable import MemexKit

/// URLProtocol 假面：记请求（URL/方法/头/体）、回罐头响应。
/// （macOS runner CI 腿跑全量；本地无 swift 工具链不跑——不伪造本地绿）
final class StubFilesProtocol: URLProtocol {
    /// (status, headers, body)
    static var handler: ((URLRequest) -> (Int, [String: String], Data))?
    static var recorded: [URLRequest] = []

    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }

    /// 会话发出的 body 在 httpBodyStream（httpBody 为 nil），流式读回
    static func body(of request: URLRequest) -> Data {
        if let body = request.httpBody { return body }
        guard let stream = request.httpBodyStream else { return Data() }
        stream.open()
        defer { stream.close() }
        var data = Data()
        let size = 4096
        let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: size)
        defer { buf.deallocate() }
        while stream.hasBytesAvailable {
            let n = stream.read(buf, maxLength: size)
            if n <= 0 { break }
            data.append(buf, count: n)
        }
        return data
    }

    override func startLoading() {
        Self.recorded.append(request)
        let (status, headers, data) = Self.handler?(request) ?? (200, [:], Data())
        let response = HTTPURLResponse(
            url: request.url!, statusCode: status, httpVersion: "HTTP/1.1", headerFields: headers
        )!
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: data)
        client?.urlProtocolDidFinishLoading(self)
    }

    override func stopLoading() {}
}

/// FilesClient 全端点单测（对齐 server FileServer 路由口径与 Android 面 16 用例）
final class FilesClientTests: XCTestCase {

    private var client: FilesClient!

    override func setUp() {
        super.setUp()
        StubFilesProtocol.handler = nil
        StubFilesProtocol.recorded = []
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [StubFilesProtocol.self]
        client = FilesClient(session: URLSession(configuration: config))
    }

    override func tearDown() {
        StubFilesProtocol.handler = nil
        StubFilesProtocol.recorded = []
        super.tearDown()
    }

    /// 登录（127.0.0.1 口径；换 token 后才 isLoggedIn）
    private func loginFirst() throws {
        StubFilesProtocol.handler = { _ in
            (200, ["Content-Type": "application/json"],
             #"{"ok":true,"token":"tok-1"}"#.data(using: .utf8)!)
        }
        try client.login(host: "127.0.0.1", port: FilesClient.defaultFilesPort,
                         account: "alice", password: "pw")
        XCTAssertTrue(client.isLoggedIn)
    }

    private func lastRequest() -> URLRequest { StubFilesProtocol.recorded.last! }
    private func lastBody() -> Data { StubFilesProtocol.body(of: lastRequest()) }

    // —— 会话 ——

    func testLoginPostsSessionAndStoresToken() throws {
        try loginFirst()
        let req = StubFilesProtocol.recorded[0]
        XCTAssertEqual(req.url?.path, "/files/session")
        XCTAssertEqual(req.httpMethod, "POST")
        XCTAssertNil(req.value(forHTTPHeaderField: "Authorization")) // 唯一免鉴权路径
        let body = try XCTUnwrap(JSONSerialization.jsonObject(with: StubFilesProtocol.body(of: req)) as? [String: Any])
        XCTAssertEqual(body["account"] as? String, "alice")
        XCTAssertEqual(body["password"] as? String, "pw")
        // 后续请求带 Bearer
        StubFilesProtocol.handler = { _ in
            (200, ["Content-Type": "application/json"], #"{"ok":true,"items":[]}"#.data(using: .utf8)!)
        }
        _ = try client.listInbox()
        XCTAssertEqual(lastRequest().value(forHTTPHeaderField: "Authorization"), "Bearer tok-1")
    }

    func testLoginFailure401CarriesStatus() {
        StubFilesProtocol.handler = { _ in
            (401, ["Content-Type": "application/json"],
             #"{"error":"账号或口令不正确"}"#.data(using: .utf8)!)
        }
        XCTAssertThrowsError(try client.login(host: "127.0.0.1", port: 24_561,
                                              account: "alice", password: "bad")) { e in
            let err = e as! FilesApiError
            XCTAssertEqual(err.op, "session.login")
            XCTAssertEqual(err.status, 401)
            XCTAssertEqual(err.error, "账号或口令不正确")
        }
        XCTAssertFalse(client.isLoggedIn)
    }

    func testUnauthenticatedCallRejectedLocally() {
        XCTAssertThrowsError(try client.listInbox()) { e in
            XCTAssertEqual((e as! FilesApiError).status, 0) // 本地守卫拒
        }
        XCTAssertEqual(StubFilesProtocol.recorded.count, 0) // 没发请求
    }

    func testLogoutClearsToken() throws {
        try loginFirst()
        client.logout()
        XCTAssertFalse(client.isLoggedIn)
        XCTAssertThrowsError(try client.listInbox())
    }

    // —— 收件箱混排 ——

    func testListInboxMixedItemsAndPaging() throws {
        try loginFirst()
        StubFilesProtocol.handler = { req in
            XCTAssertEqual(URLComponents(url: req.url!, resolvingAgainstBaseURL: false)?
                .queryItems?.first(where: { $0.name == "target" })?.value, "inbox")
            return (200, ["Content-Type": "application/json"],
                    """
                {"ok":true,"items":[
                 {"type":"memo","id":7,"content":"备忘","created_ms":111,"updated_ms":222},
                 {"type":"file","id":9,"file_name":"a.txt","file_size":3,"file_hash":"h1",
                  "pin":1,"status":2,"upload_ts":333}
                ]}
                """.data(using: .utf8)!)
        }
        let items = try client.listInbox(limit: 50, offset: 10)
        XCTAssertEqual(items.count, 2)
        XCTAssertEqual(items[0], .memo(id: 7, content: "备忘", createdMs: 111, updatedMs: 222))
        XCTAssertEqual(items[1], .file(id: 9, fileName: "a.txt", fileSize: 3, fileHash: "h1",
                                       pin: 1, status: 2, uploadTs: 333))
        let query = URLComponents(url: lastRequest().url!, resolvingAgainstBaseURL: false)?.queryItems
        XCTAssertEqual(query?.first(where: { $0.name == "limit" })?.value, "50")
        XCTAssertEqual(query?.first(where: { $0.name == "offset" })?.value, "10")
    }

    func testListPersonalReadsFilesArray() throws {
        try loginFirst()
        StubFilesProtocol.handler = { _ in
            (200, ["Content-Type": "application/json"],
             """
            {"ok":true,"files":[
             {"id":5,"file_name":"b.bin","file_size":9,"file_hash":"h2",
              "pin":0,"status":0,"upload_ts":444}
            ]}
            """.data(using: .utf8)!)
        }
        let items = try client.listPersonal()
        XCTAssertEqual(items.count, 1)
        XCTAssertEqual(items[0], .file(id: 5, fileName: "b.bin", fileSize: 9, fileHash: "h2",
                                       pin: 0, status: 0, uploadTs: 444))
        // target=me 透传（个人空间与收件箱隔离）
        let query = URLComponents(url: lastRequest().url!, resolvingAgainstBaseURL: false)?.queryItems
        XCTAssertEqual(query?.first(where: { $0.name == "target" })?.value, "me")
    }

    // —— 备忘录 CRUD ——

    func testMemoCreateAndUpdateShareRoute() throws {
        try loginFirst()
        StubFilesProtocol.handler = { _ in
            (200, ["Content-Type": "application/json"], #"{"ok":true,"id":11}"#.data(using: .utf8)!)
        }
        XCTAssertEqual(try client.createMemo("内容一"), 11)
        var req = lastRequest()
        XCTAssertEqual(req.url?.path, "/files/memo")
        XCTAssertEqual(req.httpMethod, "POST")
        var body = try XCTUnwrap(JSONSerialization.jsonObject(with: lastBody()) as? [String: Any])
        XCTAssertEqual(body["content"] as? String, "内容一")
        XCTAssertNil(body["id"]) // 建不带 id

        XCTAssertEqual(try client.updateMemo(id: 11, content: "内容二"), 11)
        req = lastRequest()
        XCTAssertEqual(req.url?.path, "/files/memo") // 同路由
        body = try XCTUnwrap(JSONSerialization.jsonObject(with: lastBody()) as? [String: Any])
        XCTAssertEqual(body["id"] as? Int64, 11) // 改带 id
        XCTAssertEqual(body["content"] as? String, "内容二")
    }

    func testMemoDeleteListFetch() throws {
        try loginFirst()
        StubFilesProtocol.handler = { req in
            switch (req.httpMethod ?? "", req.url!.path) {
            case ("DELETE", "/files/memo"):
                return (200, [:], #"{"ok":true}"#.data(using: .utf8)!)
            case ("GET", "/files/memo") where req.url!.query?.hasPrefix("limit=") == true:
                return (200, [:], #"{"ok":true,"memos":[{"id":3,"content":"c","created_ms":1,"updated_ms":2}]}"#
                    .data(using: .utf8)!)
            default:
                return (200, [:], #"{"ok":true,"memo":{"id":3,"content":"c","created_ms":1,"updated_ms":2}}"#
                    .data(using: .utf8)!)
            }
        }
        try client.deleteMemo(id: 3)
        XCTAssertEqual(lastRequest().httpMethod, "DELETE")
        XCTAssertEqual(lastRequest().url?.query, "id=3")

        let memos = try client.listMemos(limit: 20, offset: 0)
        XCTAssertEqual(memos, [FilesMemo(id: 3, content: "c", createdMs: 1, updatedMs: 2)])

        let one = try client.fetchMemo(id: 3)
        XCTAssertEqual(one.content, "c")
        XCTAssertEqual(one.updatedMs, 2)
    }

    // —— 文件上传/下载/删除 ——

    func testUploadHeadersBytesAndResult() throws {
        try loginFirst()
        StubFilesProtocol.handler = { req in
            XCTAssertEqual(req.value(forHTTPHeaderField: "Content-Type"), "application/octet-stream")
            XCTAssertEqual(req.value(forHTTPHeaderField: "X-File-Name"), "报告 终版.txt") // 原始 UTF-8 上线
            return (200, [:], #"{"id":21,"second_transfer":true}"#.data(using: .utf8)!)
        }
        let payload = Data([0x00, 0xFF, 0x10, 0x7F])
        let r = try client.upload(target: "inbox", fileName: "报告 终版.txt", data: payload)
        XCTAssertEqual(r.id, 21)
        XCTAssertTrue(r.secondTransfer) // 秒传
        XCTAssertEqual(lastRequest().url?.path, "/files/upload")
        XCTAssertEqual(lastRequest().url?.query, "target=inbox")
        XCTAssertEqual(lastBody(), payload) // 字节原样
    }

    func testDownloadRestoresUtf8NameAndWrites() throws {
        try loginFirst()
        let dir = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("memex-dl-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: dir) }
        // 线口径：服务端发原始 UTF-8 字节「报告.txt」，URLSession 按 Latin-1 解出
        let utf8 = Data("报告.txt".utf8)
        let latin1 = String(data: utf8, encoding: .isoLatin1)!
        StubFilesProtocol.handler = { _ in
            (200, ["X-File-Name": latin1, "Content-Type": "application/octet-stream"],
             Data("hello".utf8))
        }
        let saved = try client.download(fileId: 9, to: dir)
        XCTAssertEqual(saved.lastPathComponent, "报告.txt") // Latin-1 → UTF-8 还原
        XCTAssertEqual(try String(contentsOf: saved, encoding: .utf8), "hello")

        let again = try client.download(fileId: 9, to: dir)
        XCTAssertEqual(again.lastPathComponent, "报告-1.txt") // 重名加序号不覆盖
    }

    func testDownload404ThrowsWithStatus() throws {
        try loginFirst()
        StubFilesProtocol.handler = { _ in
            (404, ["Content-Type": "application/json"], #"{"error":"文件不存在"}"#.data(using: .utf8)!)
        }
        let dir = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("memex-dl-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: dir) }
        XCTAssertThrowsError(try client.download(fileId: 404, to: dir)) { e in
            let err = e as! FilesApiError
            XCTAssertEqual(err.status, 404)
            XCTAssertEqual(err.error, "文件不存在")
        }
    }

    func testDeleteFileRoute() throws {
        try loginFirst()
        StubFilesProtocol.handler = { _ in
            (200, [:], #"{"ok":true}"#.data(using: .utf8)!)
        }
        try client.deleteFile(id: 33)
        XCTAssertEqual(lastRequest().url?.path, "/files/manage/delete")
        XCTAssertEqual(lastRequest().url?.query, "id=33")
        XCTAssertEqual(lastRequest().httpMethod, "POST")
    }

    // —— 失败明示 ——

    func testForbiddenCarriesServerText() throws {
        try loginFirst()
        StubFilesProtocol.handler = { _ in
            (403, ["Content-Type": "application/json"], #"{"error":"无权删除他人文件"}"#.data(using: .utf8)!)
        }
        XCTAssertThrowsError(try client.deleteFile(id: 1)) { e in
            let err = e as! FilesApiError
            XCTAssertEqual(err.status, 403)
            XCTAssertEqual(err.error, "无权删除他人文件")
        }
    }

    func testUnavailable503Surfaced() throws {
        try loginFirst()
        StubFilesProtocol.handler = { _ in
            (503, ["Content-Type": "application/json"], #"{"error":"对象存储未就绪"}"#.data(using: .utf8)!)
        }
        XCTAssertThrowsError(try client.upload(target: "inbox", fileName: "x", data: Data([1]))) { e in
            XCTAssertEqual((e as! FilesApiError).status, 503)
        }
    }
}
