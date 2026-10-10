# 鸿蒙（HarmonyOS NEXT）客户端构建与 HAP 打包

T6.2（Qt 桌面矩阵项，2026-10-04 用户令）：Qt 桌面客户端交叉构建到
HarmonyOS NEXT（arm64-v8a）并打 HAP。**状态：代码就绪、待真机验证**
（todo.md 卷首注口径：只写代码不验证，编译/打包/真机三关均未过）。

## 架构口径

- Qt 官方支持（Qt 6.12，technology preview）：`qt_add_executable` 目标平台为
  OHOS 时自动生成 `<target>_make_hap`，经 `harmonydeployqt` 收集客户端 `.so`、
  `libqohos.so` 平台插件与 Qt 模块入包，再调 OpenHarmony `hvigor` 组装 HAP。
  参考：doc.qt.io `harmonyos-building-deploying-apps`、
  `qt-for-harmonyos-cmake-api`（属性名 `QT_HARMONYOS_APP_*` /
  `QT_HARMONYOS_MODULE_*` 实证自该页，2026-10-09）。
- vcpkg：官方 `arm64-ohos` triplet（`VCPKG_ENV_PASSTHROUGH_UNTRACKED
  OHOS_SDK_ROOT`，`CMAKE_SYSTEM_NAME=OHOS`，`OHOS_ARCH=arm64-v8a`）装非 Qt
  依赖（底座 protobuf / nlohmann-json / openssl）；服务端三件套已挪进
  `server` feature 不进鸿蒙树。Qt 本体不走 vcpkg（同 win/mac 口径，避免源码
  重编 Qt）。
- crashpad：上游无 OHOS 支持，`MEMEX_ENABLE_CRASHPAD=OFF` 闸掉
  （`crash_report.cpp` 编译桩兜底，调用点不分支），鸿蒙产物无崩溃采集。
- 手机端规则与桌面一致（R17/R18）：Qt 客户端初始化向导首步强制设服务端
  域名＋连通性校验、无匿名聊天入口——同一代码路径，无平台分支。

## 构建（外部依赖具备后）

```bash
export OHOS_SDK_ROOT=<OpenHarmony SDK 根目录>
export QT_OHOS_ROOT=<Qt for OpenHarmony 安装前缀>   # Qt 6.12+ 鸿蒙目标包
# export QT_HOST_PATH=<同版本桌面 Qt 前缀>            # 交叉 host 工具（如需）
scripts/ohos/build-client-hap.sh
```

或手动：`cmake -S . --preset client-release-ohos` →
`cmake --build build-client-ohos --target memex_client_make_hap`。

Qt for OpenHarmony 预编译包不可得时，按 doc.qt.io `harmonyos-building`
（`configure -ohos-sdk $OHOS_SDK_ROOT -qt-host-path <宿主Qt> -ohos-arch
arm64-v8a`）自编 Qt，再以其安装前缀作 `QT_OHOS_ROOT`。

## 首建核对点（代码-only 交付未验证项）

1. **网络权限**：已在 `client/CMakeLists.txt` 显式声明
   （2026-10-10：`qt_add_harmonyos_permission(memex_client NAME
   ohos.permission.INTERNET)`，OHOS 分支内）——首建后核对生成的
   `module.json5` `requestPermissions`：若与 Qt6::Network 自动贡献
   **重复**则删显式声明二选一收口（Qt 文档口径「所链 Qt 模块默认贡献
   权限」）。
2. **triplet 端口可用性**：vcpkg 对 `arm64-ohos` 的 protobuf/openssl/nlohmann
   端口为 community 支持档，首装可能遇端口内 OHOS 适配缺口（逐个对 vcpkg
   issue 修）。
3. **property 生效性**：`QT_HARMONYOS_APP_*` 为 technology preview，若首建
   报未知属性，以当时版本 CMake API 文档为准微调。
4. **Qt 最低版本**：鸿蒙目标 Qt 需 6.12+（本仓库根 `find_package(Qt6 6.4)`
   不设上界，鸿蒙树由 `QT_OHOS_ROOT` 决定实际版本）。

## 外部依赖卡点（todo.md T6.2 同步登记，仓库内无解，不空等）

- DevEco Studio（hvigor 图形化调试/日志面）
- 应用签名证书（`hap-sign-tool` / DevEco 签名，未签名 HAP 真机不可直装）
- 真机一台（HarmonyOS NEXT；模拟器需 DevEco 内置镜像）
