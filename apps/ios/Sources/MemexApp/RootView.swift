import SwiftUI
import MemexKit

/// 唯一入口（对齐 Android RouteGuard）：经守卫路由到初始化向导/登录页/主界面；
/// 全应用无免登录或匿名入口（R17/R18，移动端只有协作态）。
struct RootView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        Group {
            switch model.entry {
            case .setup:
                InitView()
            case .login:
                LoginView()
            case .main:
                ConversationListView()
            }
        }
        .onAppear { model.refresh() }
        .alert(
            "已退出登录",
            isPresented: Binding(
                get: { model.kickMessage != nil },
                set: { if !$0 { model.kickMessage = nil } }
            )
        ) {
            Button("好", role: .cancel) { model.kickMessage = nil }
        } message: {
            Text(model.kickMessage ?? "")
        }
    }
}

struct RootView_Previews: PreviewProvider {
    static var previews: some View {
        RootView().environmentObject(AppModel())
    }
}