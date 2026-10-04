import XCTest
@testable import MemexKit

/// 服务器地址解析（对齐 Android ServerAddressTest 11 用例）
final class ServerAddressTests: XCTestCase {

    func testEmptyAndWhitespace() {
        XCTAssertEqual(ServerAddress.parse(""), .err(.empty))
        XCTAssertEqual(ServerAddress.parse("   "), .err(.empty))
    }

    func testPlainHostDefaultsToPort() {
        XCTAssertEqual(ServerAddress.parse("memex.example"), .ok(ServerAddress(host: "memex.example", port: 24360)))
    }

    func testHostWithPort() {
        XCTAssertEqual(ServerAddress.parse("memex.example:9000"), .ok(ServerAddress(host: "memex.example", port: 9000)))
    }

    func testIPv6BracketForm() {
        XCTAssertEqual(
            ServerAddress.parse("[2001:db8::1]:24360"),
            .ok(ServerAddress(host: "2001:db8::1", port: 24360))
        )
    }

    func testBareIPv6WithoutBracketsRejected() {
        XCTAssertEqual(ServerAddress.parse("2001:db8::1:24360"), .err(.ipv6Form))
    }

    func testHttpSchemeStripped() {
        XCTAssertEqual(
            ServerAddress.parse("http://memex.example:24360/"),
            .ok(ServerAddress(host: "memex.example", port: 24360))
        )
    }

    func testOtherSchemeRejected() {
        XCTAssertEqual(ServerAddress.parse("ftp://memex.example"), .err(.scheme))
    }

    func testBadHostname() {
        XCTAssertEqual(ServerAddress.parse("-bad"), .err(.badHost))
        XCTAssertEqual(ServerAddress.parse("host with space"), .err(.badHost))
    }

    func testBadPort() {
        XCTAssertEqual(ServerAddress.parse("host:abc"), .err(.badPort))
        XCTAssertEqual(ServerAddress.parse("host:0"), .err(.badPort))
        XCTAssertEqual(ServerAddress.parse("host:70000"), .err(.badPort))
    }

    func testDisplayForm() {
        XCTAssertEqual(ServerAddress(host: "h", port: 1).display(), "h:1")
        XCTAssertEqual(ServerAddress(host: "2001:db8::1", port: 2).display(), "[2001:db8::1]:2")
    }
}