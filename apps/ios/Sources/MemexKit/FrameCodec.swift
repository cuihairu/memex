import Foundation

/// Memex 协议帧编解码（镜像 common/src/frame.cpp 的语义，线格式完全一致）：
/// 帧 = 4 字节大端长度前缀 + 载荷（此处载荷为 memex.proto Envelope 的序列化字节）。
/// 长度前缀只描述载荷长度，不含自身 4 字节。
public enum FrameCodec {
    public static let lengthPrefixSize = 4
    public static let maxFrameSize = 4 * 1024 * 1024

    /// 协议层错误：畸形输入必须报错返回，不得崩溃。
    public struct ProtocolViolation: Error, CustomStringConvertible {
        public let message: String
        public init(_ message: String) { self.message = message }
        public var description: String { message }
    }

    /// 编码一帧：空载荷或超限抛 ProtocolViolation。
    public static func encode(_ payload: [UInt8]) throws -> [UInt8] {
        if payload.isEmpty { throw ProtocolViolation("空载荷：帧长度必须大于 0") }
        if payload.count > maxFrameSize { throw ProtocolViolation("载荷超限：超过 MAX_FRAME_SIZE") }
        var frame = [UInt8](repeating: 0, count: lengthPrefixSize + payload.count)
        let len = payload.count
        frame[0] = UInt8((len >> 24) & 0xFF)
        frame[1] = UInt8((len >> 16) & 0xFF)
        frame[2] = UInt8((len >> 8) & 0xFF)
        frame[3] = UInt8(len & 0xFF)
        frame.replaceSubrange(lengthPrefixSize..<frame.count, with: payload)
        return frame
    }

    public enum DecodeStatus {
        case ok, needMoreData, zeroLength, tooLarge
    }

    public struct FeedResult {
        public let status: DecodeStatus
        public let frames: [[UInt8]]
    }

    /// 流式解码器：容忍字节流任意切分（TCP 粘包／半包）。
    public final class Decoder {
        private var buffer: [UInt8] = []

        public init() {}

        /// 追加字节；解出的完整帧随结果返回。
        public func feed(_ bytes: [UInt8]) -> FeedResult {
            if !bytes.isEmpty {
                buffer.append(contentsOf: bytes)
            }
            var out: [[UInt8]] = []
            var offset = 0
            while true {
                if buffer.count - offset < FrameCodec.lengthPrefixSize { break }
                let len = readU32BE(buffer, at: offset)
                if len == 0 { return FeedResult(status: .zeroLength, frames: out) }
                if len > FrameCodec.maxFrameSize { return FeedResult(status: .tooLarge, frames: out) }
                if buffer.count - offset - FrameCodec.lengthPrefixSize < len { break } // 载荷不完整
                let start = offset + FrameCodec.lengthPrefixSize
                out.append(Array(buffer[start..<(start + len)]))
                offset += FrameCodec.lengthPrefixSize + len
            }
            if offset > 0 { buffer.removeFirst(offset) }
            return FeedResult(status: out.isEmpty ? .needMoreData : .ok, frames: out)
        }

        public func reset() {
            buffer = []
        }

        private func readU32BE(_ b: [UInt8], at: Int) -> Int {
            (Int(b[at]) << 24) | (Int(b[at + 1]) << 16) | (Int(b[at + 2]) << 8) | Int(b[at + 3])
        }
    }
}