/**
 * 服务器地址解析测试（镜像 Android ServerAddressTest 语义）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ServerAddress, DEFAULT_PORT, PORT_MAX } from '../core/address';

function ok(raw: string, host: string, port: number): void {
  const r = ServerAddress.parse(raw);
  assert.equal(r.ok, true, '期望解析成功：' + raw);
  if (r.ok) {
    assert.equal(r.address.host, host);
    assert.equal(r.address.port, port);
  }
}

function err(raw: string, reason?: string): void {
  const r = ServerAddress.parse(raw);
  assert.equal(r.ok, false, '期望解析失败：' + raw);
  if (!r.ok && reason !== undefined) {
    assert.equal(r.reason, reason, raw + ' 的失败原因');
  }
}

test('合法形态：host 缺省端口', () => {
  ok('memex.local', 'memex.local', DEFAULT_PORT);
  ok('192.168.1.10', '192.168.1.10', DEFAULT_PORT);
});

test('合法形态：host:port', () => {
  ok('memex.local:24360', 'memex.local', 24360);
  ok('192.168.1.10:1', '192.168.1.10', 1);
  ok('192.168.1.10:65535', '192.168.1.10', 65535);
});

test('合法形态：[IPv6]:port 与裸 IPv6 加括号', () => {
  ok('[::1]:24360', '::1', 24360);
  ok('[2001:db8::1]:9000', '2001:db8::1', 9000);
  ok('[fe80::1]', 'fe80::1', DEFAULT_PORT);
});

test('合法形态：http:// 前缀剥除', () => {
  ok('http://memex.local:24360/', 'memex.local', 24360);
  ok('http://192.168.1.10', '192.168.1.10', DEFAULT_PORT);
  ok('http://memex.local:24360/setup', 'memex.local', 24360);
  ok('http://memex.local:24360?x=1', 'memex.local', 24360);
});

test('非法：空输入', () => {
  err('', 'EMPTY');
  err('   ', 'EMPTY');
});

test('非法：坏主机名', () => {
  err('-leading', 'BAD_HOST');
  err('.dot-start', 'BAD_HOST');
  err('a b', 'BAD_HOST');
});

test('合法边界：首尾下划线与连续点（正则允许，对齐 Android）', () => {
  ok('host_', 'host_', DEFAULT_PORT);
  ok('a..b', 'a..b', DEFAULT_PORT);
  ok('_inner_', '_inner_', DEFAULT_PORT);
});

test('非法：裸 IPv6 未包方括号', () => {
  err('::1', 'IPV6_FORM');
  err('fe80::1:8080', 'IPV6_FORM');
});

test('非法：方括号形态错误', () => {
  err('[]:8080', 'IPV6_FORM');
  err('[abc]:', 'IPV6_FORM');
  err('[abc]junk', 'IPV6_FORM');
});

test('非法：端口越界/非数字', () => {
  err('host:0', 'BAD_PORT');
  err(`host:${PORT_MAX + 1}`, 'BAD_PORT');
  err('host:abc', 'BAD_PORT');
  err('host:', 'BAD_PORT');
  err('host:12a', 'BAD_PORT');
});

test('非法：非 http scheme 明确拒绝', () => {
  err('ftp://host:21', 'SCHEME');
  err('file:///etc/passwd', 'SCHEME');
});

test('display 形态：IPv6 回括号、普通 host:port', () => {
  const v6 = ServerAddress.parse('[::1]:24360');
  assert.equal(v6.ok, true);
  assert.equal(v6.ok ? v6.address.display() : '', '[::1]:24360');
  const plain = ServerAddress.parse('memex.local:9');
  assert.equal(plain.ok, true);
  assert.equal(plain.ok ? plain.address.display() : '', 'memex.local:9');
});