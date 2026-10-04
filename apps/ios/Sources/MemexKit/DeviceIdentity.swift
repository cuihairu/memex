import Foundation
#if canImport(CommonCrypto)
import CommonCrypto
#endif

/// 设备指纹：SHA-256 hex（服务端设备台账以此为键，T2.1/T3.3）。
public enum DeviceIdentity {

    public static func sha256Hex(_ data: [UInt8]) -> String {
        #if canImport(CommonCrypto)
        var digest = [UInt8](repeating: 0, count: Int(CC_SHA256_DIGEST_LENGTH))
        data.withUnsafeBytes { buf in
            _ = CC_SHA256(buf.baseAddress, CC_LONG(data.count), &digest)
        }
        return digest.map { String(format: "%02x", $0) }.joined()
        #else
        // 非 Apple 平台（Linux swift）：无 CommonCrypto，走 sha256sum 子进程
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/sha256sum")
        process.arguments = []
        let input = Pipe()
        let output = Pipe()
        process.standardInput = input
        process.standardOutput = output
        guard (try? process.run()) != nil else { return "" }
        input.fileHandleForWriting.write(Data(data))
        try? input.fileHandleForWriting.close()
        let text = String(data: output.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        return text
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .split(separator: " ").first.map(String.init) ?? ""
        #endif
    }

    /// 由稳定种子推导指纹（iOS 侧种子＝identifierForVendor＋机型，见 App 层组装）
    public static func fingerprint(_ seed: String) -> String {
        sha256Hex(Array(seed.utf8))
    }
}