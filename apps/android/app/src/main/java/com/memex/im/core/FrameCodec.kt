package com.memex.im.core

/**
 * Memex 协议帧编解码（镜像 common/src/frame.cpp 的语义，线格式完全一致）：
 * 帧 = 4 字节大端长度前缀 + 载荷（此处载荷为 memex.proto Envelope 的序列化字节）。
 * 长度前缀只描述载荷长度，不含自身 4 字节。
 */
object FrameCodec {
    const val LENGTH_PREFIX_SIZE = 4
    const val MAX_FRAME_SIZE = 4 * 1024 * 1024

    /** 协议层错误：畸形输入必须报错返回，不得崩溃。 */
    class ProtocolViolation(message: String) : RuntimeException(message)

    /** 编码一帧：空载荷或超限抛 [ProtocolViolation]。 */
    fun encode(payload: ByteArray): ByteArray {
        if (payload.isEmpty()) throw ProtocolViolation("空载荷：帧长度必须大于 0")
        if (payload.size > MAX_FRAME_SIZE) throw ProtocolViolation("载荷超限：超过 MAX_FRAME_SIZE")
        val frame = ByteArray(LENGTH_PREFIX_SIZE + payload.size)
        val len = payload.size
        frame[0] = ((len ushr 24) and 0xFF).toByte()
        frame[1] = ((len ushr 16) and 0xFF).toByte()
        frame[2] = ((len ushr 8) and 0xFF).toByte()
        frame[3] = (len and 0xFF).toByte()
        payload.copyInto(frame, LENGTH_PREFIX_SIZE)
        return frame
    }

    enum class DecodeStatus { OK, NEED_MORE_DATA, ZERO_LENGTH, TOO_LARGE }

    data class FeedResult(val status: DecodeStatus, val frames: List<ByteArray>)

    /** 流式解码器：容忍字节流任意切分（TCP 粘包／半包）。 */
    class Decoder {
        private var buffer = ByteArray(0)

        /** 追加字节；解出的完整帧随结果返回。 */
        fun feed(bytes: ByteArray): FeedResult {
            if (bytes.isNotEmpty()) {
                val merged = ByteArray(buffer.size + bytes.size)
                buffer.copyInto(merged)
                bytes.copyInto(merged, buffer.size)
                buffer = merged
            }

            val out = ArrayList<ByteArray>()
            var offset = 0
            while (true) {
                if (buffer.size - offset < LENGTH_PREFIX_SIZE) break
                val len = readU32Be(buffer, offset)
                if (len == 0L) return FeedResult(DecodeStatus.ZERO_LENGTH, out)
                if (len > MAX_FRAME_SIZE) return FeedResult(DecodeStatus.TOO_LARGE, out)
                if (buffer.size - offset - LENGTH_PREFIX_SIZE < len) break // 载荷不完整
                out.add(
                    buffer.copyOfRange(offset + LENGTH_PREFIX_SIZE, offset + LENGTH_PREFIX_SIZE + len.toInt())
                )
                offset += LENGTH_PREFIX_SIZE + len.toInt()
            }
            if (offset > 0) buffer = buffer.copyOfRange(offset, buffer.size)
            return FeedResult(
                if (out.isEmpty()) DecodeStatus.NEED_MORE_DATA else DecodeStatus.OK,
                out
            )
        }

        fun reset() {
            buffer = ByteArray(0)
        }

        private fun readU32Be(b: ByteArray, at: Int): Long =
            ((b[at].toLong() and 0xFF) shl 24) or
                ((b[at + 1].toLong() and 0xFF) shl 16) or
                ((b[at + 2].toLong() and 0xFF) shl 8) or
                (b[at + 3].toLong() and 0xFF)
    }
}
