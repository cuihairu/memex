/**
 * 通知分级映射与归档正文（镜像 Android NoticeGradeTest 语义）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { NoticeGrade, fromProtoNumber, composeNoticeText } from '../core/format';

test('分级映射：未指定/未识别按普通', () => {
  assert.equal(fromProtoNumber(0), NoticeGrade.NORMAL);
  assert.equal(fromProtoNumber(1), NoticeGrade.NORMAL);
  assert.equal(fromProtoNumber(2), NoticeGrade.IMPORTANT);
  assert.equal(fromProtoNumber(3), NoticeGrade.URGENT);
  assert.equal(fromProtoNumber(99), NoticeGrade.NORMAL);
});

test('归档正文：全角冒号「标题：正文」', () => {
  assert.equal(composeNoticeText('发布', '新版本', ''), '发布：新版本');
});

test('归档正文：跳转随文留痕', () => {
  assert.equal(composeNoticeText('发布', '新版本', 'https://e.cn/a'), '发布：新版本 https://e.cn/a');
});