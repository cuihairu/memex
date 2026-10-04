/**
 * R17 初始化 + R18 免登录禁入的路由守卫测试（镜像 Android RouteGuardTest
 * 语义：唯一去向、向导不可绕过登录、重设同样须过校验）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { InitStore, Route, RouteGuard, InitEntry, InitGate } from '../core/route';

class MemoryInitStore implements InitStore {
  private addr: { host: string; port: number } | null = null;

  isInitialized(): boolean {
    return this.addr !== null;
  }

  serverAddress(): { host: string; port: number } | null {
    return this.addr;
  }

  markInitialized(address: { host: string; port: number }): void {
    this.addr = address;
  }
}

test('未初始化：无论登录与否只能去 INIT', () => {
  const store = new MemoryInitStore();
  assert.equal(RouteGuard.next(store, false), Route.INIT);
  assert.equal(RouteGuard.next(store, true), Route.INIT); // 理论不可达：未初始化不可能已登录
});

test('已初始化未登录：只能去 LOGIN（R18 无匿名入口）', () => {
  const store = new MemoryInitStore();
  store.markInitialized({ host: 'memex.local', port: 24360 });
  assert.equal(RouteGuard.next(store, false), Route.LOGIN);
});

test('两者齐备：MAIN', () => {
  const store = new MemoryInitStore();
  store.markInitialized({ host: 'memex.local', port: 24360 });
  assert.equal(RouteGuard.next(store, true), Route.MAIN);
});

test('常规进入向导页一律改道登录（向导不可作为回头路）', () => {
  const store = new MemoryInitStore();
  store.markInitialized({ host: 'memex.local', port: 24360 });
  assert.equal(InitGate.entry(store, false), InitEntry.TO_LOGIN);
});

test('仅登录页「修改服务器地址」重设才出示表单', () => {
  const store = new MemoryInitStore();
  store.markInitialized({ host: 'memex.local', port: 24360 });
  assert.equal(InitGate.entry(store, true), InitEntry.SHOW_FORM);
});

test('未初始化时进向导页出示表单', () => {
  const store = new MemoryInitStore();
  assert.equal(InitGate.entry(store, false), InitEntry.SHOW_FORM);
});

test('校验通过并保存后必到 LOGIN；未过校验停留 INIT', () => {
  const saved = new MemoryInitStore();
  saved.markInitialized({ host: 'a', port: 24360 });
  assert.equal(InitGate.afterSaved(saved), Route.LOGIN);
  assert.equal(InitGate.afterSaved(new MemoryInitStore()), Route.INIT);
});