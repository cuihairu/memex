/**
 * FrameCodec 测试（对齐 common/src/frame.cpp 语义：4 字节大端长度前缀＋
 * 载荷；镜像 Android FrameCodecTest 的边界用例）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  encodeFrame,
  FrameDecoder,
  DecodeStatus,
  LENGTH_PREFIX_SIZE,
  MAX_FRAME_SIZE,
} from '../core/frame_codec';
import { ProtocolViolation } from '../core/wire';

test('编码：4 字节大端长度前缀 + 载荷', () => {
  const payload = new Uint8Array([1, 2, 3, 4, 5]);
  const frame = encodeFrame(payload);
  assert.equal(frame.length, LENGTH_PREFIX_SIZE + 5);
  assert.deepEqual([...frame.slice(0, 4)], [0, 0, 0, 5]);
  assert.deepEqual([...frame.slice(4)], [1, 2, 3, 4, 5]);
});

test('编码：超过 256 字节的载荷前缀进位', () => {
  const payload = new Uint8Array(300).fill(0xab);
  const frame = encodeFrame(payload);
  assert.deepEqual([...frame.slice(0, 4)], [0, 0, 1, 44]);
  assert.equal(frame.length, LENGTH_PREFIX_SIZE + 300);
});

test('编码：空载荷抛 ProtocolViolation', () => {
  assert.throws(() => encodeFrame(new Uint8Array(0)), ProtocolViolation);
});

test('编码：超限载荷抛 ProtocolViolation', () => {
  assert.throws(() => encodeFrame(new Uint8Array(MAX_FRAME_SIZE + 1)), ProtocolViolation);
});

test('解码：单帧 OK', () => {
  const d = new FrameDecoder();
  const res = d.feed(encodeFrame(new Uint8Array([7, 8])));
  assert.equal(res.status, DecodeStatus.OK);
  assert.equal(res.frames.length, 1);
  assert.deepEqual([...res.frames[0]], [7, 8]);
});

test('解码：粘包一次出多帧', () => {
  const d = new FrameDecoder();
  const a = encodeFrame(new Uint8Array([1]));
  const b = encodeFrame(new Uint8Array([2, 3]));
  const merged = new Uint8Array(a.length + b.length);
  merged.set(a);
  merged.set(b, a.length);
  const res = d.feed(merged);
  assert.equal(res.status, DecodeStatus.OK);
  assert.equal(res.frames.length, 2);
  assert.deepEqual([...res.frames[0]], [1]);
  assert.deepEqual([...res.frames[1]], [2, 3]);
});

test('解码：半包受容忍（分两次喂）', () => {
  const d = new FrameDecoder();
  const frame = encodeFrame(new Uint8Array([9, 9, 9]));
  const first = d.feed(frame.slice(0, 3));
  assert.equal(first.status, DecodeStatus.NEED_MORE_DATA);
  assert.equal(first.frames.length, 0);
  const rest = d.feed(frame.slice(3));
  assert.equal(rest.status, DecodeStatus.OK);
  assert.equal(rest.frames.length, 1);
  assert.deepEqual([...rest.frames[0]], [9, 9, 9]);
});

test('解码：长度前缀只有一半仍 NEED_MORE_DATA', () => {
  const d = new FrameDecoder();
  const res = d.feed(new Uint8Array([0, 0]));
  assert.equal(res.status, DecodeStatus.NEED_MORE_DATA);
  assert.equal(res.frames.length, 0);
});

test('解码：零长度前缀报 ZERO_LENGTH', () => {
  const d = new FrameDecoder();
  const res = d.feed(new Uint8Array([0, 0, 0, 0, 1]));
  assert.equal(res.status, DecodeStatus.ZERO_LENGTH);
});

test('解码：超长前缀报 TOO_LARGE', () => {
  const d = new FrameDecoder();
  // 0x40000000 = 1GiB ≫ MAX_FRAME_SIZE（4MiB）
  assert.equal(d.feed(new Uint8Array([0x40, 0, 0, 0])).status, DecodeStatus.TOO_LARGE);
  // 2MiB 合法但载荷不完整 → NEED_MORE_DATA
  const d2 = new FrameDecoder();
  assert.equal(d2.feed(new Uint8Array([0, 0x20, 0, 0])).status, DecodeStatus.NEED_MORE_DATA);
});

test('reset 清空积压', () => {
  const d = new FrameDecoder();
  d.feed(new Uint8Array([0, 0, 0, 5, 1])); // 缺 4 字节载荷
  d.reset();
  const res = d.feed(encodeFrame(new Uint8Array([1])));
  assert.equal(res.status, DecodeStatus.OK);
});

test('乱序喂入：先零长度后正常帧报错不吞', () => {
  const d = new FrameDecoder();
  const r = d.feed(new Uint8Array([0, 0, 0, 0]));
  assert.ok(r.status === DecodeStatus.ZERO_LENGTH);
});