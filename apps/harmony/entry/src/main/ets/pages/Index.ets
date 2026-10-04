/**
 * 守卫页（唯一 launcher 入口，对齐 Android MainActivity 的 RouteGuard
 * 口径）：未初始化 → 向导；未登录 → 登录页；两者齐备 → 主界面。
 * 无任何匿名/免登录分支（R18）。
 */
import router from '@ohos.router';
import { Route, RouteGuard } from '@memex/core';
import { appState } from '../common/AppState';

@Entry
@Component
struct Index {
  aboutToAppear(): void {
    this.resolve();
  }

  private async resolve(): Promise<void> {
    try {
      await appState.ready();
    } catch (e) {
      // 初始化态载入失败视同未初始化（重新走向导会重新载入）
    }
    const route = RouteGuard.next(appState.initStore, appState.isLoggedIn);
    if (route === Route.MAIN) {
      router.replaceUrl({ url: 'pages/MainPage' });
    } else if (route === Route.LOGIN) {
      router.replaceUrl({ url: 'pages/LoginPage' });
    } else {
      router.replaceUrl({ url: 'pages/InitPage' });
    }
  }

  build() {
    Column() {
      LoadingProgress()
        .width(48)
        .height(48)
      Text('Memex')
        .fontSize(24)
        .fontWeight(FontWeight.Bold)
        .margin({ top: 16 })
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
  }
}
