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
import { WANT_KEY_ACTION, WANT_KEY_MSG_ID, WANT_ACTION_CONFIRM } from '../common/NoticeManager';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    appState.bindContext(this.context);
    appState.foreground = false;
    appState.openPeer = '';
    this.dispatchWant(want);
  }

  /**
   * 横幅 wantAgent 热启动解参（代码-only·待真机验证）：紧急通知「确认收悉」
   * 动作按钮经 wantAgent 启本 ability，已活着时走 onNewWant——直接确认收悉
   * （撤常驻横幅＋弹队列下一条，与冷启动 onCreate 同一出口）。
   */
  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    this.dispatchWant(want);
  }

  /** 紧急横幅 want 参数裁决：confirm_urgent＝确认收悉；其余（本体拉起）不处理。 */
  private dispatchWant(want: Want): void {
    const params = want.parameters;
    if (params === undefined) {
      return;
    }
    const action = params[WANT_KEY_ACTION];
    if (typeof action !== 'string' || action !== WANT_ACTION_CONFIRM) {
      return;
    }
    const msgId = params[WANT_KEY_MSG_ID];
    if (typeof msgId !== 'string' || msgId.length === 0) {
      return;
    }
    appState.notice.confirmUrgent(msgId);
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
