/**
 * R17 初始化与 R18 免登录禁入的路由门禁（镜像 apps/android InitStore.kt
 * 的 RouteGuard/InitGate 语义，纯逻辑可单测）。初始化状态存取接口化，
 * UI 侧注入偏好存储实现（鸿蒙壳：@ohos.data.preferences）。
 */

export interface InitStore {
  /** 是否已完成「服务器地址设置 + 连通性校验通过」。 */
  isInitialized(): boolean;
  /** 已初始化时返回地址；未初始化返回 null。 */
  serverAddress(): { host: string; port: number } | null;
  /** 校验通过后落盘（UI 层保证只在探测成功后调用）。 */
  markInitialized(address: { host: string; port: number }): void;
}

/** 应用三个可达界面；守卫输出唯一去向。 */
export enum Route {
  INIT = 'INIT',
  LOGIN = 'LOGIN',
  MAIN = 'MAIN',
}

/**
 * 路由守卫（R17/R18 的门禁核心）：
 * - 未完成初始化 → 只能去向初始化向导；
 * - 已初始化未登录 → 只能去向登录页（移动端无免登录/匿名形态）；
 * - 两者齐备 → 主界面。
 */
export class RouteGuard {
  static next(store: InitStore, isLoggedIn: boolean): Route {
    if (!store.isInitialized()) return Route.INIT;
    if (!isLoggedIn) return Route.LOGIN;
    return Route.MAIN;
  }
}

export enum InitEntry {
  SHOW_FORM = 'SHOW_FORM',
  TO_LOGIN = 'TO_LOGIN',
}

export class InitGate {
  /**
   * 已初始化的常规进入一律改道登录页（向导不可作为回头路绕过登录）；
   * 仅登录页「修改服务器地址」显式要求重设时（reconfigure）再出示表单，
   * 且重设同样必须通过连通性校验才能保存。
   */
  static entry(store: InitStore, reconfigure: boolean): InitEntry {
    if (store.isInitialized() && !reconfigure) return InitEntry.TO_LOGIN;
    return InitEntry.SHOW_FORM;
  }

  /** 校验通过并保存后的去向：必然是登录页。 */
  static afterSaved(store: InitStore): Route {
    return store.isInitialized() ? Route.LOGIN : Route.INIT;
  }
}