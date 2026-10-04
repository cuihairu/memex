/**
 * 唯一 UIAbility 入口（对齐 Android 单一 MainActivity launcher 口径）：
 * onCreate 绑定 AppState 上下文并预载初始化态；窗口内容固定为守卫页
 * pages/Index（onShow 经 RouteGuard 分发到向导/登录/主界面）。
 */
import UIAbility from '@ohos.app.ability.UIAbility';
import AbilityConstant from '@ohos.app.ability.AbilityConstant';
import Want from '@ohos.app.ability.Want';
import window from '@ohos.window';
import { BusinessError } from '@ohos.base';
import { appState } from '../common/AppState';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    appState.bindContext(this.context);
    appState.foreground = false;
    appState.openPeer = '';
  }

  onDestroy(): void {
    // 进程级状态随实例释放；初始化态已落盘，登录态按 R18 不持久化
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err: BusinessError) => {
      // 加载失败无法恢复 UI（守卫页为最小静态布局）
    });
    appState.ready().catch((_e: Error) => {
      // 预载失败由守卫页 resolve 时再行处理（此处仅提前预热）
    });
  }

  onWindowStageDestroy(): void {
    // 无常驻窗口资源
  }

  onForeground(): void {
    appState.foreground = true;
  }

  onBackground(): void {
    appState.foreground = false;
    appState.openPeer = '';
  }
}
