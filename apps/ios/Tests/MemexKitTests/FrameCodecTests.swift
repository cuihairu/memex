import XCTest
@testable import MemexKit

/// 帧编解码（对齐 Android FrameCodecTest 8 用例）
final class FrameCodecTests: XCTestCase {

    func testEncodeBigEndianLengthPrefix() throws {
        let frame = try FrameCodec.encode([0x68, 0x69]) // "hi"
        XCTAssertEqual(frame, [0x00, 0x00, 0x00, 0x02, 0x68, 0x69])
    }

    func testWholeFrameSingleFeed() {
        let frame = try! FrameCodec.encode([1, 2, 3])
        let res = FrameCodec.Decoder().feed(frame)
        XCTAssertEqual(res.status, .ok)
        XCTAssertEqual(res.frames, [[1, 2, 3]])
    }

    func testHalfFrameTwoFeeds() {
        let frame = try! FrameCodec.encode([1, 2, 3])
        let d = FrameCodec.Decoder()
        let first = d.feed(Array(frame[0..<2]))
        XCTAssertEqual(first.status, .needMoreData)
        XCTAssertTrue(first.frames.isEmpty)
        let second = d.feed(Array(frame[2...]))
        XCTAssertEqual(second.status, .ok)
        XCTAssertEqual(second.frames, [[1, 2, 3]])
    }

    func testTwoFramesStickyFeed() {
        let a = try! FrameCodec.encode([1])
        let b = try! FrameCodec.encode([2, 2])
        let res = FrameCodec.Decoder().feed(a + b)
        XCTAssertEqual(res.status, .ok)
        XCTAssertEqual(res.frames, [[1], [2, 2]])
    }

    func testByteByByteFeed() {
        let frame = try! FrameCodec.encode(Array(0..<8).map { UInt8($0) })
        let d = FrameCodec.Decoder()
        var out: [[UInt8]] = []
        for byte in frame {
            let res = d.feed([byte])
            out.append(contentsOf: res.frames)
        }
        XCTAssertEqual(out, [Array(0..<8).map { UInt8($0) }])
    }

    func testEmptyPayloadRejected() {
        XCTAssertThrowsError(try FrameCodec.encode([]))
    }

    func testOversizedPayloadRejected() {
        let big = [UInt8](repeating: 0, count: FrameCodec.maxFrameSize + 1)
        XCTAssertThrowsError(try FrameCodec.encode(big))
    }

    func testZeroLengthAndOversizedPrefixRejected() {
        let zero = FrameCodec.Decoder().feed([0, 0, 0, 0])
        XCTAssertEqual(zero.status, .zeroLength)
        let oversized = FrameCodec.Decoder().feed([0x10, 0, 0, 0])
        XCTAssertEqual(oversized.status, .tooLarge)
    }
}