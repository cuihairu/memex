# Memex 第三方组件登记（持续事项：引入任何第三方组件即登记名称、版本、
# 许可证、引入方式）。纯自研口径：以下均为宽松许可或同类义务明确的组件；
# 无 AGPL/SSPL **链接**进 memex 二进制（OnlyOffice 以独立容器进程运行，
# 进程边界即许可边界，见 AGPL 合规条）。
#
# 格式：名称 | 版本（核验日期）| 许可证 | 引入方式 | 用途 | 备注

## 构建依赖（vcpkg 清单见 vcpkg.json，基线 builtin-baseline 锁版本）

| 名称 | 版本 | 许可证 | 引入方式 | 用途 |
|---|---|---|---|---|
| asio | 1.32.0 | BSL-1.0 | vcpkg（header-only） | 协作服务端网络 I/O |
| nlohmann-json | 3.12.0 | MIT | vcpkg | 组织架构／偏好等 JSON 编解码 |
| protobuf（含 protoc） | 6.33.4 | BSD-3-Clause | vcpkg（运行时＋生成工具） | 线路协议唯一事实源 memex.proto |
| sqlite3 | 3.53.4 | Public Domain | vcpkg | 服务端归档库 |
| openssl（仅 libcrypto） | 3.6.4 | Apache-2.0 | vcpkg | 口令 PBKDF2／指纹 SHA-256 |
| qtbase（含 Widgets/Sql/Network，xcb/glib/xkb 插件） | 6.11.1 | LGPL-3.0（动态链接，未修改） | vcpkg client feature | 桌面客户端 |
| vcpkg 本体 | 基线见 vcpkg.json（builtin-baseline 锁版本） | MIT | 构建工具 | 依赖管理 |

## 音视频／协同文档底座（独立进程部署，保持进程边界）

| 名称 | 版本 | 许可证 | 引入方式 | 用途 |
|---|---|---|---|---|
| livekit-server | v1.8（docker image） | Apache-2.0 | 容器（scripts/deploy/meet-compose.yml） | 音视频会议 SFU＋内嵌 TURN |
| onlyoffice-documentserver | 8.2（docker image） | **AGPL-3.0** | 容器（scripts/deploy/docs-compose.yml） | 协同文档编辑 |
| collabora/code | —（备选，未部署） | MPL-2.0 | — | OnlyOffice 替代方案（若 AGPL 流程成本过高） |
| jitsi（jvb＋jicofo＋meet） | —（备选，未部署） | Apache-2.0 | — | LiveKit 替代方案（SFU 选型备份） |

### AGPL 合规条（OnlyOffice）
1. **未修改**：使用官方 `onlyoffice/documentserver:8.2` 镜像原样运行，
   不打补丁、不二次分发修改版——AGPL 第 13 条的网络交互源码提供义务
   以"未修改官方发行版"为前提履行：部署文档必须给出上游源码地址
   （https://github.com/ONLYOFFICE/DocumentServer）与 exact 镜像 digest。
2. **进程边界**：memex 二进制不链接、不嵌入 OnlyOffice 任何代码；
   仅经 HTTP API（编辑器回调）交互——AGPL 传染止于容器边界。
3. **客户交付**：随交付物提供本登记表＋镜像 digest 获取方式
   （`docker inspect`），满足 AGPL 第 4-6 条 notices 义务。
4. 若法务判定 AGPL 流程不可接受，切换备选 Collabora（MPL-2.0，
   文件级 copyleft，无网络条款），compose 模板届时补入。

## 构建期工具（不进交付物）

| 名称 | 用途 |
|---|---|
| ImageMagick `convert` | client/resources/logo.ico 由正典 logo.svg 机械渲染（见 qrc 注释），仅构建期使用 |
| protoc（随 protobuf） | memex.proto → C++ 生成码（构建树内，不入库） |

## 自研声明
client/、server/、common/ 下全部手工代码为 Memex 自研；
单测断言为手写宏（无 gtest/doctest 等单测框架依赖）。
