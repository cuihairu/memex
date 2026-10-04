package com.memex.im.core

import com.memex.im.core.FrameCodec.DecodeStatus
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

/** 帧编解码语义对齐 common/src/frame.cpp：4 字节大端长度前缀 + 载荷 */
class FrameCodecTest {

    @Test
    fun `编码＝大端长度前缀加载荷`() {
        val frame = FrameCodec.encode("abc".toByteArray(Charsets.US_ASCII))
        assertArrayEquals(byteArrayOf(0, 0, 0, 3, 'a'.code.toByte(), 'b'.code.toByte(), 'c'.code.toByte()), frame)
    }

    @Test
    fun `空载荷与超限载荷拒绝`() {
        assertThrows(FrameCodec.ProtocolViolation::class.java) {
            FrameCodec.encode(ByteArray(0))
        }
        assertThrows(FrameCodec.ProtocolViolation::class.java) {
            FrameCodec.encode(ByteArray(FrameCodec.MAX_FRAME_SIZE + 1))
        }
        // 恰好等于上限允许（边界含）
        FrameCodec.encode(ByteArray(FrameCodec.MAX_FRAME_SIZE))
    }

    @Test
    fun `整帧一次喂入`() {
        val frame = FrameCodec.encode("hello".toByteArray())
        val res = FrameCodec.Decoder().feed(frame)
        assertEquals(DecodeStatus.OK, res.status)
        assertEquals(1, res.frames.size)
        assertArrayEquals("hello".toByteArray(), res.frames[0])
    }

    @Test
    fun `半包两次喂入`() {
        val frame = FrameCodec.encode("hello".toByteArray())
        val d = FrameCodec.Decoder()
        val first = d.feed(frame.copyOfRange(0, 2))
        assertEquals(DecodeStatus.NEED_MORE_DATA, first.status)
        val second = d.feed(frame.copyOfRange(2, frame.size))
        assertEquals(DecodeStatus.OK, second.status)
        assertArrayEquals("hello".toByteArray(), second.frames[0])
    }

    @Test
    fun `粘包两帧一次喂入`() {
        val f1 = FrameCodec.encode("one".toByteArray())
        val f2 = FrameCodec.encode("two-two".toByteArray())
        val res = FrameCodec.Decoder().feed(f1 + f2)
        assertEquals(DecodeStatus.OK, res.status)
        assertEquals(2, res.frames.size)
        assertArrayEquals("one".toByteArray(), res.frames[0])
        assertArrayEquals("two-two".toByteArray(), res.frames[1])
    }

    @Test
    fun `零长度帧报非法`() {
        val res = FrameCodec.Decoder().feed(byteArrayOf(0, 0, 0, 0))
        assertEquals(DecodeStatus.ZERO_LENGTH, res.status)
    }

    @Test
    fun `超限帧报非法`() {
        val res = FrameCodec.Decoder().feed(byteArrayOf(0x7F, 0xFF.toByte(), 0xFF.toByte(), 0xFF.toByte()))
        assertEquals(DecodeStatus.TOO_LARGE, res.status)
    }

    @Test
    fun `逐字节喂入也能拼出整帧`() {
        val frame = FrameCodec.encode("piecewise".toByteArray())
        val d = FrameCodec.Decoder()
        var last = FrameCodec.FeedResult(DecodeStatus.NEED_MORE_DATA, emptyList())
        for (b in frame) {
            last = d.feed(byteArrayOf(b))
        }
        assertEquals(DecodeStatus.OK, last.status)
        assertArrayEquals("piecewise".toByteArray(), last.frames.single())
    }
}
